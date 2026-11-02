#include "launcher/gtk/services/soupHttpClient.h"

#include "launcher/gtk/i18n.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <span>
#include <utility>

namespace kyty::gtk {

using core::HttpFailure;
using core::RedirectPolicy;
using core::RequestId;

namespace {

// libsoup's flags for parsing HTTP URLs (an int in C).
constexpr auto kUriFlags = static_cast<GUriFlags>(SOUP_HTTP_URI_FLAGS);

// A GUri reference (GUri is a boxed type, not a GObject).
class UriPtr {
public:
	UriPtr() = default;
	explicit UriPtr(GUri* uri): m_uri(uri) {}
	UriPtr(const UriPtr&)            = delete;
	UriPtr& operator=(const UriPtr&) = delete;
	UriPtr(UriPtr&& other) noexcept: m_uri(std::exchange(other.m_uri, nullptr)) {}
	UriPtr& operator=(UriPtr&& other) noexcept {
		if (this != &other) {
			reset(std::exchange(other.m_uri, nullptr));
		}
		return *this;
	}
	~UriPtr() { reset(); }

	[[nodiscard]] GUri* get() const { return m_uri; }
	explicit            operator bool() const { return m_uri != nullptr; }

	void reset(GUri* uri = nullptr) {
		if (GUri* old = std::exchange(m_uri, uri); old != nullptr) {
			g_uri_unref(old);
		}
	}

private:
	GUri* m_uri = nullptr;
};

// One GBytes reference.
class BytesPtr {
public:
	explicit BytesPtr(GBytes* bytes): m_bytes(bytes) {}
	BytesPtr(const BytesPtr&)            = delete;
	BytesPtr& operator=(const BytesPtr&) = delete;
	~BytesPtr() {
		if (m_bytes != nullptr) {
			g_bytes_unref(m_bytes);
		}
	}

	[[nodiscard]] GBytes* get() const { return m_bytes; }

private:
	GBytes* m_bytes;
};

bool IsRedirect(guint status) {
	return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

bool IsLoopbackHost(const char* host) {
	return host != nullptr && (g_strcmp0(host, "127.0.0.1") == 0 || g_strcmp0(host, "::1") == 0 ||
	                           g_ascii_strcasecmp(host, "localhost") == 0);
}

// The port of uri, with the scheme's default port when the URL names none.
int EffectivePort(GUri* uri) {
	const int port = g_uri_get_port(uri);
	if (port != -1) {
		return port;
	}
	const char* scheme = g_uri_get_scheme(uri);
	if (g_ascii_strcasecmp(scheme, "https") == 0) {
		return 443;
	}
	if (g_ascii_strcasecmp(scheme, "http") == 0) {
		return 80;
	}
	return -1;
}

// RedirectPolicy::SameOrigin: the same scheme, host, and port.
bool SameOrigin(GUri* a, GUri* b) {
	const char* host_a = g_uri_get_host(a);
	const char* host_b = g_uri_get_host(b);
	return g_ascii_strcasecmp(g_uri_get_scheme(a), g_uri_get_scheme(b)) == 0 && host_a != nullptr &&
	       host_b != nullptr && g_ascii_strcasecmp(host_a, host_b) == 0 &&
	       EffectivePort(a) == EffectivePort(b);
}

std::string UriText(GUri* uri) {
	const GCharPtr text(g_uri_to_string(uri));
	return std::string(text.View());
}

// Formats a translated message with one or two string arguments.
std::string Message(const char* format, const std::string& first) {
	const GCharPtr text(g_strdup_printf(format, first.c_str()));
	return std::string(text.View());
}

std::string Message(const char* format, const std::string& first, const std::string& second) {
	const GCharPtr text(g_strdup_printf(format, first.c_str(), second.c_str()));
	return std::string(text.View());
}

guint Milliseconds(std::chrono::milliseconds duration) {
	return static_cast<guint>(
	    std::clamp<int64_t>(duration.count(), 0, static_cast<int64_t>(G_MAXUINT)));
}

// Closes a response body stream without reading the rest: with a cancelled cancellable libsoup ends
// the message at once (a plain close would read the remaining body and could block). A stream with
// a pending read is closed when that read has ended (OnRead).
void CloseWithoutReading(GInputStream* stream) {
	if (stream == nullptr || g_input_stream_is_closed(stream) ||
	    g_input_stream_has_pending(stream)) {
		return;
	}
	auto cancelled = GObjectPtr<GCancellable>::Adopt(g_cancellable_new());
	g_cancellable_cancel(cancelled.get());
	g_input_stream_close(stream, cancelled.get(), nullptr);
}

} // namespace

struct SoupHttpClient::Request {
	SoupHttpClient*   owner = nullptr; // null once the client is gone
	RequestId         id    = 0;
	core::HttpRequest request;
	ChunkFn           on_chunk;
	DoneFn            on_done;
	UriPtr            requested; // the URL of the request
	UriPtr            current;   // the URL of the current hop
	int               redirects     = 0;
	int               status        = 0; // the status of the latest response
	uint64_t          received      = 0; // body bytes delivered
	bool              finished      = false;
	bool              cancel_queued = false;

	GObjectPtr<GCancellable> cancellable;
	GObjectPtr<SoupMessage>  message;
	GObjectPtr<GInputStream> stream;
	SourceId                 start_source;
	SourceId                 stall_source;
	SourceId                 total_source;
	SourceId                 cancel_source;

	// An operation's result is ignored once the request has ended, was cancelled, or the client is
	// gone.
	[[nodiscard]] bool Ignored() const { return finished || cancel_queued || owner == nullptr; }
};

gpointer SoupHttpClient::Hold(const RequestRef& request) {
	return new RequestRef(request);
}

SoupHttpClient::RequestRef SoupHttpClient::Take(gpointer data) {
	std::unique_ptr<RequestRef> holder(static_cast<RequestRef*>(data));
	return std::move(*holder);
}

SoupHttpClient::SoupHttpClient(): SoupHttpClient(Options {}) {}

SoupHttpClient::SoupHttpClient(Options options)
    : m_options(options), m_session(GObjectPtr<SoupSession>::Adopt(soup_session_new())) {
	if (m_options.tls_database != nullptr) {
		soup_session_set_tls_database(m_session.get(), m_options.tls_database);
		// The session holds the database; the client keeps no pointer of its own.
		m_options.tls_database = nullptr;
	}
}

SoupHttpClient::~SoupHttpClient() {
	for (auto& [id, request]: m_requests) {
		request->owner    = nullptr;
		request->finished = true;
		request->start_source.Remove();
		request->stall_source.Remove();
		request->total_source.Remove();
		request->cancel_source.Remove();
		g_cancellable_cancel(request->cancellable.get());
		CloseWithoutReading(request->stream.get());
	}
	m_requests.clear();
	soup_session_abort(m_session.get());
}

RequestId SoupHttpClient::Get(const core::HttpRequest& request, ChunkFn on_chunk, DoneFn on_done) {
	const RequestId id    = m_next_id++;
	auto            entry = std::make_shared<Request>();
	entry->owner          = this;
	entry->id             = id;
	entry->request        = request;
	entry->on_chunk       = std::move(on_chunk);
	entry->on_done        = std::move(on_done);
	entry->cancellable    = GObjectPtr<GCancellable>::Adopt(g_cancellable_new());
	if (request.total_timeout.count() > 0) {
		entry->total_source = SourceId(
		    g_timeout_add(Milliseconds(request.total_timeout), &OnTotalTimeout, entry.get()));
	}
	ArmStallTimer(*entry);
	entry->start_source = SourceId(g_idle_add(&OnStart, entry.get()));
	m_requests.emplace(id, std::move(entry));
	return id;
}

void SoupHttpClient::Cancel(RequestId id) {
	const auto found = m_requests.find(id);
	if (found == m_requests.end() || found->second->cancel_queued) {
		return;
	}
	Request& request      = *found->second;
	request.cancel_queued = true;
	request.start_source.Remove();
	request.stall_source.Remove();
	request.total_source.Remove();
	// A GIO operation that completes because of this ignores its result (cancel_queued).
	g_cancellable_cancel(request.cancellable.get());
	request.cancel_source = SourceId(g_idle_add(&OnCancelIdle, &request));
}

bool SoupHttpClient::Allowed(GUri* uri) const {
	const char* scheme = g_uri_get_scheme(uri);
	const char* host   = g_uri_get_host(uri);
	if (host == nullptr || *host == '\0') {
		return false;
	}
	if (g_ascii_strcasecmp(scheme, "https") == 0) {
		return true;
	}
	return m_options.allow_loopback_http && g_ascii_strcasecmp(scheme, "http") == 0 &&
	       IsLoopbackHost(host);
}

void SoupHttpClient::ArmStallTimer(Request& request) {
	if (request.request.stall_timeout.count() <= 0) {
		return;
	}
	request.stall_source = SourceId(
	    g_timeout_add(Milliseconds(request.request.stall_timeout), &OnStallTimeout, &request));
}

gboolean SoupHttpClient::OnStart(gpointer data) {
	auto*      request = static_cast<Request*>(data);
	const auto found   = request->owner->m_requests.find(request->id);
	// The request is pending: Finish() and Cancel() remove this source.
	request->owner->Begin(found->second);
	return G_SOURCE_REMOVE;
}

gboolean SoupHttpClient::OnStallTimeout(gpointer data) {
	auto* request = static_cast<Request*>(data);
	request->owner->Finish(
	    request->id, HttpFailure::Stalled,
	    Message(_("no data for %s ms"), std::to_string(request->request.stall_timeout.count())));
	return G_SOURCE_REMOVE;
}

gboolean SoupHttpClient::OnTotalTimeout(gpointer data) {
	auto* request = static_cast<Request*>(data);
	request->owner->Finish(request->id, HttpFailure::TimedOut,
	                       Message(_("timed out after %s ms"),
	                               std::to_string(request->request.total_timeout.count())));
	return G_SOURCE_REMOVE;
}

gboolean SoupHttpClient::OnCancelIdle(gpointer data) {
	auto* request = static_cast<Request*>(data);
	request->owner->Finish(request->id, HttpFailure::Cancelled, _("cancelled"));
	return G_SOURCE_REMOVE;
}

void SoupHttpClient::Begin(const RequestRef& request) {
	const std::string& url = request->request.url;
	GErrorPtr          error;
	UriPtr             uri(g_uri_parse(url.c_str(), kUriFlags, error.Out()));
	if (!uri) {
		Finish(request->id, HttpFailure::Network,
		       Message(_("Invalid URL %s: %s"), url, std::string(error.Message())));
		return;
	}
	if (!Allowed(uri.get())) {
		Finish(request->id, HttpFailure::Network,
		       Message(_("Only https URLs are allowed: %s"), url));
		return;
	}
	request->current.reset(g_uri_ref(uri.get()));
	request->requested = std::move(uri);
	Send(request);
}

void SoupHttpClient::Send(const RequestRef& request) {
	request->message =
	    GObjectPtr<SoupMessage>::Adopt(soup_message_new_from_uri("GET", request->current.get()));
	SoupMessage* message = request->message.get();
	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	SoupMessageHeaders* headers = soup_message_get_request_headers(message);
	for (const auto& [name, value]: request->request.headers) {
		soup_message_headers_append(headers, name.c_str(), value.c_str());
	}
	soup_session_send_async(m_session.get(), message, G_PRIORITY_DEFAULT,
	                        request->cancellable.get(), &OnSent, Hold(request));
}

void SoupHttpClient::OnSent(GObject* source, GAsyncResult* result, gpointer data) {
	const RequestRef request = Take(data);
	GErrorPtr        error;
	auto             stream = GObjectPtr<GInputStream>::Adopt(
	    soup_session_send_finish(SOUP_SESSION(source), result, error.Out()));
	if (request->Ignored()) {
		CloseWithoutReading(stream.get());
		return;
	}
	request->owner->OnResponse(request, std::move(stream), error);
}

void SoupHttpClient::OnResponse(const RequestRef& request, GObjectPtr<GInputStream> stream,
                                const GErrorPtr& error) {
	if (!stream) {
		Finish(request->id, HttpFailure::Network, std::string(error.Message()));
		return;
	}
	const guint status = soup_message_get_status(request->message.get());
	request->status    = static_cast<int>(status);
	if (IsRedirect(status)) {
		CloseWithoutReading(stream.get());
		OnRedirect(request, status);
		return;
	}
	if (status < 200 || status > 299) {
		CloseWithoutReading(stream.get());
		const char* reason = soup_message_get_reason_phrase(request->message.get());
		Finish(request->id, HttpFailure::Status,
		       reason != nullptr && *reason != '\0'
		           ? Message(_("HTTP status %s %s"), std::to_string(status), reason)
		           : Message(_("HTTP status %s"), std::to_string(status)));
		return;
	}
	request->stream = std::move(stream);
	Read(request);
}

void SoupHttpClient::OnRedirect(const RequestRef& request, guint status) {
	const char* location = soup_message_headers_get_one(
	    soup_message_get_response_headers(request->message.get()), "Location");
	if (location == nullptr || *location == '\0') {
		Finish(request->id, HttpFailure::Network,
		       Message(_("The redirect from %s (HTTP status %s) has no Location header"),
		               UriText(request->current.get()), std::to_string(status)));
		return;
	}
	GErrorPtr error;
	UriPtr target(g_uri_parse_relative(request->current.get(), location, kUriFlags, error.Out()));
	if (!target) {
		Finish(
		    request->id, HttpFailure::Network,
		    Message(_("Refused the redirect to %s: %s"), location, std::string(error.Message())));
		return;
	}
	const std::string text = UriText(target.get());
	if (request->redirects >= kMaxRedirects) {
		Finish(request->id, HttpFailure::Network,
		       Message(NGettext("Refused the redirect to %s: more than %s redirect",
		                        "Refused the redirect to %s: more than %s redirects",
		                        static_cast<unsigned long>(kMaxRedirects)),
		               text, std::to_string(kMaxRedirects)));
		return;
	}
	if (!Allowed(target.get())) {
		Finish(request->id, HttpFailure::Network,
		       Message(_("Refused the redirect to %s: only https URLs are allowed"), text));
		return;
	}
	if (request->request.redirects == RedirectPolicy::SameOrigin &&
	    !SameOrigin(request->requested.get(), target.get())) {
		Finish(request->id, HttpFailure::Network,
		       Message(_("Refused the redirect to %s: it leaves the origin of %s"), text,
		               UriText(request->requested.get())));
		return;
	}
	request->redirects++;
	request->current = std::move(target);
	Send(request);
}

void SoupHttpClient::Read(const RequestRef& request) {
	g_input_stream_read_bytes_async(request->stream.get(), kChunkSize, G_PRIORITY_DEFAULT,
	                                request->cancellable.get(), &OnRead, Hold(request));
}

void SoupHttpClient::OnRead(GObject* source, GAsyncResult* result, gpointer data) {
	const RequestRef request = Take(data);
	GErrorPtr        error;
	const BytesPtr   bytes(
	    g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, error.Out()));
	if (request->Ignored()) {
		// The read that was pending when the request ended: the stream can be closed now.
		CloseWithoutReading(G_INPUT_STREAM(source));
		return;
	}
	request->owner->OnChunk(request, bytes.get(), error);
}

void SoupHttpClient::OnChunk(const RequestRef& request, GBytes* bytes, const GErrorPtr& error) {
	if (bytes == nullptr) {
		Finish(request->id, HttpFailure::Network, std::string(error.Message()));
		return;
	}
	gsize       size = 0;
	const auto* data = static_cast<const std::byte*>(g_bytes_get_data(bytes, &size));
	if (size == 0) {
		Finish(request->id, HttpFailure::None, "");
		return;
	}
	const uint64_t max_bytes = request->request.max_bytes;
	if (max_bytes > 0 && request->received + size > max_bytes) {
		Finish(request->id, HttpFailure::TooLarge,
		       Message(NGettext("response body larger than %s byte",
		                        "response body larger than %s bytes", max_bytes),
		               std::to_string(max_bytes)));
		return;
	}
	request->received += size;
	ArmStallTimer(*request);
	// on_chunk may call Get() or Cancel(), or destroy this client: only request is used afterwards
	// until it shows that the client still exists.
	const bool keep_going = request->on_chunk ? request->on_chunk(std::span(data, size)) : true;
	if (request->owner == nullptr || request->finished) {
		return;
	}
	if (!keep_going) {
		Finish(request->id, HttpFailure::Cancelled, _("aborted by the chunk callback"));
		return;
	}
	if (request->cancel_queued) {
		return;
	}
	Read(request);
}

void SoupHttpClient::Finish(RequestId id, HttpFailure failure, std::string message) {
	const auto found = m_requests.find(id);
	if (found == m_requests.end()) {
		return;
	}
	const RequestRef request = std::move(found->second);
	m_requests.erase(found);
	request->finished = true;
	request->start_source.Remove();
	request->stall_source.Remove();
	request->total_source.Remove();
	request->cancel_source.Remove();
	// A pending GIO operation completes with G_IO_ERROR_CANCELLED and is ignored (finished).
	g_cancellable_cancel(request->cancellable.get());
	CloseWithoutReading(request->stream.get());
	request->stream.reset();
	const core::HttpResult result {failure, request->status, std::move(message)};
	DoneFn                 done = std::move(request->on_done);
	request->on_chunk           = nullptr;
	// done may destroy this client; nothing of it is used afterwards.
	if (done) {
		done(result);
	}
}

} // namespace kyty::gtk
