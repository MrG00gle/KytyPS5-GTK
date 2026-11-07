#pragma once

// HTTP for the UI tests without a network (phase 7 plan D12): a SoupServer in the test process on
// 127.0.0.1 that answers from scripted responses, and a core::HttpClient that sends the launcher's
// https requests there through the real SoupHttpClient (allow_loopback_http), so the libsoup
// adapter runs in the UI tests. Requests are recorded on both sides. Main thread only; both use
// the default main context.

#include "launcher/core/httpClient.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/services/soupHttpClient.h"

#include <cstddef>
#include <libsoup/soup.h>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace KytyTests {

// What the server answers for one path.
struct LoopbackResponse {
	unsigned    status = 200;
	std::string body;
	std::string content_type = "application/json";
	// Extra response headers, for example a Location of a redirect: LoopbackHttpClient sends an
	// https target to this server too, and any target that is neither https nor this server's
	// ends the request (HttpFailure::Network, "No URL mapping for <url>").
	std::vector<std::pair<std::string, std::string>> headers;
	// The response starts this many milliseconds after the request arrived.
	unsigned delay_ms = 0;
	// 0: the body in one piece; else chunks of this many bytes, chunk_interval_ms apart (the first
	// one with the headers). With stall_after_chunks > 0 the body stops after that many chunks and
	// never completes.
	std::size_t chunk_size         = 0;
	unsigned    chunk_interval_ms  = 0;
	int         stall_after_chunks = 0;
};

// A request as one side saw it.
struct RecordedRequest {
	std::string                                      url;  // the client: as asked, https://...
	std::string                                      path; // the server: /<host>/<path>[?query]
	std::string                                      method;
	std::vector<std::pair<std::string, std::string>> headers;
};

class LoopbackServer {
public:
	LoopbackServer();
	~LoopbackServer();

	LoopbackServer(const LoopbackServer&)            = delete;
	LoopbackServer& operator=(const LoopbackServer&) = delete;

	[[nodiscard]] int Port() const { return m_port; }

	// The loopback URL that serves url: https://<host>/<path> becomes
	// http://127.0.0.1:<port>/<host>/<path> (query kept); any other URL stays as it is.
	[[nodiscard]] std::string Rewrite(std::string_view url) const;

	// Every later request of url (an https URL, compared with its query) gets response. A path
	// without a script answers 404.
	void Script(std::string_view url, LoopbackResponse response);

	// The requests that reached the server, in arrival order, and how many asked for url.
	[[nodiscard]] const std::vector<RecordedRequest>& Requests() const { return m_requests; }
	[[nodiscard]] int                                 Hits(std::string_view url) const;

private:
	struct Pending;

	static void     OnRequest(SoupServer* server, SoupServerMessage* message, const char* path,
	                          GHashTable* query, gpointer data);
	static gboolean OnDelay(gpointer data);
	static gboolean OnChunkTimer(gpointer data);
	static void     OnFinished(SoupServerMessage* message, gpointer data);

	static std::string PathOf(std::string_view url);
	void               Start(Pending& pending);
	void               AppendChunk(Pending& pending);

	kyty::gtk::GObjectPtr<SoupServer>       m_server;
	int                                     m_port = 0;
	std::map<std::string, LoopbackResponse> m_scripts;
	std::vector<RecordedRequest>            m_requests;
	std::vector<std::unique_ptr<Pending>>   m_pending;
};

// The core::HttpClient of the UI tests: records each request (URL, method, headers) and hands it to
// a SoupHttpClient with allow_loopback_http, whose results and request ids it passes on unchanged.
// That client's rewrite_url sends every hop to the server, the request and each redirect: an https
// URL becomes LoopbackServer::Rewrite(url), a URL of the server itself stays, and any other URL has
// no mapping, so nothing reaches the network. The server must outlive the client.
class LoopbackHttpClient final: public kyty::core::HttpClient {
public:
	explicit LoopbackHttpClient(LoopbackServer& server);
	~LoopbackHttpClient() override;

	LoopbackHttpClient(const LoopbackHttpClient&)            = delete;
	LoopbackHttpClient& operator=(const LoopbackHttpClient&) = delete;

	kyty::core::RequestId Get(const kyty::core::HttpRequest& request, ChunkFn on_chunk,
	                          DoneFn on_done) override;
	void                  Cancel(kyty::core::RequestId id) override;

	// Every Get() so far, in call order (url as the caller asked; path is the rewritten URL).
	[[nodiscard]] const std::vector<RecordedRequest>& Requests() const { return m_requests; }
	[[nodiscard]] std::size_t ActiveCount() const { return m_client.ActiveCount(); }

private:
	LoopbackServer&              m_server;
	kyty::gtk::SoupHttpClient    m_client;
	std::vector<RecordedRequest> m_requests;
};

} // namespace KytyTests
