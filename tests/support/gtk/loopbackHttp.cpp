#include "support/gtk/loopbackHttp.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace KytyTests {

using kyty::gtk::GErrorPtr;
using kyty::gtk::GObjectPtr;
using kyty::gtk::SignalScope;
using kyty::gtk::SourceId;

namespace {

constexpr std::string_view kHttps = "https://";

} // namespace

// A scripted response in progress.
struct LoopbackServer::Pending {
	LoopbackServer*               owner = nullptr;
	GObjectPtr<SoupServerMessage> message;
	LoopbackResponse              response;
	bool                          paused = false;
	std::size_t                   offset = 0;
	int                           chunks = 0;
	bool                          ended  = false;
	SourceId                      timer;
	SignalScope                   finished;
	SignalScope                   disconnected;
};

LoopbackServer::LoopbackServer() {
	m_server = GObjectPtr<SoupServer>::Adopt(soup_server_new(nullptr, nullptr));
	soup_server_add_handler(m_server.get(), nullptr, &OnRequest, this, nullptr);
	GErrorPtr error;
	if (soup_server_listen_local(m_server.get(), 0, SOUP_SERVER_LISTEN_IPV4_ONLY, error.Out()) ==
	    FALSE) {
		std::fprintf(stderr, "LoopbackServer: cannot listen on 127.0.0.1: %.*s\n",
		             static_cast<int>(error.Message().size()), error.Message().data());
		std::abort();
	}
	GSList* uris = soup_server_get_uris(m_server.get());
	if (uris == nullptr) {
		std::fprintf(stderr, "LoopbackServer: the server has no URI\n");
		std::abort();
	}
	m_port = g_uri_get_port(static_cast<GUri*>(uris->data));
	g_slist_free_full(uris, reinterpret_cast<GDestroyNotify>(g_uri_unref));
}

LoopbackServer::~LoopbackServer() {
	for (auto& pending: m_pending) {
		pending->timer.Remove();
		pending->finished.Disconnect();
		pending->disconnected.Disconnect();
	}
	soup_server_disconnect(m_server.get());
}

std::string LoopbackServer::PathOf(std::string_view url) {
	if (url.substr(0, kHttps.size()) != kHttps) {
		return std::string(url);
	}
	return "/" + std::string(url.substr(kHttps.size()));
}

std::string LoopbackServer::Rewrite(std::string_view url) const {
	if (url.substr(0, kHttps.size()) != kHttps) {
		return std::string(url);
	}
	return "http://127.0.0.1:" + std::to_string(m_port) + PathOf(url);
}

void LoopbackServer::Script(std::string_view url, LoopbackResponse response) {
	m_scripts.insert_or_assign(PathOf(url), std::move(response));
}

int LoopbackServer::Hits(std::string_view url) const {
	const std::string path = PathOf(url);
	return static_cast<int>(
	    std::count_if(m_requests.begin(), m_requests.end(),
	                  [&path](const auto& request) { return request.path == path; }));
}

void LoopbackServer::OnRequest(SoupServer* /*server*/, SoupServerMessage* message,
                               const char* /*path*/, GHashTable* /*query*/, gpointer data) {
	auto*       self  = static_cast<LoopbackServer*>(data);
	GUri*       uri   = soup_server_message_get_uri(message);
	const char* query = g_uri_get_query(uri);
	std::string key   = g_uri_get_path(uri);
	if (query != nullptr && *query != '\0') {
		key += std::string("?") + query;
	}
	RecordedRequest record;
	// The https URL that Rewrite() turned into this path.
	record.url    = "https:/" + key;
	record.path   = key;
	record.method = soup_server_message_get_method(message);
	soup_message_headers_foreach(
	    soup_server_message_get_request_headers(message),
	    [](const char* name, const char* value, gpointer headers) {
		    static_cast<std::vector<std::pair<std::string, std::string>>*>(headers)->emplace_back(
		        name, value);
	    },
	    &record.headers);
	self->m_requests.push_back(std::move(record));

	const auto found = self->m_scripts.find(key);
	if (found == self->m_scripts.end()) {
		const std::string body = "no script for " + key;
		soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, nullptr);
		soup_server_message_set_response(message, "text/plain", SOUP_MEMORY_COPY, body.data(),
		                                 body.size());
		return;
	}
	auto pending      = std::make_unique<Pending>();
	pending->owner    = self;
	pending->message  = GObjectPtr<SoupServerMessage>::Ref(message);
	pending->response = found->second;
	pending->finished =
	    SignalScope::Connect(message, "finished", G_CALLBACK(&OnFinished), pending.get());
	pending->disconnected =
	    SignalScope::Connect(message, "disconnected", G_CALLBACK(&OnFinished), pending.get());
	Pending& raw = *pending;
	self->m_pending.push_back(std::move(pending));
	if (raw.response.delay_ms > 0) {
		soup_server_message_pause(message);
		raw.paused = true;
		raw.timer  = SourceId(g_timeout_add(raw.response.delay_ms, &OnDelay, &raw));
		return;
	}
	self->Start(raw);
}

gboolean LoopbackServer::OnDelay(gpointer data) {
	auto* pending = static_cast<Pending*>(data);
	// The source ends here; Start() may store a new one in the same SourceId.
	pending->timer = SourceId();
	if (!pending->ended) {
		pending->owner->Start(*pending);
	}
	return G_SOURCE_REMOVE;
}

void LoopbackServer::Start(Pending& pending) {
	SoupServerMessage*      message  = pending.message.get();
	const LoopbackResponse& response = pending.response;
	soup_server_message_set_status(message, response.status, nullptr);
	SoupMessageHeaders* headers = soup_server_message_get_response_headers(message);
	for (const auto& [name, value]: response.headers) {
		soup_message_headers_replace(headers, name.c_str(), value.c_str());
	}
	if (response.chunk_size == 0) {
		soup_server_message_set_response(message, response.content_type.c_str(), SOUP_MEMORY_COPY,
		                                 response.body.data(), response.body.size());
	} else {
		soup_message_headers_set_encoding(headers, SOUP_ENCODING_CHUNKED);
		soup_message_headers_set_content_type(headers, response.content_type.c_str(), nullptr);
		AppendChunk(pending);
		const bool stalled =
		    response.stall_after_chunks > 0 && pending.chunks >= response.stall_after_chunks;
		if (pending.offset < response.body.size() && !stalled) {
			pending.timer =
			    SourceId(g_timeout_add(response.chunk_interval_ms, &OnChunkTimer, &pending));
		}
	}
	if (pending.paused) {
		pending.paused = false;
		soup_server_message_unpause(message);
	}
}

void LoopbackServer::AppendChunk(Pending& pending) {
	const auto&       body = pending.response.body;
	const std::size_t size = std::min(pending.response.chunk_size, body.size() - pending.offset);
	SoupMessageBody*  sink = soup_server_message_get_response_body(pending.message.get());
	if (size > 0) {
		soup_message_body_append(sink, SOUP_MEMORY_COPY, body.data() + pending.offset, size);
	}
	pending.offset += size;
	pending.chunks++;
	if (pending.offset >= body.size()) {
		soup_message_body_complete(sink);
	}
}

gboolean LoopbackServer::OnChunkTimer(gpointer data) {
	auto*                   pending  = static_cast<Pending*>(data);
	const LoopbackResponse& response = pending->response;
	if (pending->ended) {
		return G_SOURCE_REMOVE;
	}
	pending->owner->AppendChunk(*pending);
	soup_server_message_unpause(pending->message.get());
	const bool stalled =
	    response.stall_after_chunks > 0 && pending->chunks >= response.stall_after_chunks;
	return pending->offset < response.body.size() && !stalled ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

void LoopbackServer::OnFinished(SoupServerMessage* /*message*/, gpointer data) {
	auto* pending  = static_cast<Pending*>(data);
	pending->ended = true;
	pending->timer.Remove();
}

LoopbackHttpClient::LoopbackHttpClient(LoopbackServer& server)
    : m_server(server), m_client(kyty::gtk::SoupHttpClient::Options {.allow_loopback_http = true}) {
}

LoopbackHttpClient::~LoopbackHttpClient() = default;

kyty::core::RequestId LoopbackHttpClient::Get(const kyty::core::HttpRequest& request,
                                              ChunkFn on_chunk, DoneFn on_done) {
	kyty::core::HttpRequest rewritten = request;
	rewritten.url                     = m_server.Rewrite(request.url);
	m_requests.push_back({request.url, rewritten.url, "GET", request.headers});
	return m_client.Get(rewritten, std::move(on_chunk), std::move(on_done));
}

void LoopbackHttpClient::Cancel(kyty::core::RequestId id) {
	m_client.Cancel(id);
}

} // namespace KytyTests
