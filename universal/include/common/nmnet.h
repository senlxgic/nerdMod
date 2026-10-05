/*
	nerdMod networking core: a non-blocking plain-HTTP/1.1 GET client over an abstract Transport.

	Why a Transport: the Wi-Fi stack is a platform thing (see docs/WEATHER.md - the Weather app uses DSWiFi in DSi mode, the menu has none
	this toolchain), while everything above it - URL handling, request text, response parsing (Content-Length, chunked,
	read-until-close), size caps, timeout, cancellation - is plain code that can be tested on a PC with a mock Transport.
	A platform provides one Transport; until then `UnavailableTransport` reports "no network" and callers fall back to
	their cache. There is no TLS here and none is faked: https:// URLs are refused.

	All calls return immediately. poll() is meant to be called once per frame with a millisecond clock.
*/
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

namespace nmnet {

// ---- URL -------------------------------------------------------------------------------------------------
struct Url {
	bool ok = false;
	bool tls = false;
	std::string host, path;
	int port = 80;
};

// allowTls: accept "https://" (port 443); the transport must then really do TLS with certificate checks. Default: refused.
inline Url parseUrl(const char *u, bool allowTls = false) {
	Url r;
	if (!u)
		return r;
	const char *p = u;
	if (strncmp(p, "http://", 7) == 0)
		p += 7;
	else if (strncmp(p, "https://", 8) == 0) {
		r.tls = true;
		if (!allowTls)
			return r; // refused: this transport has no TLS
		r.port = 443;
		p += 8;
	} else
		return r;
	const char *hostEnd = p;
	while (*hostEnd && *hostEnd != '/' && *hostEnd != ':')
		hostEnd++;
	if (hostEnd == p || hostEnd - p > 120)
		return r;
	r.host.assign(p, hostEnd - p);
	for (char c : r.host)
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
			return r;
	p = hostEnd;
	if (*p == ':') {
		p++;
		int port = 0, digits = 0;
		while (*p >= '0' && *p <= '9' && digits < 6) {
			port = port * 10 + (*p - '0');
			p++;
			digits++;
		}
		if (digits == 0 || port <= 0 || port > 65535)
			return r;
		r.port = port;
	}
	if (*p == 0)
		r.path = "/";
	else if (*p == '/')
		r.path = p;
	else
		return r;
	for (char c : r.path)
		if (c <= ' ' || c == 0x7F) // no spaces / control characters (no header injection)
			return r;
	if (r.path.size() > 400)
		return r;
	r.ok = true;
	return r;
}

inline std::string buildGet(const Url &u) {
	std::string s = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host;
	if (u.port != (u.tls ? 443 : 80))
		s += ":" + std::to_string(u.port);
	s += "\r\nUser-Agent: nerdMod/1\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
	return s;
}

// ---- response parser ----------------------------------------------------------------------------------------
class ResponseParser {
  public:
	enum State { HEADERS, BODY_LENGTH, BODY_CHUNK_SIZE, BODY_CHUNK_DATA, BODY_CHUNK_CRLF, BODY_UNTIL_CLOSE, DONE, ERROR };
	static constexpr size_t MAX_HEADER = 2048;
	static constexpr size_t MAX_BODY = 4096; // default cap; setMaxBody() raises it (the Weather app reads up to 12 KB)
	void setMaxBody(size_t n) { maxBody = n; }

	State state() const { return st; }
	int status() const { return code; }
	const std::string &body() const { return bodyText; }
	bool done() const { return st == DONE; }
	bool failed() const { return st == ERROR; }

	// Feeds received bytes. Returns false once the response is complete or invalid.
	bool feed(const char *data, size_t n) {
		for (size_t i = 0; i < n && st != DONE && st != ERROR; i++)
			step(data[i]);
		return st != DONE && st != ERROR;
	}
	// The peer closed the connection.
	void closed() {
		if (st == BODY_UNTIL_CLOSE)
			st = DONE;
		else if (st != DONE)
			st = ERROR; // cut off in the middle
	}

  private:
	void fail() { st = ERROR; }
	void addBody(char c) {
		if (bodyText.size() >= maxBody) {
			fail();
			return;
		}
		bodyText.push_back(c);
	}
	static int hexv(char c) {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	}
	void headersDone() {
		// status line
		size_t sp = head.find(' ');
		if (head.compare(0, 5, "HTTP/") != 0 || sp == std::string::npos || head.size() < sp + 4) {
			fail();
			return;
		}
		code = atoi(head.c_str() + sp + 1);
		if (code < 100 || code > 599) {
			fail();
			return;
		}
		contentLength = -1;
		chunked = false;
		size_t pos = head.find("\r\n");
		while (pos != std::string::npos && pos + 2 < head.size()) {
			size_t next = head.find("\r\n", pos + 2);
			std::string line = head.substr(pos + 2, (next == std::string::npos ? head.size() : next) - pos - 2);
			for (char &c : line)
				if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
			if (line.compare(0, 15, "content-length:") == 0)
				contentLength = atol(line.c_str() + 15);
			else if (line.compare(0, 18, "transfer-encoding:") == 0 && line.find("chunked") != std::string::npos)
				chunked = true;
			pos = next;
		}
		if (code == 204 || code == 304 || (code >= 100 && code < 200))
			st = DONE;
		else if (chunked)
			st = BODY_CHUNK_SIZE;
		else if (contentLength == 0)
			st = DONE;
		else if (contentLength > 0) {
			if ((size_t)contentLength > maxBody) {
				fail();
				return;
			}
			remaining = contentLength;
			st = BODY_LENGTH;
		} else
			st = BODY_UNTIL_CLOSE;
	}
	void step(char c) {
		switch (st) {
			case HEADERS:
				head.push_back(c);
				if (head.size() > MAX_HEADER) {
					fail();
					break;
				}
				if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0)
					headersDone();
				break;
			case BODY_LENGTH:
				addBody(c);
				if (st == BODY_LENGTH && --remaining == 0)
					st = DONE;
				break;
			case BODY_UNTIL_CLOSE:
				addBody(c);
				break;
			case BODY_CHUNK_SIZE:
				if (c == '\n') {
					if (!sizeSeen) {
						fail();
						break;
					}
					sizeSeen = false;
					if (chunkLeft == 0)
						st = DONE; // last chunk (trailers are ignored)
					else
						st = BODY_CHUNK_DATA;
				} else if (c == '\r' || c == ';') {
					ext = ext || c == ';';
				} else if (!ext) {
					const int h = hexv(c);
					if (h < 0 || chunkLeft > 0x10000) {
						fail();
						break;
					}
					chunkLeft = chunkLeft * 16 + (size_t)h;
					sizeSeen = true;
				}
				if (st == BODY_CHUNK_DATA || st == DONE)
					ext = false;
				break;
			case BODY_CHUNK_DATA:
				addBody(c);
				if (st == BODY_CHUNK_DATA && --chunkLeft == 0)
					st = BODY_CHUNK_CRLF;
				break;
			case BODY_CHUNK_CRLF:
				if (c == '\n') {
					st = BODY_CHUNK_SIZE;
					chunkLeft = 0;
				} else if (c != '\r')
					fail();
				break;
			default:
				break;
		}
	}

	State st = HEADERS;
	std::string head, bodyText;
	size_t maxBody = MAX_BODY;
	int code = 0;
	long contentLength = -1, remaining = 0;
	bool chunked = false, sizeSeen = false, ext = false;
	size_t chunkLeft = 0;
};

// ---- transport + request ----------------------------------------------------------------------------------
class Transport {
  public:
	enum Poll { PENDING, READY, FAILED };
	virtual ~Transport() {}
	virtual bool available(const char **reason) = 0;			// false: no usable network here
	virtual bool connectStart(const std::string &host, int port) = 0;
	virtual Poll connectPoll() = 0;
	// Non-blocking: returns bytes sent (0 = try later), or -1 on error.
	virtual int send(const char *data, size_t n) = 0;
	// Non-blocking: returns bytes received (0 = nothing yet), -1 = closed by peer, -2 = error.
	virtual int recv(char *buf, size_t n) = 0;
	virtual void close() = 0;
};

class UnavailableTransport : public Transport {
  public:
	bool available(const char **reason) override {
		if (reason)
			*reason = "No Wi-Fi driver for DSi mode in this build";
		return false;
	}
	bool connectStart(const std::string &, int) override { return false; }
	Poll connectPoll() override { return FAILED; }
	int send(const char *, size_t) override { return -1; }
	int recv(char *, size_t) override { return -2; }
	void close() override {}
};

class Request {
  public:
	enum Status { IDLE, CONNECTING, SENDING, RECEIVING, DONE, FAILED, TIMEOUT, CANCELLED, UNAVAILABLE, BAD_URL, HTTP_ERROR };

	explicit Request(Transport &t) : tr(t) {}

	// Starts a GET. Returns immediately; the outcome is read from status() after poll() calls.
	void start(const char *url, uint32_t nowMs, uint32_t timeoutMs) {
		reset();
		const Url u = parseUrl(url);
		if (!u.ok) {
			st = BAD_URL;
			return;
		}
		const char *reason = nullptr;
		if (!tr.available(&reason)) {
			st = UNAVAILABLE;
			return;
		}
		out = buildGet(u);
		deadline = nowMs + timeoutMs;
		if (!tr.connectStart(u.host, u.port)) {
			st = FAILED;
			return;
		}
		st = CONNECTING;
	}

	void cancel() {
		if (st == CONNECTING || st == SENDING || st == RECEIVING) {
			tr.close();
			st = CANCELLED;
		}
	}

	// One step of work (a few small transport calls). Safe to call in any state.
	Status poll(uint32_t nowMs) {
		if (st != CONNECTING && st != SENDING && st != RECEIVING)
			return st;
		if ((int32_t)(nowMs - deadline) >= 0) {
			tr.close();
			st = TIMEOUT;
			return st;
		}
		if (st == CONNECTING) {
			const Transport::Poll p = tr.connectPoll();
			if (p == Transport::FAILED) {
				tr.close();
				st = FAILED;
			} else if (p == Transport::READY)
				st = SENDING;
		}
		if (st == SENDING) {
			const int n = tr.send(out.data() + sent, out.size() - sent);
			if (n < 0) {
				tr.close();
				st = FAILED;
			} else {
				sent += (size_t)n;
				if (sent >= out.size())
					st = RECEIVING;
			}
		} else if (st == RECEIVING) {
			char buf[256];
			const int n = tr.recv(buf, sizeof buf);
			if (n > 0) {
				parser.feed(buf, (size_t)n);
			} else if (n == -1) {
				parser.closed();
			} else if (n == -2) {
				tr.close();
				st = FAILED;
				return st;
			}
			if (parser.done() || parser.failed() || n == -1) {
				tr.close();
				if (parser.done())
					st = (parser.status() >= 200 && parser.status() < 300) ? DONE : HTTP_ERROR;
				else
					st = FAILED;
			}
		}
		return st;
	}

	Status status() const { return st; }
	int httpStatus() const { return parser.status(); }
	const std::string &body() const { return parser.body(); }
	bool finished() const { return st != IDLE && st != CONNECTING && st != SENDING && st != RECEIVING; }

  private:
	void reset() {
		st = IDLE;
		sent = 0;
		out.clear();
		parser = ResponseParser();
	}
	Transport &tr;
	Status st = IDLE;
	std::string out;
	size_t sent = 0;
	uint32_t deadline = 0;
	ResponseParser parser;
};

} // namespace nmnet
