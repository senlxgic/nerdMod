#include "net.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <dswifi9.h>
#include <nds.h>

#include <mbedtls/build_info.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "cacerts.h"
#include "common/nmnet.h"
#include "common/nmweather.h"

namespace net {

namespace {

// DSWiFi does not export inet_ntoa; s_addr is in network byte order, so the first octet is the low byte.
void dotted(char *out, size_t cap, uint32_t addr) {
	snprintf(out, cap, "%u.%u.%u.%u", (unsigned)(addr & 255), (unsigned)((addr >> 8) & 255), (unsigned)((addr >> 16) & 255), (unsigned)(addr >> 24));
}


constexpr int FPS = 60;
constexpr int WIFI_TIMEOUT_FRAMES = 25 * FPS;
constexpr int CONNECT_TIMEOUT_FRAMES = 12 * FPS;
constexpr int HANDSHAKE_TIMEOUT_FRAMES = 40 * FPS;
constexpr int READ_TIMEOUT_FRAMES = 25 * FPS;

bool wifiInited = false;
bool wifiConnected = false;

bool step(Tick tick, void *ctx, const char *status) {
	if (!tick) {
		cothread_yield_irq(IRQ_VBLANK);
		return true;
	}
	return tick(ctx, status); // draws, polls the keys and waits one frame
}

// The root list is parsed once and kept.
mbedtls_x509_crt caChain;
bool caLoaded = false;
int caSkipped = 0;

bool loadRoots() {
	if (caLoaded)
		return true;
	mbedtls_x509_crt_init(&caChain);
	const int r = mbedtls_x509_crt_parse(&caChain, (const unsigned char *)CA_BUNDLE_PEM, sizeof(CA_BUNDLE_PEM)); // length includes the NUL
	if (r < 0)
		return false;
	caSkipped = r;
	caLoaded = true;
	return true;
}

// Mbed TLS state of one connection, released in the destructor on every path.
struct Conn {
	mbedtls_net_context net;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	struct addrinfo *addr = nullptr;
	bool tlsUp = false;
	Conn() {
		mbedtls_net_init(&net);
		mbedtls_ssl_init(&ssl);
		mbedtls_ssl_config_init(&conf);
		mbedtls_entropy_init(&entropy);
		mbedtls_ctr_drbg_init(&drbg);
	}
	~Conn() {
		if (tlsUp)
			mbedtls_ssl_close_notify(&ssl);
		mbedtls_net_free(&net);
		mbedtls_ssl_free(&ssl);
		mbedtls_ssl_config_free(&conf);
		mbedtls_ctr_drbg_free(&drbg);
		mbedtls_entropy_free(&entropy);
		if (addr)
			freeaddrinfo(addr);
	}
};

void describeVerify(uint32_t flags, Info &info) {
	if (flags & MBEDTLS_X509_BADCERT_EXPIRED)
		snprintf(info.verify, sizeof info.verify, "certificate expired (is the DSi clock right?)");
	else if (flags & MBEDTLS_X509_BADCERT_FUTURE)
		snprintf(info.verify, sizeof info.verify, "certificate not valid yet (DSi clock too early?)");
	else if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
		snprintf(info.verify, sizeof info.verify, "certificate chain not trusted (unknown root)");
	else if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
		snprintf(info.verify, sizeof info.verify, "certificate is for another host name");
	else
		mbedtls_x509_crt_verify_info(info.verify, sizeof info.verify, "", flags);
}

} // namespace

bool wifiIsUp() { return wifiConnected && Wifi_AssocStatus() == ASSOCSTATUS_ASSOCIATED; }

Err wifiUp(Tick tick, void *ctx, Info &info) {
	if (!wifiInited) {
		if (!step(tick, ctx, "Starting Wi-Fi (DSi mode)"))
			return E_CANCEL;
		if (!Wifi_InitDefault(INIT_ONLY | WIFI_ATTEMPT_DSI_MODE))
			return E_WIFI_INIT;
		wifiInited = true;
	}
	int st = Wifi_AssocStatus();
	if (st == ASSOCSTATUS_DISCONNECTED || st == ASSOCSTATUS_CANNOTCONNECT)
		Wifi_AutoConnect();
	int frames = 0;
	for (; frames < WIFI_TIMEOUT_FRAMES; frames++) {
		st = Wifi_AssocStatus();
		if (st == ASSOCSTATUS_ASSOCIATED)
			break;
		if (st == ASSOCSTATUS_CANNOTCONNECT) {
			wifiConnected = false;
			return E_NO_AP;
		}
		const char *text = st == ASSOCSTATUS_SEARCHING ? "Looking for your access point" : st == ASSOCSTATUS_AUTHENTICATING ? "Logging in to Wi-Fi" :
						   st == ASSOCSTATUS_ASSOCIATING ? "Joining Wi-Fi" : st == ASSOCSTATUS_ACQUIRINGDHCP ? "Getting an IP address" : "Connecting to Wi-Fi";
		if (!step(tick, ctx, text))
			return E_CANCEL;
	}
	if (st != ASSOCSTATUS_ASSOCIATED) {
		wifiConnected = false;
		return E_TIMEOUT;
	}
	wifiConnected = true;
	struct in_addr a;
	a.s_addr = Wifi_GetIP();
	dotted(info.ip, sizeof info.ip, a.s_addr);
	return E_OK;
}

void wifiDown() {
	if (!wifiInited)
		return;
	Wifi_DisconnectAP();
	Wifi_DisableWifi();
	wifiConnected = false;
}

Err httpsGet(const char *url, size_t maxBody, std::string &body, Tick tick, void *ctx, Info &info) {
	body.clear();
	const nmnet::Url u = nmnet::parseUrl(url, true);
	if (!u.ok || !u.tls)
		return E_HTTP;
	if (!nmweather::clockLooksSet((int64_t)time(NULL)))
		return E_CLOCK; // every certificate would look "not valid yet"
	if (!loadRoots())
		return E_TLS;
	info.caCount = (int)CA_BUNDLE_COUNT - caSkipped;
	info.caSkipped = caSkipped;

	Conn c;

	// ---- DNS (cannot be interrupted; bounded by the resolver)
	if (!step(tick, ctx, "Finding the weather server"))
		return E_CANCEL;
	struct addrinfo hints;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	char port[8];
	snprintf(port, sizeof port, "%d", u.port);
	if (getaddrinfo(u.host.c_str(), port, &hints, &c.addr) != 0 || !c.addr)
		return E_DNS;
	{
		struct sockaddr_in *sin = (struct sockaddr_in *)c.addr->ai_addr;
		dotted(info.serverIp, sizeof info.serverIp, sin->sin_addr.s_addr);
	}

	// ---- TCP, non-blocking so that the user can cancel and the time limit holds
	const int fd = socket(c.addr->ai_family, c.addr->ai_socktype, c.addr->ai_protocol);
	if (fd < 0)
		return E_TCP;
	c.net.fd = fd;
	if (mbedtls_net_set_nonblock(&c.net) != 0)
		return E_TCP;
	{
		bool connected = false;
		for (int frames = 0; frames < CONNECT_TIMEOUT_FRAMES && !connected; frames++) {
			if (!step(tick, ctx, "Connecting to the weather server"))
				return E_CANCEL;
			const int r = connect(fd, c.addr->ai_addr, c.addr->ai_addrlen);
			if (r == 0 || errno == EISCONN)
				connected = true;
			else if (errno != EINPROGRESS && errno != EALREADY && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				return E_TCP;
		}
		if (!connected)
			return E_TIMEOUT;
	}

	// ---- TLS
	static const char *pers = "nerdmod_weather";
	if (mbedtls_ctr_drbg_seed(&c.drbg, mbedtls_entropy_func, &c.entropy, (const unsigned char *)pers, strlen(pers)) != 0)
		return E_TLS;
	if (mbedtls_ssl_config_defaults(&c.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
		return E_TLS;
	mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_REQUIRED); // never OPTIONAL / NONE
	mbedtls_ssl_conf_ca_chain(&c.conf, &caChain, nullptr);
	mbedtls_ssl_conf_rng(&c.conf, mbedtls_ctr_drbg_random, &c.drbg);
	if (mbedtls_ssl_setup(&c.ssl, &c.conf) != 0)
		return E_TLS;
	if (mbedtls_ssl_set_hostname(&c.ssl, u.host.c_str()) != 0) // the name the certificate must match (also SNI)
		return E_TLS;
	mbedtls_ssl_set_bio(&c.ssl, &c.net, mbedtls_net_send, mbedtls_net_recv, nullptr);

	for (int frames = 0;; frames++) {
		const int r = mbedtls_ssl_handshake(&c.ssl);
		if (r == 0)
			break;
		if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
			info.tlsError = r;
			const uint32_t flags = mbedtls_ssl_get_verify_result(&c.ssl);
			if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || (flags != 0 && flags != 0xFFFFFFFFu)) {
				describeVerify(flags, info);
				return E_CERT;
			}
			return E_TLS;
		}
		if (frames >= HANDSHAKE_TIMEOUT_FRAMES)
			return E_TIMEOUT;
		if (!step(tick, ctx, "Secure handshake"))
			return E_CANCEL;
	}
	c.tlsUp = true;
	snprintf(info.tls, sizeof info.tls, "%s", mbedtls_ssl_get_version(&c.ssl));
	snprintf(info.cipher, sizeof info.cipher, "%s", mbedtls_ssl_get_ciphersuite(&c.ssl));

	// ---- request
	const std::string req = nmnet::buildGet(u);
	{
		size_t sent = 0;
		for (int frames = 0; sent < req.size(); frames++) {
			const int r = mbedtls_ssl_write(&c.ssl, (const unsigned char *)req.data() + sent, req.size() - sent);
			if (r > 0)
				sent += (size_t)r;
			else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
				return E_TLS;
			if (frames > READ_TIMEOUT_FRAMES)
				return E_TIMEOUT;
			if (sent < req.size() && !step(tick, ctx, "Sending request"))
				return E_CANCEL;
		}
	}

	// ---- response
	nmnet::ResponseParser parser;
	parser.setMaxBody(maxBody);
	int idle = 0;
	while (!parser.done() && !parser.failed()) {
		unsigned char buf[1024];
		const int r = mbedtls_ssl_read(&c.ssl, buf, sizeof buf);
		if (r > 0) {
			idle = 0;
			info.bytes += r;
			parser.feed((const char *)buf, (size_t)r);
			continue;
		}
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
			if (++idle > READ_TIMEOUT_FRAMES)
				return E_TIMEOUT;
			if (!step(tick, ctx, "Receiving the forecast"))
				return E_CANCEL;
			continue;
		}
		// closed (close_notify, EOF) or a read error: the parser decides whether what arrived is complete
		if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == 0)
			c.tlsUp = false; // the server already closed: no close_notify of ours
		parser.closed();
		break;
	}
	info.httpStatus = parser.status();
	if (parser.failed())
		return info.bytes > 0 && parser.body().size() >= maxBody ? E_TOO_BIG : E_HTTP;
	if (!parser.done() || parser.status() != 200)
		return E_HTTP;
	body = parser.body();
	return E_OK;
}

const char *shortText(Err e, const Info &info, char *buf, size_t n) {
	switch (e) {
		case E_OK: return "OK";
		case E_CANCEL: return "Cancelled";
		case E_WIFI_INIT: return "Wi-Fi could not start";
		case E_NO_AP: return "No Wi-Fi: set up an access point in DSi settings";
		case E_TIMEOUT: return "Timed out - check your connection";
		case E_CLOCK: return "DSi clock not set - fix date in DSi settings";
		case E_DNS: return "Server not found (DNS) - no internet?";
		case E_TCP: return "Could not connect to the server";
		case E_TLS: return "Secure connection failed";
		case E_CERT: return "Server certificate not trusted (check clock)";
		case E_TOO_BIG: return "Server answer too large";
		case E_HTTP:
			if (info.httpStatus > 0 && info.httpStatus != 200) {
				snprintf(buf, n, "Server error %d - try later", info.httpStatus);
				return buf;
			}
			return "Unexpected answer from server";
	}
	return "Error";
}

const char *longText(Err e) {
	switch (e) {
		case E_OK: return "All checks passed.";
		case E_CANCEL: return "Cancelled.";
		case E_WIFI_INIT: return "The Wi-Fi hardware did not start. Restart the console.";
		case E_NO_AP: return "No access point: configure Wi-Fi in DSi System Settings > Internet (WPA2 works).";
		case E_TIMEOUT: return "Timed out. Move closer to the router and retry.";
		case E_CLOCK: return "The console clock is not set. Fix it in DSi System Settings.";
		case E_DNS: return "Wi-Fi is up but the name lookup failed: the router has no internet?";
		case E_TCP: return "The server did not accept the connection.";
		case E_TLS: return "The secure handshake failed.";
		case E_CERT: return "The certificate was refused (clock or unknown root).";
		case E_TOO_BIG: return "The answer was larger than the app accepts.";
		case E_HTTP: return "The server did not answer with weather data.";
	}
	return "Error";
}

} // namespace net
