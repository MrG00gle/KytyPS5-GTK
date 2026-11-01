#pragma once

// Work off the main thread for the content windows (tasks 21.3, 21.5; plan D29, D44; design.md,
// "Concurrency and lifetime"): reading trophy packages, listing add-ons, and packing an archive
// run in a GTask thread, and their results are applied on the main thread.

#include <functional>

namespace kyty::gtk {

// Runs work in a GTask thread, then done from the main loop of the calling thread's main context
// (call it on the main thread) once work has returned. Both run exactly once. There is no
// cancellation: the caller gives work a core::CancelToken of its own where it can stop early, and
// done checks whether the caller still wants the result (a liveness flag and a generation number
// held in shared_ptrs, never a raw pointer that done dereferences unchecked). The functions and
// what they capture may be destroyed on either thread, so they must not hold widget references.
void RunInBackground(std::function<void()> work, std::function<void()> done);

} // namespace kyty::gtk
