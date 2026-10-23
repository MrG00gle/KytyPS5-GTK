#include "common/hostStats.h"

#include "common/config.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_map>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/resource.h>
#include <unistd.h>

namespace Common {

namespace {

// Cumulative counters; rates come from the difference between two samples.
struct Counters {
	struct Thread {
		uint64_t    cpu_ns;
		std::string name;
	};
	std::optional<uint64_t>              process_cpu_ns;
	std::optional<uint64_t>              system_busy;
	std::optional<uint64_t>              system_total;
	std::optional<uint64_t>              context_switches;
	std::optional<uint64_t>              page_faults;
	std::optional<uint64_t>              page_ins;
	std::optional<uint64_t>              disk_read;
	std::optional<uint64_t>              disk_write;
	std::unordered_map<uint64_t, Thread> threads;
	uint64_t                             wall_ns = 0;
};

uint64_t WallNanoseconds() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}


void SampleRusage(Counters& now) {
	rusage usage {};
	if (getrusage(RUSAGE_SELF, &usage) != 0) {
		return;
	}
	const auto to_ns = [](const timeval& time) {
		return static_cast<uint64_t>(time.tv_sec) * 1000000000u +
		       static_cast<uint64_t>(time.tv_usec) * 1000u;
	};
	now.process_cpu_ns   = to_ns(usage.ru_utime) + to_ns(usage.ru_stime);
	now.context_switches = static_cast<uint64_t>(usage.ru_nvcsw + usage.ru_nivcsw);
	now.page_faults      = static_cast<uint64_t>(usage.ru_minflt + usage.ru_majflt);
	now.page_ins         = static_cast<uint64_t>(usage.ru_majflt);
}


using ProcValues = std::unordered_map<std::string, uint64_t>;

// Reads "<key>: <number> [kB]" lines; kB values are returned in bytes.
ProcValues ReadProcValues(const char* path) {
	ProcValues    values;
	std::ifstream file(path);
	std::string   line;
	while (std::getline(file, line)) {
		const auto colon = line.find(':');
		if (colon == std::string::npos) {
			continue;
		}
		std::istringstream stream(line.substr(colon + 1));
		uint64_t           number = 0;
		std::string        unit;
		if (stream >> number) {
			stream >> unit;
			values[line.substr(0, colon)] = unit == "kB" ? number * 1024u : number;
		}
	}
	return values;
}

std::optional<uint64_t> Find(const ProcValues& values, const char* key) {
	const auto it = values.find(key);
	return it != values.end() ? std::optional(it->second) : std::nullopt;
}

struct PlatformState {};

void SampleThreads(HostStats& stats, Counters& now) {
	static const double ns_per_tick = 1e9 / static_cast<double>(std::max(sysconf(_SC_CLK_TCK), 1L));
	std::error_code     error;
	uint32_t            count = 0;
	for (std::filesystem::directory_iterator it("/proc/self/task", error), end; !error && it != end;
	     it.increment(error)) {
		count++;
		std::ifstream stat_file(it->path() / "stat");
		std::string   stat;
		uint64_t      id = 0;
		if (!(stat_file >> id)) {
			continue; // The thread may have exited during enumeration.
		}
		std::getline(stat_file, stat);
		const auto close = stat.rfind(')');
		if (close == std::string::npos) {
			continue;
		}
		// Fields after the command name start at field 3 (state); utime and stime are 14 and 15.
		std::istringstream fields(stat.substr(close + 2));
		std::string        field;
		for (int index = 3; index < 14 && fields >> field; index++) {
		}
		uint64_t user   = 0;
		uint64_t system = 0;
		if (!(fields >> user >> system)) {
			continue;
		}
		auto& thread  = now.threads[id];
		thread.cpu_ns = static_cast<uint64_t>(static_cast<double>(user + system) * ns_per_tick);
		std::ifstream name_file(it->path() / "comm");
		std::getline(name_file, thread.name);
	}
	if (!error) {
		stats.thread_count = count;
	}
}

void SamplePlatform(PlatformState& /*state*/, HostStats& stats, Counters& now) {
	SampleRusage(now);

	std::ifstream cpu_file("/proc/stat");
	std::string   label;
	if (cpu_file >> label && label == "cpu") {
		uint64_t total = 0;
		uint64_t idle  = 0;
		uint64_t value = 0;
		for (int index = 0; index < 8 && cpu_file >> value; index++) {
			total += value;
			if (index == 3 || index == 4) { // idle, iowait
				idle += value;
			}
		}
		now.system_total = total;
		now.system_busy  = total - idle;
	}

	const auto status         = ReadProcValues("/proc/self/status");
	stats.process_memory      = Find(status, "VmRSS");
	stats.process_resident    = Find(status, "VmRSS");
	stats.process_file_backed = Find(status, "RssFile");
	stats.process_compressed  = Find(status, "VmSwap");
	stats.process_virtual     = Find(status, "VmSize");

	const auto io  = ReadProcValues("/proc/self/io");
	now.disk_read  = Find(io, "read_bytes");
	now.disk_write = Find(io, "write_bytes");

	const auto memory    = ReadProcValues("/proc/meminfo");
	const auto total     = Find(memory, "MemTotal");
	const auto available = Find(memory, "MemAvailable");
	if (total && available && *available <= *total) {
		stats.system_memory_total = total;
		stats.system_memory_used  = *total - *available;
	}
	stats.system_free = Find(memory, "MemFree");
	if (const auto cached = Find(memory, "Cached")) {
		stats.system_cached = *cached + Find(memory, "Buffers").value_or(0);
	}
	const auto swap_total = Find(memory, "SwapTotal");
	const auto swap_free  = Find(memory, "SwapFree");
	if (swap_total && swap_free && *swap_free <= *swap_total) {
		stats.swap_total = swap_total;
		stats.swap_used  = *swap_total - *swap_free;
	}

	// Pressure stall information: share of the last 10 s some task waited on memory.
	std::ifstream pressure("/proc/pressure/memory");
	std::string   kind;
	std::string   average;
	double        stalled = 0.0;
	if (pressure >> kind >> average && kind == "some" && average.rfind("avg10=", 0) == 0 &&
	    std::istringstream(average.substr(6)) >> stalled) {
		stats.memory_pressure = stalled >= 20.0  ? MemoryPressure::Critical
		                        : stalled >= 5.0 ? MemoryPressure::Warning
		                                         : MemoryPressure::Normal;
	} else if (total && available && *total != 0) {
		const double free_share = static_cast<double>(*available) / static_cast<double>(*total);
		stats.memory_pressure   = free_share < 0.10   ? MemoryPressure::Critical
		                          : free_share < 0.25 ? MemoryPressure::Warning
		                                              : MemoryPressure::Normal;
	}

	SampleThreads(stats, now);
}


constexpr size_t BUSIEST_THREADS = 6;

} // namespace

struct HostStatsSampler::Impl {
	PlatformState state;
	Counters      last;
	uint32_t      logical_cores = std::max(std::thread::hardware_concurrency(), 1u);
	bool          has_last      = false;
};

HostStatsSampler::HostStatsSampler(): m_impl(std::make_unique<Impl>()) {}

HostStatsSampler::~HostStatsSampler() = default;

HostStats HostStatsSampler::Sample() {
	HostStats stats;
	stats.logical_cores = m_impl->logical_cores;

	Counters now;
	now.wall_ns = WallNanoseconds();
	SamplePlatform(m_impl->state, stats, now);

	const auto& last    = m_impl->last;
	const auto  elapsed = static_cast<double>(now.wall_ns - last.wall_ns) / 1e9;
	if (m_impl->has_last && elapsed > 0.0) {
		const auto rate = [&](std::optional<uint64_t> current,
		                      std::optional<uint64_t> previous) -> std::optional<double> {
			if (!current || !previous || *current < *previous) {
				return std::nullopt;
			}
			return static_cast<double>(*current - *previous) / elapsed;
		};
		if (const auto cpu = rate(now.process_cpu_ns, last.process_cpu_ns)) {
			stats.process_cpu_percent =
			    std::clamp(*cpu / 1e9 * 100.0 / stats.logical_cores, 0.0, 100.0);
		}
		if (now.system_total && last.system_total && now.system_busy && last.system_busy &&
		    *now.system_total > *last.system_total && *now.system_busy >= *last.system_busy) {
			stats.system_cpu_percent =
			    std::clamp(static_cast<double>(*now.system_busy - *last.system_busy) /
			                   static_cast<double>(*now.system_total - *last.system_total) * 100.0,
			               0.0, 100.0);
		}
		stats.context_switches_per_s = rate(now.context_switches, last.context_switches);
		stats.page_faults_per_s      = rate(now.page_faults, last.page_faults);
		stats.page_ins_per_s         = rate(now.page_ins, last.page_ins);
		stats.disk_read_per_s        = rate(now.disk_read, last.disk_read);
		stats.disk_write_per_s       = rate(now.disk_write, last.disk_write);

		for (const auto& [id, thread]: now.threads) {
			// A thread missing from the last sample started during this interval.
			const auto     previous = last.threads.find(id);
			const uint64_t before   = previous != last.threads.end() ? previous->second.cpu_ns : 0;
			if (thread.cpu_ns < before) {
				continue;
			}
			const double percent =
			    static_cast<double>(thread.cpu_ns - before) / 1e9 / elapsed * 100.0;
			stats.busiest_threads.push_back(
			    {thread.name.empty() ? "thread " + std::to_string(id) : thread.name, percent});
		}
		std::sort(stats.busiest_threads.begin(), stats.busiest_threads.end(),
		          [](const auto& a, const auto& b) { return a.cpu_percent > b.cpu_percent; });
		if (stats.busiest_threads.size() > BUSIEST_THREADS) {
			stats.busiest_threads.resize(BUSIEST_THREADS);
		}
	}
	m_impl->last     = std::move(now);
	m_impl->has_last = true;
	return stats;
}

} // namespace Common
