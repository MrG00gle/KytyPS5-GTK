// Feature: linux-gtk-modernization, Property 13: Controller value clamping
//
// Validates Requirement 9.12. For 64-bit integers v, i, k (uniform over the whole int64_t range, a
// small range around the limits, or the edges INT64_MIN, -1, 0, 1, 4, 5, 99, 100, 101, INT64_MAX):
// ClampControllerSettings(v, i, k) gives speaker volume clamp(v, 0, 100), vibration intensity
// clamp(i, 0, 100), and keyboard player clamp(k, 0, 4), so in-range values are unchanged. The
// controller field descriptors and a Kyty.ini that stores the three values clamp them the same way,
// without listing them as defaulted.

#include "launcher/core/configurationFields.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/iniFile.h"
#include "launcher/core/settingsStore.h"
#include "support/configurationGenerators.h"
#include "support/property.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <rapidcheck.h>
#include <string>
#include <vector>

namespace {

using namespace KytyTests::ConfigGen;

int64_t Value() {
	static const std::vector<int64_t> edges = {
	    std::numeric_limits<int64_t>::min(), -1, 0, 1, 4, 5, 99, 100, 101,
	    std::numeric_limits<int64_t>::max()};
	switch (Choose(0, 3)) {
		case 0: return *rc::gen::resize(100, rc::gen::arbitrary<int64_t>());
		case 1: return PickOf(edges);
		default: return Choose<int64_t>(-200, 201);
	}
}

} // namespace

KYTY_PROPERTY(13, "Controller value clamping") {
	return rc::check(name, []() {
		const int64_t v = Value();
		const int64_t i = Value();
		const int64_t k = Value();
		RC_LOG() << "v=" << v << " i=" << i << " k=" << k << "\n";

		const auto clamped = kyty::core::ClampControllerSettings(v, i, k);
		RC_ASSERT(clamped.speaker_volume == std::clamp<int64_t>(v, 0, 100));
		RC_ASSERT(clamped.vibration_intensity == std::clamp<int64_t>(i, 0, 100));
		RC_ASSERT(clamped.keyboard_player == std::clamp<int64_t>(k, 0, 4));
		if (v >= 0 && v <= 100) {
			RC_ASSERT(clamped.speaker_volume == v);
		}
		if (i >= 0 && i <= 100) {
			RC_ASSERT(clamped.vibration_intensity == i);
		}
		if (k >= 0 && k <= 4) {
			RC_ASSERT(clamped.keyboard_player == k);
		}

		// The global-only field descriptors.
		kyty::core::ControllerSettings settings;
		const auto                     fields = kyty::core::GlobalOnlyFields();
		RC_ASSERT(fields[1].set(settings, kyty::core::FieldValue(v)));
		RC_ASSERT(fields[2].set(settings, kyty::core::FieldValue(i)));
		RC_ASSERT(fields[3].set(settings, kyty::core::FieldValue(k)));
		RC_ASSERT(settings.speaker_volume == clamped.speaker_volume);
		RC_ASSERT(settings.vibration_intensity == clamped.vibration_intensity);
		RC_ASSERT(settings.keyboard_player == clamped.keyboard_player);

		// A stored Kyty.ini value.
		auto file = kyty::core::IniFile::Parse(
		    kyty::core::SerializeSettings(kyty::core::LauncherSettings {}));
		file.SetString("GlobalConfiguration/controller_speaker_volume", std::to_string(v));
		file.SetString("GlobalConfiguration/controller_vibration_intensity", std::to_string(i));
		file.SetString("GlobalConfiguration/controller_keyboard_player", std::to_string(k));
		const auto report = kyty::core::ParseSettings(file.Serialize());
		RC_ASSERT(report.defaulted_keys.empty());
		RC_ASSERT(report.settings.controller.speaker_volume == clamped.speaker_volume);
		RC_ASSERT(report.settings.controller.vibration_intensity == clamped.vibration_intensity);
		RC_ASSERT(report.settings.controller.keyboard_player == clamped.keyboard_player);

		RC_TAG(v < 0 || v > 100 || i < 0 || i > 100 || k < 0 || k > 4 ? "some value clamped"
		                                                              : "all in range");
	});
}
