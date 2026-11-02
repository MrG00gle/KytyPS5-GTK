#pragma once

// core::HttpClient over libsoup 3 (task 19.4; plan D19, HC-07, HC-12). It keeps the contract of
// launcher/core/httpClient.h:
// - One SoupSession without a User-Agent of its own: a request sends exactly the headers it lists.
// - Get() queues the request on an idle source, so neither ChunkFn nor DoneFn runs before Get()
//   returns. Ids start at 1 and are never 0.
// - Only https URLs are requested ("Only https URLs are allowed: <url>" ends the request with
//   HttpFailure::Network otherwise).
// - Redirects (301, 302, 303, 307, 308) are followed by hand (SOUP_MESSAGE_NO_REDIRECT), at most
//   kMaxRedirects per request, with the Location header resolved against the redirecting URL
//   (g_uri_parse_relative): only to https URLs, and with RedirectPolicy::SameOrigin only to the
//   scheme, host, and port of the requested URL. A refused redirect, an eleventh one, or one
//   without a usable Location ends the request with HttpFailure::Network and a message that names
//   the URL; HttpResult::status then holds the redirect's status.
// - A 2xx response is read in chunks of at most kChunkSize bytes (g_input_stream_read_bytes_async)
//   and passed to ChunkFn in order; DoneFn follows the last chunk with HttpFailure::None. Any other
//   status ends the request with HttpFailure::Status without reading the body.
// - Limits: stall_timeout runs from Get() and restarts at every body chunk (Stalled), total_timeout
//   runs from Get() (TimedOut), and more than max_bytes body bytes end the request before the chunk
//   that would exceed the limit is delivered (TooLarge). A zero limit is no limit.
// - A ChunkFn that returns false ends the request with HttpFailure::Cancelled; Cancel(id) of a
//   pending request delivers one HttpFailure::Cancelled from an idle source, never inside Cancel().
// - The destructor cancels every pending request without calling any callback. ChunkFn and DoneFn
//   may call Get() and Cancel() and may destroy the client.

#include "launcher/core/httpClient.h"
#include "launcher/gtk/gobjectPtr.h"

#include <cstddef>
#include <libsoup/soup.h>
#include <map>
#include <memory>
#include <string>

namespace kyty::gtk {

class SoupHttpClient final: public core::HttpClient {
public:
	struct Options {
		// Tests only: also allow http:// URLs (and redirects to them) whose host is 127.0.0.1, ::1,
		// or localhost, so that a loopback SoupServer can answer. The launcher never sets it.
		bool allow_loopback_http = false;
		// Tests only (phase 7 plan D12): the certificate authorities the session trusts instead of
		// the system's, so that an https request can reach a loopback SoupServer with a test
		// certificate. The session keeps its own reference. The launcher never sets it.
		GTlsDatabase* tls_database = nullptr;
	};

	static constexpr int         kMaxRedirects = 10;
	static constexpr std::size_t kChunkSize    = 64 * 1024;

	SoupHttpClient();
	explicit SoupHttpClient(Options options);
	~SoupHttpClient() override;

	SoupHttpClient(const SoupHttpClient&)            = delete;
	SoupHttpClient& operator=(const SoupHttpClient&) = delete;

	core::RequestId Get(const core::HttpRequest& request, ChunkFn on_chunk,
	                    DoneFn on_done) override;
	void            Cancel(core::RequestId id) override;

	// Requests that have not ended yet.
	[[nodiscard]] std::size_t ActiveCount() const { return m_requests.size(); }

private:
	struct Request;
	using RequestRef = std::shared_ptr<Request>;

	// The data of one pending GIO operation keeps its request alive until the operation ends.
	static gpointer   Hold(const RequestRef& request);
	static RequestRef Take(gpointer data);

	static gboolean OnStart(gpointer data);
	static gboolean OnStallTimeout(gpointer data);
	static gboolean OnTotalTimeout(gpointer data);
	static gboolean OnCancelIdle(gpointer data);
	static void     OnSent(GObject* source, GAsyncResult* result, gpointer data);
	static void     OnRead(GObject* source, GAsyncResult* result, gpointer data);

	[[nodiscard]] bool Allowed(GUri* uri) const;
	void               ArmStallTimer(Request& request);
	void               Begin(const RequestRef& request);
	void               Send(const RequestRef& request);
	void               OnResponse(const RequestRef& request, GObjectPtr<GInputStream> stream,
	                              const GErrorPtr& error);
	void               OnRedirect(const RequestRef& request, guint status);
	void               Read(const RequestRef& request);
	void               OnChunk(const RequestRef& request, GBytes* bytes, const GErrorPtr& error);
	void               Finish(core::RequestId id, core::HttpFailure failure, std::string message);

	Options                               m_options;
	GObjectPtr<SoupSession>               m_session;
	core::RequestId                       m_next_id = 1;
	std::map<core::RequestId, RequestRef> m_requests;
};

} // namespace kyty::gtk
