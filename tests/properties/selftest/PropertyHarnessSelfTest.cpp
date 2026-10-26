// Property 0 of property_harness_selftest_tests fails on purpose. The CTest
// property_harness_selftest (tests/cmake/PropertyHarnessSelfTest.cmake) runs it twice and checks
// that the failure report names a seed and that rerunning with that seed reproduces the same shrunk
// counterexample (Requirement 17.9).

#include "support/property.h"

#include <vector>

// Feature: linux-gtk-modernization, Property 0: a failing property reports a reproducible seed
KYTY_PROPERTY(0, "a failing property reports a reproducible seed") {
	// Fails for every vector with 3 or more elements; shrinks to [0, 0, 0].
	return rc::check(name, [](const std::vector<int>& values) { RC_ASSERT(values.size() < 3U); });
}
