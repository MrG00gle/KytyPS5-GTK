#pragma once

// RapidCheck generators for the title sound selection property (Property 11, task 14.3). Header
// only, and only for property TUs: RapidCheck needs exceptions, which the test support library is
// built without. The generator functions draw values with operator* on generators, so call them
// only inside a property.
//
// An event sequence mixes selections of a few entries (so the same entry comes back often),
// cleared selections, decode completions, run starts, and game exits. A completion names the
// request it belongs to by a random pick that the property maps onto the requests issued so far
// (often the latest, often an earlier one) or onto a generation that was never requested, and
// carries a success or one of the TitleSoundError values.

#include "launcher/core/titleSound.h"
#include "launcher/core/titleSoundController.h"

#include <cstddef>
#include <cstdint>
#include <rapidcheck.h>
#include <string>
#include <vector>

namespace KytyTests::TitleSoundGen {

// A uniform choice from [min, max). rc::gen::inRange alone narrows its range at small sizes.
template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

inline bool OneIn(int n) {
	return Choose(0, n) == 0;
}

// The entries a sequence selects: a directory, an archive, and a third directory.
inline const std::vector<kyty::core::EntryId>& Entries() {
	static const std::vector<kyty::core::EntryId> entries = {
	    "/games/Alpha", "/games/archives/Beta.zar", "/games/Gamma (EU)"};
	return entries;
}

enum class EventKind { Select, Clear, DecodeFinished, RunStarted, GameExited };

struct Event {
	EventKind kind  = EventKind::Select;
	size_t    entry = 0; // Select: index into Entries()
	// DecodeFinished: which request; 0-5 the latest, the one before it, ..., 6 and 7 a generation
	// that was never requested.
	int                         pick    = 0;
	bool                        success = true;
	kyty::core::TitleSoundError error   = kyty::core::TitleSoundError::Decode;
};

inline const char* EventName(EventKind kind) {
	switch (kind) {
		case EventKind::Select: return "select";
		case EventKind::Clear: return "clear";
		case EventKind::DecodeFinished: return "decode finished";
		case EventKind::RunStarted: return "run started";
		case EventKind::GameExited: return "game exited";
	}
	return "?";
}

inline Event GenerateEvent() {
	static const std::vector<kyty::core::TitleSoundError> kErrors = {
	    kyty::core::TitleSoundError::Absent,   kyty::core::TitleSoundError::Unreadable,
	    kyty::core::TitleSoundError::TooLarge, kyty::core::TitleSoundError::InvalidHeader,
	    kyty::core::TitleSoundError::Decode,   kyty::core::TitleSoundError::Cancelled};
	Event     event;
	const int pick = Choose(0, 20);
	event.kind     = pick < 7    ? EventKind::Select
	                 : pick < 9  ? EventKind::Clear
	                 : pick < 16 ? EventKind::DecodeFinished
	                 : pick < 18 ? EventKind::RunStarted
	                             : EventKind::GameExited;
	event.entry    = Choose<size_t>(0, Entries().size());
	event.pick     = OneIn(2) ? 0 : Choose(0, 8);
	event.success  = !OneIn(4);
	event.error    = kErrors[Choose<size_t>(0, kErrors.size())];
	return event;
}

// 0-60 events.
inline std::vector<Event> GenerateEvents() {
	std::vector<Event> events;
	const int          count = Choose(0, 61);
	for (int i = 0; i < count; i++) {
		events.push_back(GenerateEvent());
	}
	return events;
}

} // namespace KytyTests::TitleSoundGen
