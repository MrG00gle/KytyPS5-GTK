#pragma once

// An HttpClient with scripted responses, delivered on a FakeScheduler's virtual clock. It keeps the
// contract of launcher/core/httpClient.h: Get() and Cancel() never call a callback, every request
// ends with exactly one DoneFn call, and all callbacks run from FakeScheduler::AdvanceBy() or
// RunUntilIdle(), so they are never re-entrant. No network access.
//
// Timeline of a request whose response is scripted (all times relative to Get()):
// - at latency: a network failure ends the request (HttpFailure::Network); a status outside 200-299
//   ends it with HttpFailure::Status and no body; otherwise the first body chunk arrives;
// - every chunk_interval after that: the next chunk; after the last chunk the request ends with
//   HttpFailure::None. An empty body ends the request at latency;
// - stall_after_bytes: the body stops after that many bytes and the request never ends by itself.
// The request limits apply on the same clock: more than max_bytes body bytes in total ends the
// request with TooLarge before the chunk that would exceed it is delivered, no body byte for
// stall_timeout gives Stalled, and total_timeout after Get() gives TimedOut (zero limits are off).
// A ChunkFn that returns false ends the request with Cancelled right after it returns. Cancel()
// ends a pending request with Cancelled at the current virtual time, from a scheduled timer.

#include "launcher/core/httpClient.h"
#include "launcher/core/scheduler.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace KytyTests {

class FakeScheduler;

struct FakeResponse {
	int                       status = 200;
	std::string               body;
	std::vector<size_t>       chunk_sizes; // sizes of the first chunks; the rest is one chunk
	std::chrono::milliseconds latency {0}; // from Get() to the first chunk or the failure
	std::chrono::milliseconds chunk_interval {0};
	std::optional<size_t>     stall_after_bytes;
	bool                      network_failure = false;
	std::string               failure_message = "simulated network failure";
};

class FakeHttpClient final: public kyty::core::HttpClient {
public:
	explicit FakeHttpClient(FakeScheduler& scheduler);
	~FakeHttpClient() override;

	FakeHttpClient(const FakeHttpClient&)            = delete;
	FakeHttpClient& operator=(const FakeHttpClient&) = delete;

	// Queues a response for url. Requests for a url take its responses in FIFO order; a request
	// without a scripted response fails with HttpFailure::Network.
	void AddResponse(const std::string& url, FakeResponse response);

	kyty::core::RequestId Get(const kyty::core::HttpRequest& request, ChunkFn on_chunk,
	                          DoneFn on_done) override;
	void                  Cancel(kyty::core::RequestId id) override;

	// Every request passed to Get(), in call order.
	[[nodiscard]] const std::vector<kyty::core::HttpRequest>& Requests() const {
		return m_requests;
	}

	// Requests that have not ended yet.
	[[nodiscard]] size_t ActiveCount() const { return m_active.size(); }

	// Scripted responses that no request has taken yet.
	[[nodiscard]] size_t UnusedResponseCount() const;

private:
	struct Active {
		kyty::core::HttpRequest request;
		FakeResponse            response;
		ChunkFn                 on_chunk;
		DoneFn                  on_done;
		size_t                  delivered        = 0;
		size_t                  next_chunk       = 0;
		kyty::core::TimerId     event_timer      = 0;
		kyty::core::TimerId     stall_timer      = 0;
		kyty::core::TimerId     total_timer      = 0;
		bool                    headers_received = false;
		bool                    cancel_queued    = false;
	};

	Active& Lookup(kyty::core::RequestId id);
	void    Start(kyty::core::RequestId id);
	void    DeliverNext(kyty::core::RequestId id);
	void    ArmStallTimer(Active& active, kyty::core::RequestId id);
	void    CancelTimers(Active& active);
	void    Finish(kyty::core::RequestId id, kyty::core::HttpFailure failure, std::string message);

	FakeScheduler&                                  m_scheduler;
	kyty::core::RequestId                           m_next_id = 1;
	std::map<std::string, std::deque<FakeResponse>> m_responses;
	std::map<kyty::core::RequestId, Active>         m_active;
	std::vector<kyty::core::HttpRequest>            m_requests;
};

} // namespace KytyTests
