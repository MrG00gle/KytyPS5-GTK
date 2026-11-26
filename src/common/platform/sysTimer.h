#ifndef KYTY_COMMON_PLATFORM_SYSTIMER_H_
#define KYTY_COMMON_PLATFORM_SYSTIMER_H_

#include "common/common.h"

#include <algorithm>
#include <cstdint>
#include <ctime>


struct SysFileTimeStruct {
#if   KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	// Nanoseconds preserve sub-second file timestamps.
	time_t time;
	long   nanos;
#endif
	bool is_invalid;
};

#if   KYTY_PLATFORM == KYTY_PLATFORM_LINUX

inline int64_t SysFileTimeToNs(const SysFileTimeStruct& f) {
	return int64_t {f.time} * 1000000000 + f.nanos;
}

#endif

#endif /* KYTY_COMMON_PLATFORM_SYSTIMER_H_ */
