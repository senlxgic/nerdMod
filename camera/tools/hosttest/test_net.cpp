#include <stdio.h>
#include <string>
#include <vector>
#include "../../../universal/include/common/nmnet.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
using namespace nmnet;

// A scripted transport: connects after `connectTicks`, then delivers `reply` in `slice`-byte pieces, one piece per recv.
struct Mock : Transport {
	bool up = true; int connectTicks = 0; std::string reply; size_t slice = 7; bool closeAfter = true; bool failSend = false; bool failRecv = false;
	std::string sent; size_t pos = 0; int ticks = 0; bool closed = false;
	bool available(const char **r) override { if (r) *r = "mock"; return up; }
	bool connectStart(const std::string &, int) override { ticks = 0; return true; }
	Poll connectPoll() override { return ++ticks > connectTicks ? READY : PENDING; }
	int send(const char *d, size_t n) override { if (failSend) return -1; size_t k = n < 5 ? n : 5; sent.append(d, k); return (int)k; }
	int recv(char *b, size_t n) override {
		if (failRecv) return -2;
		if (pos >= reply.size()) return closeAfter ? -1 : 0;
		size_t k = reply.size() - pos; if (k > slice) k = slice; if (k > n) k = n;
		memcpy(b, reply.data() + pos, k); pos += k; return (int)k;
	}
	void close() override { closed = true; }
};

static Request::Status run(Request &r, uint32_t &t, int maxSteps = 5000) {
	for (int i = 0; i < maxSteps && !r.finished(); i++) r.poll(t += 16);
	return r.status();
}

int main() {
	// ---- URLs
	CHECK(parseUrl("http://example.com/w?x=1").ok);
	Url u = parseUrl("http://relay.local:8080/a/b"); CHECK(u.ok && u.host == "relay.local" && u.port == 8080 && u.path == "/a/b");
	u = parseUrl("http://host"); CHECK(u.ok && u.path == "/");
	CHECK(parseUrl("https://example.com/").tls && !parseUrl("https://example.com/").ok);
	CHECK(!parseUrl("ftp://x/").ok && !parseUrl("").ok && !parseUrl(nullptr).ok && !parseUrl("http://").ok);
	CHECK(!parseUrl("http://ho st/").ok && !parseUrl("http://h:99999/").ok && !parseUrl("http://h:/").ok);
	CHECK(!parseUrl("http://h/a b").ok && !parseUrl("http://h/a\r\nHost: evil").ok);
	CHECK(buildGet(parseUrl("http://a.b:81/x")).find("Host: a.b:81\r\n") != std::string::npos);
	// ---- parser
	{ ResponseParser p; std::string s = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"; p.feed(s.data(), s.size()); CHECK(p.done() && p.status() == 200 && p.body() == "hello"); }
	{ ResponseParser p; std::string s = "HTTP/1.1 200 OK\r\ncontent-length: 10\r\n\r\nhello"; p.feed(s.data(), s.size()); CHECK(!p.done()); p.closed(); CHECK(p.failed()); } // truncated
	{ ResponseParser p; std::string s = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\n\r\n"; p.feed(s.data(), s.size()); CHECK(p.done() && p.body() == "hello world"); }
	{ ResponseParser p; std::string s = "HTTP/1.1 200 OK\r\n\r\nuntil close"; p.feed(s.data(), s.size()); CHECK(!p.done()); p.closed(); CHECK(p.done() && p.body() == "until close"); }
	{ ResponseParser p; std::string s = "garbage\r\n\r\n"; p.feed(s.data(), s.size()); CHECK(p.failed()); }
	{ ResponseParser p; std::string s = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n"; p.feed(s.data(), s.size()); CHECK(p.failed()); }
	{ ResponseParser p; std::string s = "HTTP/1.1 200 OK\r\nContent-Length: 999999\r\n\r\n"; p.feed(s.data(), s.size()); CHECK(p.failed()); } // too big
	{ ResponseParser p; std::string s(5000, 'a'); std::string h = "HTTP/1.1 200 OK\r\nX: " + s; p.feed(h.data(), h.size()); CHECK(p.failed()); } // header flood
	{ ResponseParser p; std::string s = "HTTP/1.1 204 No Content\r\n\r\n"; p.feed(s.data(), s.size()); CHECK(p.done() && p.body().empty()); }
	{ ResponseParser p; std::string s = "HTTP/1.1 200 OK\r\n\r\n"; p.feed(s.data(), s.size()); std::string big(5000, 'x'); p.feed(big.data(), big.size()); CHECK(p.failed()); }
	// byte-at-a-time feeding gives the same result
	{ ResponseParser p; std::string s = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"; for (char c : s) p.feed(&c, 1); CHECK(p.done() && p.body() == "abc"); }
	// ---- requests
	{ Mock m; m.reply = "HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\n{\"a\":\"bcde\"}"; m.connectTicks = 3; Request r(m); uint32_t t = 0;
	  r.start("http://h/x", t, 5000); CHECK(r.status() == Request::CONNECTING); CHECK(run(r, t) == Request::DONE && r.body() == "{\"a\":\"bcde\"}" && m.sent.find("GET /x HTTP/1.1") == 0 && m.closed); }
	{ Mock m; m.reply = "HTTP/1.1 404 Not Found\r\nContent-Length: 2\r\n\r\nno"; Request r(m); uint32_t t = 0; r.start("http://h/x", t, 5000); CHECK(run(r, t) == Request::HTTP_ERROR && r.httpStatus() == 404); }
	{ Mock m; m.connectTicks = 100000; Request r(m); uint32_t t = 1000; r.start("http://h/x", t, 500); CHECK(run(r, t) == Request::TIMEOUT && m.closed); }          // never connects
	{ Mock m; m.reply = "HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nabc"; m.closeAfter = false; Request r(m); uint32_t t = 0; r.start("http://h/x", t, 800); CHECK(run(r, t) == Request::TIMEOUT); } // stalls
	{ Mock m; m.reply = "HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nabc"; Request r(m); uint32_t t = 0; r.start("http://h/x", t, 5000); CHECK(run(r, t) == Request::FAILED); } // cut off
	{ Mock m; m.reply = "x"; m.closeAfter = false; Request r(m); uint32_t t = 0; r.start("http://h/x", t, 5000); r.poll(t += 16); r.cancel(); CHECK(r.status() == Request::CANCELLED && m.closed); r.poll(t += 16); CHECK(r.status() == Request::CANCELLED); }
	{ Mock m; m.up = false; Request r(m); uint32_t t = 0; r.start("http://h/x", t, 5000); CHECK(r.status() == Request::UNAVAILABLE && r.finished()); }
	{ Mock m; Request r(m); uint32_t t = 0; r.start("https://h/x", t, 5000); CHECK(r.status() == Request::BAD_URL); }
	{ Mock m; m.failSend = true; Request r(m); uint32_t t = 0; r.start("http://h/x", t, 5000); CHECK(run(r, t) == Request::FAILED); }
	{ Mock m; m.failRecv = true; Request r(m); uint32_t t = 0; r.start("http://h/x", t, 5000); CHECK(run(r, t) == Request::FAILED); }
	{ UnavailableTransport ut; Request r(ut); uint32_t t = 0; r.start("http://h/x", t, 5000); CHECK(r.status() == Request::UNAVAILABLE); }
	// clock wrap-around does not time out early
	{ Mock m; m.reply = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx"; Request r(m); uint32_t t = 0xFFFFFF00u; r.start("http://h/x", t, 5000); CHECK(run(r, t) == Request::DONE); }
	// Phase 2D: TLS URLs are accepted only when asked for; port 443 is the default; the response cap can be raised
	{
		Url u = parseUrl("https://api.open-meteo.com/v1/forecast?x=1", true);
		CHECK(u.ok && u.tls && u.port == 443 && u.host == "api.open-meteo.com" && u.path == "/v1/forecast?x=1");
		CHECK(buildGet(u).find("Host: api.open-meteo.com\r\n") != std::string::npos);
		CHECK(parseUrl("https://h:8443/", true).port == 8443 && buildGet(parseUrl("https://h:8443/", true)).find("Host: h:8443") != std::string::npos);
		CHECK(!parseUrl("https://h/", false).ok && !parseUrl("ftp://h/", true).ok);
		ResponseParser rp; rp.setMaxBody(20000);
		std::string big = "HTTP/1.1 200 OK\r\nContent-Length: 9000\r\n\r\n" + std::string(9000, 'x');
		rp.feed(big.data(), big.size());
		CHECK(rp.done() && rp.body().size() == 9000);
		ResponseParser small; small.feed(big.data(), big.size());
		CHECK(small.failed());
	}
	printf(failures ? "%d FAILURES\n" : "net tests OK\n", failures);
	return failures;
}
