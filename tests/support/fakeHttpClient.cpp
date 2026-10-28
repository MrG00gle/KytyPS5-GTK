#include "support/fakeHttpClient.h"

#include "support/fakeScheduler.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <utility>

namespace KytyTests {

using kyty::core::HttpFailure;
using kyty::core::RequestId;

FakeHttpClient::FakeHttpClient(FakeScheduler& scheduler): m_scheduler(scheduler) {}

FakeHttpClient::~FakeHttpClient() {
	// The scheduler may outlive this client: no timer may call back into it.
	for (auto& entry: m_active) {
		CancelTimers(entry.second);
	}
}

void FakeHttpClient::AddResponse(const std::string& url, FakeResponse response) {
	m_responses[url].push_back(std::move(response));
}

size_t FakeHttpClient::UnusedResponseCount() const {
	size_t count = 0;
	for (const auto& entry: m_responses) {
		count += entry.second.size();
	}
	return count;
}

RequestId FakeHttpClient::Get(const kyty::core::HttpRequest& request, ChunkFn on_chunk,
                              DoneFn on_done) {
	const RequestId id = m_next_id++;
	m_requests.push_back(request);

	Active active;
	active.request  = request;
	active.on_chunk = std::move(on_chunk);
	active.on_done  = std::move(on_done);
	auto scripted   = m_responses.find(request.url);
	if (scripted != m_responses.end() && !scripted->second.empty()) {
		active.response = std::move(scripted->second.front());
		scripted->second.pop_front();
	} else {
		active.response.network_failure = true;
		active.response.failure_message = "no scripted response for " + request.url;
	}

	auto& stored = m_active.emplace(id, std::move(active)).first->second;
	if (const auto timeout = request.total_timeout; timeout.count() > 0) {
		stored.total_timer = m_scheduler.AfterUnrecorded(timeout, [this, id, timeout] {
			Finish(id, HttpFailure::TimedOut,
			       "timed out after " + std::to_string(timeout.count()) + " ms");
		});
	}
	ArmStallTimer(stored, id);
	stored.event_timer =
	    m_scheduler.AfterUnrecorded(stored.response.latency, [this, id] { Start(id); });
	return id;
}

void FakeHttpClient::Cancel(RequestId id) {
	const auto found = m_active.find(id);
	if (found == m_active.end() || found->second.cancel_queued) {
		return;
	}
	auto& active         = found->second;
	active.cancel_queued = true;
	CancelTimers(active);
	active.event_timer = m_scheduler.AfterUnrecorded(std::chrono::milliseconds {0}, [this, id] {
		Finish(id, HttpFailure::Cancelled, "cancelled");
	});
}

FakeHttpClient::Active& FakeHttpClient::Lookup(RequestId id) {
	const auto found = m_active.find(id);
	if (found == m_active.end()) {
		// Finish() cancels every timer of a request, so its events never run afterwards.
		std::fprintf(stderr, "FakeHttpClient: event for the finished request %llu\n",
		             static_cast<unsigned long long>(id));
		std::abort();
	}
	return found->second;
}

void FakeHttpClient::Start(RequestId id) {
	auto& active       = Lookup(id);
	active.event_timer = 0;
	if (active.response.network_failure) {
		Finish(id, HttpFailure::Network, active.response.failure_message);
		return;
	}
	active.headers_received = true;
	if (active.response.status < 200 || active.response.status > 299) {
		Finish(id, HttpFailure::Status, "HTTP status " + std::to_string(active.response.status));
		return;
	}
	DeliverNext(id);
}

void FakeHttpClient::DeliverNext(RequestId id) {
	auto& active       = Lookup(id);
	active.event_timer = 0;
	const auto& body   = active.response.body;
	if (active.delivered >= body.size()) {
		Finish(id, HttpFailure::None, "");
		return;
	}
	const size_t limit =
	    std::min(body.size(), active.response.stall_after_bytes.value_or(body.size()));
	if (active.delivered >= limit) {
		return; // stalled: only a limit or Cancel() ends the request
	}

	auto& sizes = active.response.chunk_sizes;
	while (active.next_chunk < sizes.size() && sizes[active.next_chunk] == 0) {
		active.next_chunk++;
	}
	size_t size = body.size() - active.delivered;
	if (active.next_chunk < sizes.size()) {
		size = std::min(size, sizes[active.next_chunk]);
		active.next_chunk++;
	}
	size = std::min(size, limit - active.delivered);

	const uint64_t max_bytes = active.request.max_bytes;
	if (max_bytes > 0 && active.delivered + size > max_bytes) {
		Finish(id, HttpFailure::TooLarge,
		       "response body larger than " + std::to_string(max_bytes) + " bytes");
		return;
	}
	const auto chunk = std::as_bytes(std::span(body.data() + active.delivered, size));
	active.delivered += size;
	ArmStallTimer(active, id);
	// The callback may call Get() or Cancel(), which never remove this entry, so active stays
	// valid.
	const bool keep_going = active.on_chunk ? active.on_chunk(chunk) : true;
	if (!keep_going) {
		Finish(id, HttpFailure::Cancelled, "aborted by the chunk callback");
		return;
	}
	if (active.cancel_queued) {
		return;
	}
	if (active.delivered >= body.size()) {
		Finish(id, HttpFailure::None, "");
		return;
	}
	if (active.delivered < limit) {
		active.event_timer = m_scheduler.AfterUnrecorded(active.response.chunk_interval,
		                                                 [this, id] { DeliverNext(id); });
	}
}

void FakeHttpClient::ArmStallTimer(Active& active, RequestId id) {
	const auto timeout = active.request.stall_timeout;
	if (timeout.count() <= 0) {
		return;
	}
	m_scheduler.Cancel(active.stall_timer);
	active.stall_timer = m_scheduler.AfterUnrecorded(timeout, [this, id, timeout] {
		Finish(id, HttpFailure::Stalled, "no data for " + std::to_string(timeout.count()) + " ms");
	});
}

void FakeHttpClient::CancelTimers(Active& active) {
	for (auto* timer: {&active.event_timer, &active.stall_timer, &active.total_timer}) {
		m_scheduler.Cancel(*timer);
		*timer = 0;
	}
}

void FakeHttpClient::Finish(RequestId id, HttpFailure failure, std::string message) {
	const auto found = m_active.find(id);
	if (found == m_active.end()) {
		return;
	}
	Active active = std::move(found->second);
	m_active.erase(found);
	CancelTimers(active);
	const kyty::core::HttpResult result {
	    failure, active.headers_received ? active.response.status : 0, std::move(message)};
	if (active.on_done) {
		active.on_done(result);
	}
}

} // namespace KytyTests
