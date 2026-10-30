#pragma once

// HTTP GET for Launcher_Core and the Updater (design.md, "Launcher_Core interfaces"). The GTK layer
// implements HttpClient with libsoup (task 19.x); tests use KytyTests::FakeHttpClient. Core code
// never performs network I/O itself.
//
// Contract (every implementation keeps it, the libsoup client included):
// - Get() returns a RequestId (never 0) and never calls ChunkFn or DoneFn before it returns.
// - ChunkFn and DoneFn run on the main thread. ChunkFn receives the body in order, in chunks of
//   any size. DoneFn runs exactly once per request, after the last ChunkFn call; neither runs
//   after DoneFn.
// - A ChunkFn that returns false ends the request: no further ChunkFn call, and DoneFn runs once
//   with HttpFailure::Cancelled.
// - Cancel(id) of a pending request makes DoneFn run once with HttpFailure::Cancelled, later,
//   never inside Cancel() itself. Cancel() of a finished or unknown request, or of 0, does
//   nothing.
// - The limits of HttpRequest end the request with TooLarge (more than max_bytes body bytes),
//   Stalled (no body byte for stall_timeout), or TimedOut (total_timeout since Get()). A zero
//   limit is no limit.
// - Redirects (301, 302, 303, 307, 308) are followed to their Location, at most 10 per request,
//   and only to https URLs; with RedirectPolicy::SameOrigin only to the scheme, host, and port of
//   the requested URL as well (the Qt launcher's SameOriginRedirectPolicy for the cheat
//   collection). A redirect that the policy refuses, or one too many, ends the request with
//   HttpFailure::Network and a message that names the refused URL.
// - Status: a response arrived with a status outside 200-299; HttpResult::status holds it.
//   Network: no response (DNS, connection, TLS, or transfer error); HttpResult::message
//   describes it.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace kyty::core {

using RequestId = uint64_t;

// Which redirects a request follows (see the contract above): HttpsOnly follows a redirect to any
// https URL (GitHub sends release downloads to another host); SameOrigin only to the same scheme,
// host, and port.
enum class RedirectPolicy { HttpsOnly, SameOrigin };

struct HttpRequest {
	std::string                                      url;
	std::vector<std::pair<std::string, std::string>> headers;
	uint64_t                                         max_bytes = 0;
	std::chrono::milliseconds                        stall_timeout {0};
	std::chrono::milliseconds                        total_timeout {0};
	RedirectPolicy                                   redirects = RedirectPolicy::HttpsOnly;
};

enum class HttpFailure { None, Network, Status, TooLarge, Stalled, TimedOut, Cancelled };

struct HttpResult {
	HttpFailure failure = HttpFailure::None;
	int         status  = 0; // HTTP status code when a response arrived, else 0
	std::string message;     // empty on success
};

class HttpClient {
public:
	using ChunkFn = std::function<bool(std::span<const std::byte>)>; // return false to abort
	using DoneFn  = std::function<void(const HttpResult&)>;

	virtual ~HttpClient() = default;

	virtual RequestId Get(const HttpRequest& request, ChunkFn on_chunk, DoneFn on_done) = 0;
	virtual void      Cancel(RequestId id)                                              = 0;
};

} // namespace kyty::core
