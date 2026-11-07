// Example tests of the libsoup HTTP client (src/launcher/gtk/services/soupHttpClient.h, task 19.4;
// plan D19, HC-07, HC-12) against the core::HttpClient contract (src/launcher/core/httpClient.h),
// with a SoupServer on 127.0.0.1 (no other network; the client's allow_loopback_http test option):
// a body in several chunks arrives in order and DoneFn runs once after the last chunk; no callback
// runs inside Get() or Cancel(); 404 gives Status 404; a refused connection gives Network;
// max_bytes + 1 gives TooLarge (max_bytes passes); a pausing server gives Stalled at stall_timeout
// while a slow but steady body passes; total_timeout gives TimedOut; a ChunkFn that returns false
// and Cancel() each give one Cancelled; Cancel() of a finished, unknown, or 0 id does nothing; a
// non-https URL without the test option gives Network "Only https URLs are allowed"; a redirect to
// another loopback port is followed under HttpsOnly and refused under SameOrigin, a relative
// same-origin redirect is followed, and a redirect loop stops after 10 redirects; the test option
// rewrite_url maps the requested URL and every redirect target, and an unmapped one ends with
// Network naming it before anything is sent; the request headers are sent and no User-Agent of
// the client's own. One GMainLoop context serves both.
// https (phase 7 plan D12, p6-u3-5): a loopback SoupServer with the test certificate of
// tests/fixtures/tls/ answers a client that trusts the test authority (Options::tls_database)
// without allow_loopback_http; the system's authorities refuse that certificate (Network, nothing
// reaches the server); a redirect from https to http is refused.

#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/services/soupHttpClient.h"
#include "support/noDisplay.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <glib.h>
#include <libsoup/soup.h>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using kyty::core::HttpFailure;
using kyty::core::HttpRequest;
using kyty::core::HttpResult;
using kyty::core::RedirectPolicy;
using kyty::core::RequestId;
using kyty::gtk::GErrorPtr;
using kyty::gtk::GObjectPtr;
using kyty::gtk::SoupHttpClient;
using kyty::gtk::SourceId;
using namespace std::chrono_literals;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "SoupHttpClientTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

const char* FailureName(HttpFailure failure) {
	switch (failure) {
		case HttpFailure::None: return "None";
		case HttpFailure::Network: return "Network";
		case HttpFailure::Status: return "Status";
		case HttpFailure::TooLarge: return "TooLarge";
		case HttpFailure::Stalled: return "Stalled";
		case HttpFailure::TimedOut: return "TimedOut";
		case HttpFailure::Cancelled: return "Cancelled";
	}
	return "?";
}

// Iterates the default main context until done() or limit.
bool RunUntil(const std::function<bool()>& done, std::chrono::milliseconds limit = 10s) {
	const gint64 deadline = g_get_monotonic_time() + limit.count() * 1000;
	while (!done()) {
		if (g_get_monotonic_time() >= deadline) {
			return false;
		}
		g_main_context_iteration(nullptr, FALSE);
		g_usleep(500);
	}
	return true;
}

void RunFor(std::chrono::milliseconds duration) {
	RunUntil([] { return false; }, duration);
}

// ---- The loopback server

// A chunked response: count chunks of text, one every interval; the first is written at once.
// stall_after: stop after that many chunks without completing the body.
struct Stream {
	GObjectPtr<SoupServerMessage> message;
	std::string                   text;
	int                           count       = 0;
	int                           sent        = 0;
	int                           stall_after = -1;
	guint                         interval_ms = 0;
	SourceId                      timer;
	bool                          finished = false;
};

class Server {
public:
	// With certificate, the server speaks https only.
	explicit Server(GTlsCertificate* certificate = nullptr)
	    : m_scheme(certificate != nullptr ? "https" : "http") {
		m_server = GObjectPtr<SoupServer>::Adopt(soup_server_new(nullptr, nullptr));
		soup_server_add_handler(m_server.get(), nullptr, &OnRequest, this, nullptr);
		auto options = SOUP_SERVER_LISTEN_IPV4_ONLY;
		if (certificate != nullptr) {
			soup_server_set_tls_certificate(m_server.get(), certificate);
			options = static_cast<SoupServerListenOptions>(options | SOUP_SERVER_LISTEN_HTTPS);
		}
		GErrorPtr error;
		Check(soup_server_listen_local(m_server.get(), 0, options, error.Out()) != FALSE,
		      "the loopback server listens: " + std::string(error.Message()));
		GSList* uris = soup_server_get_uris(m_server.get());
		Check(uris != nullptr, "the server has a URI");
		m_port = g_uri_get_port(static_cast<GUri*>(uris->data));
		g_slist_free_full(uris, reinterpret_cast<GDestroyNotify>(g_uri_unref));
	}

	~Server() {
		for (auto& stream: m_streams) {
			stream->timer.Remove();
			g_signal_handlers_disconnect_by_data(stream->message.get(), stream.get());
		}
		soup_server_disconnect(m_server.get());
	}

	Server(const Server&)            = delete;
	Server& operator=(const Server&) = delete;

	[[nodiscard]] int         Port() const { return m_port; }
	[[nodiscard]] std::string Url(const std::string& path) const {
		return m_scheme + "://127.0.0.1:" + std::to_string(m_port) + path;
	}

	// Where /redirect-other sends the client.
	void SetRedirectTarget(std::string url) { m_redirect_target = std::move(url); }

	[[nodiscard]] int Hits(const std::string& path) const {
		const auto found = m_hits.find(path);
		return found == m_hits.end() ? 0 : found->second;
	}
	[[nodiscard]] const std::optional<std::string>& LastUserAgent() const { return m_user_agent; }
	[[nodiscard]] const std::optional<std::string>& LastTestHeader() const { return m_test_header; }

	// Chunked streams whose server message has finished (completed or aborted by the client).
	[[nodiscard]] int FinishedStreams() const {
		int count = 0;
		for (const auto& stream: m_streams) {
			count += stream->finished ? 1 : 0;
		}
		return count;
	}

private:
	static gint64 Query(GHashTable* query, const char* key, gint64 fallback) {
		const char* value =
		    query != nullptr ? static_cast<const char*>(g_hash_table_lookup(query, key)) : nullptr;
		return value != nullptr ? g_ascii_strtoll(value, nullptr, 10) : fallback;
	}

	static void SetBody(SoupServerMessage* message, guint status, const std::string& body) {
		soup_server_message_set_status(message, status, nullptr);
		soup_server_message_set_response(message, "text/plain", SOUP_MEMORY_COPY, body.data(),
		                                 body.size());
	}

	static void OnRequest(SoupServer* /*server*/, SoupServerMessage* message, const char* path,
	                      GHashTable* query, gpointer data) {
		auto*             self = static_cast<Server*>(data);
		const std::string route(path);
		self->m_hits[route]++;
		SoupMessageHeaders* headers = soup_server_message_get_request_headers(message);
		const char*         agent   = soup_message_headers_get_one(headers, "User-Agent");
		const char*         test    = soup_message_headers_get_one(headers, "X-Kyty-Test");
		self->m_user_agent  = agent != nullptr ? std::optional<std::string>(agent) : std::nullopt;
		self->m_test_header = test != nullptr ? std::optional<std::string>(test) : std::nullopt;

		if (route == "/ok") {
			SetBody(message, SOUP_STATUS_OK, "hello");
		} else if (route == "/body") {
			const auto  size = static_cast<std::size_t>(Query(query, "size", 0));
			std::string body(size, '\0');
			for (std::size_t i = 0; i < size; i++) {
				body[i] = static_cast<char>('a' + i % 26);
			}
			SetBody(message, SOUP_STATUS_OK, body);
		} else if (route == "/missing") {
			SetBody(message, SOUP_STATUS_NOT_FOUND, "not here");
		} else if (route == "/stream") {
			self->StartStream(message, static_cast<int>(Query(query, "count", 5)),
			                  static_cast<guint>(Query(query, "interval", 30)),
			                  static_cast<int>(Query(query, "stall", -1)));
		} else if (route == "/redirect-other") {
			soup_server_message_set_redirect(message, SOUP_STATUS_FOUND,
			                                 self->m_redirect_target.c_str());
		} else if (route == "/redirect-same") {
			soup_server_message_set_status(message, SOUP_STATUS_MOVED_PERMANENTLY, nullptr);
			soup_message_headers_replace(soup_server_message_get_response_headers(message),
			                             "Location", "/ok");
		} else if (route == "/loop") {
			soup_server_message_set_status(message, SOUP_STATUS_TEMPORARY_REDIRECT, nullptr);
			soup_message_headers_replace(soup_server_message_get_response_headers(message),
			                             "Location", "/loop");
		} else if (route == "/redirect-nowhere") {
			soup_server_message_set_status(message, SOUP_STATUS_SEE_OTHER, nullptr);
		} else {
			SetBody(message, SOUP_STATUS_OK, "default");
		}
	}

	void StartStream(SoupServerMessage* message, int count, guint interval_ms, int stall_after) {
		auto stream         = std::make_unique<Stream>();
		stream->message     = GObjectPtr<SoupServerMessage>::Ref(message);
		stream->count       = count;
		stream->interval_ms = interval_ms;
		stream->stall_after = stall_after;
		soup_server_message_set_status(message, SOUP_STATUS_OK, nullptr);
		soup_message_headers_set_encoding(soup_server_message_get_response_headers(message),
		                                  SOUP_ENCODING_CHUNKED);
		g_signal_connect(message, "finished", G_CALLBACK(&OnStreamFinished), stream.get());
		g_signal_connect(message, "disconnected", G_CALLBACK(&OnStreamFinished), stream.get());
		Stream* raw = stream.get();
		m_streams.push_back(std::move(stream));
		// The first chunk goes out with the headers; the server pauses itself when it runs out of
		// chunks and resumes after each unpause.
		AppendChunk(*raw);
		if (raw->sent < raw->count && raw->sent != raw->stall_after) {
			raw->timer = SourceId(g_timeout_add(raw->interval_ms, &OnStreamTimer, raw));
		}
	}

	static void AppendChunk(Stream& stream) {
		SoupMessageBody*  body  = soup_server_message_get_response_body(stream.message.get());
		const std::string chunk = "chunk-" + std::to_string(stream.sent) + ";";
		soup_message_body_append(body, SOUP_MEMORY_COPY, chunk.data(), chunk.size());
		stream.text += chunk;
		stream.sent++;
		if (stream.sent == stream.count) {
			soup_message_body_complete(body);
		}
	}

	static gboolean OnStreamTimer(gpointer data) {
		auto* stream = static_cast<Stream*>(data);
		if (stream->finished) {
			return G_SOURCE_REMOVE;
		}
		AppendChunk(*stream);
		soup_server_message_unpause(stream->message.get());
		if (stream->sent == stream->count || stream->sent == stream->stall_after) {
			return G_SOURCE_REMOVE;
		}
		return G_SOURCE_CONTINUE;
	}

	static void OnStreamFinished(SoupServerMessage* /*message*/, gpointer data) {
		auto* stream     = static_cast<Stream*>(data);
		stream->finished = true;
		stream->timer.Remove();
	}

	std::string                          m_scheme;
	GObjectPtr<SoupServer>               m_server;
	int                                  m_port = 0;
	std::string                          m_redirect_target;
	std::map<std::string, int>           m_hits;
	std::optional<std::string>           m_user_agent;
	std::optional<std::string>           m_test_header;
	std::vector<std::unique_ptr<Stream>> m_streams;
};

// ---- Requests

// What one request delivered; every callback checks that it does not run inside Get() or Cancel().
struct Outcome {
	std::string              body;
	int                      chunks = 0;
	int                      done   = 0;
	HttpResult               result;
	bool                     chunk_after_done = false;
	std::function<bool(int)> on_chunk; // the chunk number; false aborts
};

bool g_inside_call = false;

RequestId Start(kyty::core::HttpClient& client, const HttpRequest& request, Outcome& outcome) {
	g_inside_call      = true;
	const RequestId id = client.Get(
	    request,
	    [&outcome](std::span<const std::byte> bytes) {
		    Check(!g_inside_call, "ChunkFn never runs inside Get() or Cancel()");
		    if (outcome.done > 0) {
			    outcome.chunk_after_done = true;
		    }
		    outcome.chunks++;
		    outcome.body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		    return outcome.on_chunk ? outcome.on_chunk(outcome.chunks) : true;
	    },
	    [&outcome](const HttpResult& result) {
		    Check(!g_inside_call, "DoneFn never runs inside Get() or Cancel()");
		    outcome.done++;
		    outcome.result = result;
	    });
	g_inside_call = false;
	Check(id != 0, "Get() never returns 0");
	return id;
}

void CancelOutside(kyty::core::HttpClient& client, RequestId id) {
	g_inside_call = true;
	client.Cancel(id);
	g_inside_call = false;
}

// Runs one request to its end and checks the DoneFn count.
Outcome Run(SoupHttpClient& client, const HttpRequest& request,
            std::chrono::milliseconds limit = 10s) {
	Outcome outcome;
	Start(client, request, outcome);
	Check(outcome.done == 0 && outcome.chunks == 0, "nothing is delivered before Get() returns");
	Check(RunUntil([&] { return outcome.done > 0; }, limit), "the request ends: " + request.url);
	RunFor(30ms);
	Check(outcome.done == 1, "DoneFn runs exactly once: " + request.url);
	Check(!outcome.chunk_after_done, "no chunk after DoneFn: " + request.url);
	std::printf("SoupHttpClientTests: %s -> %s, status %d, %d chunk(s), %zu byte(s), \"%s\"\n",
	            request.url.c_str(), FailureName(outcome.result.failure), outcome.result.status,
	            outcome.chunks, outcome.body.size(), outcome.result.message.c_str());
	return outcome;
}

HttpRequest Request(std::string url) {
	HttpRequest request;
	request.url = std::move(url);
	return request;
}

SoupHttpClient::Options Loopback() {
	return SoupHttpClient::Options {.allow_loopback_http = true};
}

void TestSuccess(Server& server) {
	SoupHttpClient client(Loopback());
	const auto     ok = Run(client, Request(server.Url("/ok")));
	Check(ok.result.failure == HttpFailure::None && ok.result.status == 200 && ok.body == "hello",
	      "a 200 response delivers its body and None");
	Check(ok.result.message.empty(), "success has no message");

	const auto  stream = Run(client, Request(server.Url("/stream?count=6&interval=40")));
	std::string expected;
	for (int i = 0; i < 6; i++) {
		expected += "chunk-" + std::to_string(i) + ";";
	}
	Check(stream.result.failure == HttpFailure::None && stream.body == expected,
	      "a chunked body arrives complete and in order");
	Check(stream.chunks >= 3, "a body written in pauses arrives in several chunks");
	Check(client.ActiveCount() == 0, "no request is left");
}

void TestStatusAndNetwork(Server& server) {
	SoupHttpClient client(Loopback());
	const auto     missing = Run(client, Request(server.Url("/missing")));
	Check(missing.result.failure == HttpFailure::Status && missing.result.status == 404,
	      "404 gives Status with the code");
	Check(missing.chunks == 0, "a non-2xx body is not delivered");

	// A port that was just closed refuses the connection.
	int closed_port = 0;
	{
		Server gone;
		closed_port = gone.Port();
	}
	const auto refused =
	    Run(client, Request("http://127.0.0.1:" + std::to_string(closed_port) + "/ok"));
	Check(refused.result.failure == HttpFailure::Network && refused.result.status == 0 &&
	          !refused.result.message.empty(),
	      "a refused connection gives Network with a message and status 0");

	const auto invalid = Run(client, Request("this is not a url"));
	Check(invalid.result.failure == HttpFailure::Network && !invalid.result.message.empty(),
	      "an invalid URL gives Network");
}

void TestHttpsOnly(Server& server) {
	SoupHttpClient production;
	const int      hits  = server.Hits("/ok");
	const auto     plain = Run(production, Request(server.Url("/ok")));
	Check(plain.result.failure == HttpFailure::Network &&
	          plain.result.message == "Only https URLs are allowed: " + server.Url("/ok"),
	      "a non-https URL gives Network \"Only https URLs are allowed\"");
	Check(server.Hits("/ok") == hits && plain.chunks == 0, "nothing is requested");
	const auto ftp = Run(production, Request("ftp://127.0.0.1/file"));
	Check(ftp.result.failure == HttpFailure::Network &&
	          ftp.result.message.rfind("Only https URLs are allowed", 0) == 0,
	      "another scheme is refused the same way");
	SoupHttpClient loopback(Loopback());
	const auto     remote = Run(loopback, Request("http://192.0.2.1/ok"));
	Check(remote.result.failure == HttpFailure::Network &&
	          remote.result.message.rfind("Only https URLs are allowed", 0) == 0,
	      "the test option allows http only to loopback hosts");
}

void TestLimits(Server& server) {
	SoupHttpClient client(Loopback());
	auto           request = Request(server.Url("/body?size=1000"));
	request.max_bytes      = 1000;
	const auto exact       = Run(client, request);
	Check(exact.result.failure == HttpFailure::None && exact.body.size() == 1000,
	      "a body of exactly max_bytes passes");
	request.url          = server.Url("/body?size=1001");
	const auto too_large = Run(client, request);
	Check(too_large.result.failure == HttpFailure::TooLarge && too_large.body.size() <= 1000,
	      "max_bytes + 1 gives TooLarge before the excess is delivered");
	// The message counts through ngettext: the plural form, and the singular one for 1 byte.
	Check(too_large.result.message == "response body larger than 1000 bytes",
	      "the TooLarge message names the limit (plural)");
	request.url         = server.Url("/body?size=2");
	request.max_bytes   = 1;
	const auto one_byte = Run(client, request);
	Check(one_byte.result.failure == HttpFailure::TooLarge &&
	          one_byte.result.message == "response body larger than 1 byte",
	      "the TooLarge message names the limit (singular)");

	// Several 64 KiB reads: the limit holds across chunks.
	request.url       = server.Url("/body?size=300000");
	request.max_bytes = 200000;
	const auto big    = Run(client, request);
	Check(big.result.failure == HttpFailure::TooLarge && big.body.size() <= 200000,
	      "the limit counts every chunk");

	auto stall          = Request(server.Url("/stream?count=5&interval=10&stall=1"));
	stall.stall_timeout = 200ms;
	const auto start    = std::chrono::steady_clock::now();
	const auto stalled  = Run(client, stall);
	const auto elapsed  = std::chrono::steady_clock::now() - start;
	std::printf("SoupHttpClientTests: Stalled after %lld ms (stall_timeout 200 ms)\n",
	            static_cast<long long>(
	                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
	Check(stalled.result.failure == HttpFailure::Stalled && stalled.body == "chunk-0;",
	      "a server that pauses after one chunk gives Stalled");
	Check(elapsed >= 200ms && elapsed < 3s, "Stalled comes at stall_timeout");

	auto steady          = Request(server.Url("/stream?count=8&interval=100"));
	steady.stall_timeout = 300ms;
	const auto passed    = Run(client, steady);
	Check(passed.result.failure == HttpFailure::None && passed.chunks >= 4,
	      "a slow but steady body (800 ms in total) passes a 300 ms stall_timeout");

	auto slow          = Request(server.Url("/stream?count=20&interval=100"));
	slow.total_timeout = 400ms;
	const auto timed   = Run(client, slow);
	Check(timed.result.failure == HttpFailure::TimedOut && timed.result.status == 200 &&
	          timed.chunks >= 1,
	      "total_timeout gives TimedOut while the body still arrives");
	Check(RunUntil([&] { return server.FinishedStreams() >= 3; }, 3s),
	      "the server sees the aborted streams end");
}

void TestCancellation(Server& server) {
	SoupHttpClient client(Loopback());

	// A ChunkFn that returns false ends the request with one Cancelled.
	Outcome abort;
	abort.on_chunk = [](int chunk) { return chunk < 2; };
	Start(client, Request(server.Url("/stream?count=10&interval=30")), abort);
	Check(RunUntil([&] { return abort.done > 0; }), "the aborted request ends");
	RunFor(150ms);
	Check(abort.done == 1 && abort.result.failure == HttpFailure::Cancelled && abort.chunks == 2,
	      "a ChunkFn that returns false gives one Cancelled and no further chunk");

	// Cancel() right after Get() and while the body arrives: one deferred Cancelled.
	Outcome         early;
	const RequestId early_id = Start(client, Request(server.Url("/ok")), early);
	CancelOutside(client, early_id);
	CancelOutside(client, early_id);
	Check(early.done == 0, "Cancel() never calls DoneFn itself");
	Check(RunUntil([&] { return early.done > 0; }), "the cancelled request ends");
	RunFor(100ms);
	Check(early.done == 1 && early.result.failure == HttpFailure::Cancelled && early.chunks == 0,
	      "Cancel() right after Get() gives one Cancelled");

	Outcome         middle;
	const RequestId middle_id =
	    Start(client, Request(server.Url("/stream?count=20&interval=30")), middle);
	Check(RunUntil([&] { return middle.chunks >= 1; }), "the body starts");
	CancelOutside(client, middle_id);
	const int chunks_at_cancel = middle.chunks;
	Check(RunUntil([&] { return middle.done > 0; }), "the request cancelled mid-body ends");
	RunFor(150ms);
	Check(middle.done == 1 && middle.result.failure == HttpFailure::Cancelled &&
	          middle.chunks == chunks_at_cancel,
	      "Cancel() during the body gives one Cancelled and no further chunk");

	// Cancel() of a finished, unknown, or 0 id does nothing.
	Outcome         finished;
	const RequestId finished_id = Start(client, Request(server.Url("/ok")), finished);
	Check(RunUntil([&] { return finished.done > 0; }), "the request finishes");
	CancelOutside(client, finished_id);
	CancelOutside(client, 0);
	CancelOutside(client, finished_id + 1000);
	RunFor(80ms);
	Check(finished.done == 1 && finished.result.failure == HttpFailure::None,
	      "Cancel() of a finished request changes nothing");

	// A ChunkFn may cancel its own request: one Cancelled.
	Outcome   self;
	RequestId self_id = 0;
	self.on_chunk     = [&](int) {
		client.Cancel(self_id);
		return true;
	};
	self_id = Start(client, Request(server.Url("/stream?count=10&interval=30")), self);
	Check(RunUntil([&] { return self.done > 0; }), "the self-cancelled request ends");
	RunFor(150ms);
	Check(self.done == 1 && self.result.failure == HttpFailure::Cancelled && self.chunks == 1,
	      "Cancel() from ChunkFn gives one Cancelled");

	// The destructor ends every request without a callback.
	Outcome orphan;
	{
		SoupHttpClient short_lived(Loopback());
		Start(short_lived, Request(server.Url("/stream?count=20&interval=30")), orphan);
		RunUntil([&] { return orphan.chunks >= 1; });
	}
	RunFor(200ms);
	Check(orphan.done == 0, "no DoneFn after the client is destroyed");

	// DoneFn may destroy the client.
	auto    owned = std::make_unique<SoupHttpClient>(Loopback());
	Outcome last;
	bool    destroyed = false;
	g_inside_call     = true;
	owned->Get(
	    Request(server.Url("/ok")), [](std::span<const std::byte>) { return true; },
	    [&](const HttpResult& result) {
		    last.result = result;
		    owned.reset();
		    destroyed = true;
	    });
	g_inside_call = false;
	Check(RunUntil([&] { return destroyed; }), "DoneFn destroys the client");
	Check(last.result.failure == HttpFailure::None, "the request succeeded first");
}

void TestRedirects(Server& server) {
	Server other;
	server.SetRedirectTarget(other.Url("/ok"));
	SoupHttpClient client(Loopback());

	auto       https_only = Request(server.Url("/redirect-other"));
	const auto followed   = Run(client, https_only);
	Check(followed.result.failure == HttpFailure::None && followed.body == "hello" &&
	          other.Hits("/ok") == 1,
	      "HttpsOnly follows a redirect to another port");

	auto same_origin      = Request(server.Url("/redirect-other"));
	same_origin.redirects = RedirectPolicy::SameOrigin;
	const auto refused    = Run(client, same_origin);
	Check(refused.result.failure == HttpFailure::Network && refused.result.status == 302 &&
	          refused.result.message.find(other.Url("/ok")) != std::string::npos,
	      "SameOrigin refuses a redirect to another port and names the URL");
	Check(other.Hits("/ok") == 1, "the refused target is never requested");

	auto relative      = Request(server.Url("/redirect-same"));
	relative.redirects = RedirectPolicy::SameOrigin;
	const auto local   = Run(client, relative);
	Check(local.result.failure == HttpFailure::None && local.body == "hello",
	      "SameOrigin follows a relative redirect on the same origin");

	const int  before = server.Hits("/loop");
	const auto loop   = Run(client, Request(server.Url("/loop")));
	Check(loop.result.failure == HttpFailure::Network && loop.result.status == 307 &&
	          loop.result.message.find(server.Url("/loop")) != std::string::npos &&
	          loop.result.message.ends_with(": more than 10 redirects"),
	      "a redirect loop ends with Network naming the URL and the limit");
	Check(server.Hits("/loop") - before == 1 + SoupHttpClient::kMaxRedirects,
	      "at most 10 redirects are followed");

	const auto nowhere = Run(client, Request(server.Url("/redirect-nowhere")));
	Check(nowhere.result.failure == HttpFailure::Network && nowhere.result.status == 303,
	      "a redirect without Location ends with Network");

	// A redirect to a non-https URL is refused for a production client: here the test option
	// covers loopback only, so a redirect to another host is refused.
	server.SetRedirectTarget("http://192.0.2.1/elsewhere");
	const auto elsewhere = Run(client, Request(server.Url("/redirect-other")));
	Check(elsewhere.result.failure == HttpFailure::Network &&
	          elsewhere.result.message.find("http://192.0.2.1/elsewhere") != std::string::npos,
	      "a redirect to a non-https URL is refused and named");
}

// Options::rewrite_url (phase 8 plan D17, p7-u2-3): https://mapped.example/<path> goes to the
// loopback server's /<path>, and every other URL has no mapping. A redirect to a mapped URL, also a
// relative one (resolved against the URL as requested), is served there; a request or a redirect to
// an unmapped https URL ends with Network naming it, and nothing is sent.
void TestRewrite(Server& server) {
	const std::string        mapped_prefix = "https://mapped.example";
	std::vector<std::string> mapped_urls;
	auto                     options = Loopback();
	options.rewrite_url              = [&](const std::string& url) -> std::string {
		mapped_urls.push_back(url);
		if (url.rfind(mapped_prefix + "/", 0) == 0) {
			return server.Url(url.substr(mapped_prefix.size()));
		}
		return "";
	};
	SoupHttpClient client(std::move(options));

	const int  ok_hits = server.Hits("/ok");
	const auto direct  = Run(client, Request(mapped_prefix + "/ok"));
	Check(direct.result.failure == HttpFailure::None && direct.body == "hello" &&
	          server.Hits("/ok") == ok_hits + 1,
	      "a mapped https URL is served by the URL it maps to");

	server.SetRedirectTarget(mapped_prefix + "/ok");
	mapped_urls.clear();
	const auto redirected = Run(client, Request(mapped_prefix + "/redirect-other"));
	Check(redirected.result.failure == HttpFailure::None && redirected.body == "hello" &&
	          server.Hits("/ok") == ok_hits + 2,
	      "a redirect to a mapped https URL is mapped too and served");
	Check(mapped_urls ==
	          std::vector<std::string> {mapped_prefix + "/redirect-other", mapped_prefix + "/ok"},
	      "rewrite_url sees the requested URL and the redirect target as the server wrote it");

	auto relative      = Request(mapped_prefix + "/redirect-same");
	relative.redirects = RedirectPolicy::SameOrigin;
	const auto local   = Run(client, relative);
	Check(
	    local.result.failure == HttpFailure::None && local.body == "hello" &&
	        server.Hits("/ok") == ok_hits + 3,
	    "a relative redirect resolves against the requested URL, keeps its origin, and is mapped");

	const std::string unmapped = "https://unmapped.example/secret?x=1";
	server.SetRedirectTarget(unmapped);
	const int  secret_hits = server.Hits("/secret");
	const auto refused     = Run(client, Request(mapped_prefix + "/redirect-other"));
	Check(refused.result.failure == HttpFailure::Network && refused.result.status == 302 &&
	          refused.result.message == "No URL mapping for " + unmapped && refused.chunks == 0,
	      "a redirect to an unmapped https URL ends with Network naming it");
	const auto unknown = Run(client, Request(unmapped));
	Check(unknown.result.failure == HttpFailure::Network && unknown.result.status == 0 &&
	          unknown.result.message == "No URL mapping for " + unmapped,
	      "a request of an unmapped https URL ends with Network naming it");
	Check(server.Hits("/secret") == secret_hits, "nothing is sent for an unmapped URL");

	// A mapping must pass the scheme check like any URL.
	SoupHttpClient production(SoupHttpClient::Options {
	    .rewrite_url = [&server](const std::string&) { return server.Url("/ok"); }});
	const int      before = server.Hits("/ok");
	const auto     plain  = Run(production, Request(mapped_prefix + "/ok"));
	Check(plain.result.failure == HttpFailure::Network &&
	          plain.result.message == "Only https URLs are allowed: " + server.Url("/ok") &&
	          server.Hits("/ok") == before,
	      "a mapping to http without allow_loopback_http is refused and named");
	Check(client.ActiveCount() == 0 && production.ActiveCount() == 0, "no request is left");
}

void TestHeaders(Server& server) {
	SoupHttpClient client(Loopback());
	auto           request = Request(server.Url("/headers"));
	request.headers        = {{"User-Agent", "KytyTest/1.0"}, {"X-Kyty-Test", "value 1"}};
	const auto sent        = Run(client, request);
	Check(sent.result.failure == HttpFailure::None, "the header request succeeds");
	Check(server.LastUserAgent() == std::optional<std::string>("KytyTest/1.0") &&
	          server.LastTestHeader() == std::optional<std::string>("value 1"),
	      "the request headers are sent");
	const auto bare = Run(client, Request(server.Url("/headers")));
	Check(bare.result.failure == HttpFailure::None && !server.LastUserAgent().has_value(),
	      "the client adds no User-Agent of its own");
}

std::string FixturePath(const char* name) {
	return std::string(KYTY_TEST_SOURCE_DIR) + "/tests/fixtures/tls/" + name;
}

// https against a loopback server with the test certificate (p6-u3-5): the https branch of the
// client, its certificate check, and the https-only redirect rule, without allow_loopback_http.
void TestTls(Server& plain) {
	GErrorPtr  error;
	const auto certificate = GObjectPtr<GTlsCertificate>::Adopt(
	    g_tls_certificate_new_from_files(FixturePath("server-cert.pem").c_str(),
	                                     FixturePath("server-key.pem").c_str(), error.Out()));
	Check(static_cast<bool>(certificate),
	      "the test server certificate loads: " + std::string(error.Message()));
	const auto authority = GObjectPtr<GTlsDatabase>::Adopt(
	    g_tls_file_database_new(FixturePath("ca-cert.pem").c_str(), error.Out()));
	Check(static_cast<bool>(authority),
	      "the test authority loads: " + std::string(error.Message()));
	Server secure(certificate.get());

	SoupHttpClient trusting(SoupHttpClient::Options {.tls_database = authority.get()});
	const auto     trusted = Run(trusting, Request(secure.Url("/ok")));
	Check(trusted.result.failure == HttpFailure::None && trusted.result.status == 200 &&
	          trusted.body == "hello" && secure.Hits("/ok") == 1,
	      "a client that trusts the test authority fetches https from the loopback server");

	SoupHttpClient production;
	const auto     refused = Run(production, Request(secure.Url("/ok")));
	Check(refused.result.failure == HttpFailure::Network && refused.result.status == 0 &&
	          !refused.result.message.empty() && refused.chunks == 0,
	      "the system's authorities refuse the test certificate: Network, with a message");
	Check(secure.Hits("/ok") == 1, "nothing reaches the server without a trusted certificate");

	secure.SetRedirectTarget(plain.Url("/ok"));
	const int  plain_hits = plain.Hits("/ok");
	const auto downgrade  = Run(trusting, Request(secure.Url("/redirect-other")));
	Check(downgrade.result.failure == HttpFailure::Network && downgrade.result.status == 302 &&
	          downgrade.result.message ==
	              "Refused the redirect to " + plain.Url("/ok") + ": only https URLs are allowed",
	      "a redirect from https to http is refused and named");
	Check(plain.Hits("/ok") == plain_hits,
	      "the http target of the refused redirect is never asked");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("SoupHttpClientTests");
	g_log_set_always_fatal(GLogLevelFlags(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING));

	Server server;
	TestSuccess(server);
	TestStatusAndNetwork(server);
	TestHttpsOnly(server);
	TestLimits(server);
	TestCancellation(server);
	TestRedirects(server);
	TestRewrite(server);
	TestHeaders(server);
	TestTls(server);
	std::printf("SoupHttpClientTests: all tests passed\n");
	return 0;
}
