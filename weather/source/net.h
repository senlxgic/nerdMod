/*
	Network layer of the Weather app (BlocksDS DSWiFi + Mbed TLS).

	Wi-Fi: DSWiFi in DSi mode (WIFI_ATTEMPT_DSI_MODE), connected through the access point stored in the console's firmware
	settings (Wifi_AutoConnect). HTTPS: Mbed TLS with certificate verification REQUIRED against the root list in cacerts.h,
	hostname check, and a clock check first (a wrong DSi date makes every certificate look expired). Nothing here ever
	disables verification.

	Every wait loop calls the Tick callback once per frame; the callback draws progress and returns false when the user
	cancels. All waits have a time limit, so nothing can hang. The only call that cannot be interrupted is the DNS lookup
	(bounded by the lwIP resolver, about 10 seconds).
*/
#pragma once

#include <stddef.h>
#include <string>

namespace net {

typedef bool (*Tick)(void *ctx, const char *status); // false = cancel

enum Err {
	E_OK = 0,
	E_CANCEL,
	E_WIFI_INIT,	// the Wi-Fi library did not start
	E_NO_AP,		// no access point configured / could not associate
	E_TIMEOUT,
	E_CLOCK,		// the console clock is not set
	E_DNS,
	E_TCP,
	E_TLS,			// handshake failed (not a certificate problem)
	E_CERT,			// certificate refused
	E_HTTP,			// the server answered with an error or garbage
	E_TOO_BIG
};

struct Info {
	char ip[20] = "";		// console address
	char serverIp[20] = "";
	char tls[12] = "";		// "TLSv1.2"
	char cipher[48] = "";
	char verify[96] = "";	// why a certificate was refused
	int httpStatus = 0;
	int bytes = 0;
	int caCount = 0;		// root certificates loaded
	int caSkipped = 0;		// roots that could not be parsed
	int tlsError = 0;		// raw Mbed TLS code
};

// Connects (or confirms) Wi-Fi. E_OK when associated and an address was obtained.
Err wifiUp(Tick tick, void *ctx, Info &info);
void wifiDown();
bool wifiIsUp();

// HTTPS GET of url (https:// only). The body (at most maxBody bytes) is returned on E_OK.
Err httpsGet(const char *url, size_t maxBody, std::string &body, Tick tick, void *ctx, Info &info);

// Short text for the status line (fits 48 characters) and a longer explanation for the Network Test page.
const char *shortText(Err e, const Info &info, char *buf, size_t n);
const char *longText(Err e);

} // namespace net
