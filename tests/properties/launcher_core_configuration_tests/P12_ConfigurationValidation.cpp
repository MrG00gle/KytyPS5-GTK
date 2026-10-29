// Feature: linux-gtk-modernization, Property 12: Configuration validation
//
// Validates Requirement 9.4. A valid configuration (configurationGenerators.h) gets, in two thirds
// of the cases, each rule broken with probability 1/4; a field whose rule is not broken is set to
// a valid value at its boundary half of the time: the user name (empty, whitespace only, or 17+
// UTF-8 bytes after trimming, against 1 and 16 bytes with surrounding whitespace), the user ID (-1,
// 254, 255, 2,147,482,880, the int32 limits, against 0, 253, 256, 2,147,482,879), vblank 29/361
// against 30/360, trophy 4/11 against 5/10, an empty output path while shader logs or printf output
// go to a File or dumps are enabled (against an empty path while the output is off), and the
// further Validate() rules: gpu_index below -1, console_language outside 0-29, an enum value
// without a key, an elf that is empty, absolute, or has a ".." component, and an input mapping
// entry without both sides. Validate() must return an error exactly when a rule is broken, naming
// the first broken field in table order; each field's own check (or the output path rule) must fail
// exactly when its rule is broken; and the valid configuration with only one of the broken rules
// must fail on that field.

#include "launcher/core/configurationFields.h"
#include "launcher/core/gameConfiguration.h"
#include "support/configurationGenerators.h"
#include "support/property.h"

#include <cstdint>
#include <limits>
#include <rapidcheck.h>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace KytyTests::ConfigGen;
using kyty::core::GameConfiguration;
using kyty::core::LogDirection;

constexpr int32_t kInt32Min = std::numeric_limits<int32_t>::min();
constexpr int32_t kInt32Max = std::numeric_limits<int32_t>::max();

std::string LongUserName() {
	// 17-24 bytes after trimming, mixing 1-, 2-, 3-, and 4-byte characters.
	std::string name = VisibleText(1);
	while (name.size() < kyty::core::kMaxUserNameBytes + 1) {
		name += Utf8(PickOf(Visible()));
	}
	if (OneIn(2)) {
		name = Whitespace() + name + Whitespace();
	}
	return name;
}

// Breaks or keeps each rule; returns the fields whose rule is broken.
std::set<std::string> Mutate(GameConfiguration& c) {
	std::set<std::string> broken;
	// A third of the cases break no rule (only boundary values), so that both outcomes are common.
	const bool keep_valid = OneIn(3);
	const auto decide     = [&](const char* field) {
		const bool breaks = !keep_valid && OneIn(4);
		if (breaks) {
			broken.insert(field);
		}
		return breaks;
	};
	const auto edge = [] { return OneIn(2); };

	if (decide("screen_resolution")) {
		c.screen_resolution = static_cast<kyty::core::ScreenResolution>(OneIn(2) ? 4 : -1);
	}
	if (decide("user_name")) {
		c.user_name = PickOf(std::vector<std::string> {"", " ", "\t\xE3\x80\x80", LongUserName()});
	} else if (edge()) {
		c.user_name =
		    Whitespace() + (OneIn(2) ? std::string("K") : std::string(16, 'k')) + Whitespace();
	}
	if (decide("user_id")) {
		c.user_id = PickOf(
		    std::vector<int32_t> {-1, -1, 254, 255, 2147482880, 2147482880, kInt32Max, kInt32Min});
	} else if (edge()) {
		c.user_id = PickOf(std::vector<int32_t> {0, 253, 256, kyty::core::kMaxUserId});
	}
	if (decide("present_mode")) {
		c.present_mode = static_cast<kyty::core::PresentMode>(OneIn(2) ? 3 : 100);
	}
	if (decide("gpu_index")) {
		c.gpu_index = OneIn(2) ? -2 : Choose<int32_t>(kInt32Min, -1);
	} else if (edge()) {
		c.gpu_index = PickOf(std::vector<int32_t> {-1, 0, kInt32Max});
	}
	if (decide("trophy_duration_seconds")) {
		c.trophy_duration_seconds = PickOf(std::vector<int32_t> {4, 4, 11, 11, 0, kInt32Max});
	} else if (edge()) {
		c.trophy_duration_seconds = OneIn(2) ? 5 : 10;
	}
	if (decide("vblank_frequency")) {
		c.vblank_frequency = PickOf(std::vector<int32_t> {29, 29, 361, 361, 0, kInt32Min});
	} else if (edge()) {
		c.vblank_frequency = OneIn(2) ? 30 : 360;
	}
	if (decide("console_language")) {
		c.console_language = PickOf(std::vector<int32_t> {-1, -1, 30, 30, kInt32Max});
	} else if (edge()) {
		c.console_language = OneIn(2) ? 0 : kyty::core::kMaxConsoleLanguage;
	}
	if (decide("shader_log_folder")) {
		c.shader_log_direction = LogDirection::File;
		c.shader_log_folder    = "";
	} else if (edge()) {
		c.shader_log_direction = OneIn(2) ? LogDirection::Silent : LogDirection::Console;
		c.shader_log_folder    = "";
	}
	if (decide("command_buffer_dump_folder")) {
		c.command_buffer_dump_enabled = true;
		c.command_buffer_dump_folder  = "";
	} else if (edge()) {
		c.command_buffer_dump_enabled = false;
		c.command_buffer_dump_folder  = "";
	}
	if (decide("printf_output_file")) {
		c.printf_direction   = LogDirection::File;
		c.printf_output_file = "";
	} else if (edge()) {
		c.printf_direction   = OneIn(2) ? LogDirection::Silent : LogDirection::Console;
		c.printf_output_file = "";
	}
	if (decide("elf")) {
		c.elf = PickOf(std::vector<std::string> {"", "/eboot.bin", "..", "../x", "a/../b", "a/.."});
	}
	if (decide("host_input_mapping")) {
		const auto bad = PickOf(std::vector<std::string> {"Cross", "=J", "Cross=", " = ", ""});
		c.host_input_mapping.insert(
		    c.host_input_mapping.begin() + Choose<size_t>(0, c.host_input_mapping.size() + 1), bad);
	}
	return broken;
}

// The output field that a path rule depends on.
std::string OutputOf(std::string_view path) {
	if (path == "shader_log_folder") {
		return "shader_log_direction";
	}
	if (path == "command_buffer_dump_folder") {
		return "command_buffer_dump_enabled";
	}
	if (path == "printf_output_file") {
		return "printf_direction";
	}
	return {};
}

} // namespace

KYTY_PROPERTY(12, "Configuration validation") {
	return rc::check(name, []() {
		const GameConfiguration valid  = ValidConfiguration();
		GameConfiguration       c      = valid;
		const auto              broken = Mutate(c);
		const auto              error  = kyty::core::Validate(c);
		RC_LOG() << "broken:";
		for (const auto& field: broken) {
			RC_LOG() << " " << field;
		}
		RC_LOG() << "\n";
		if (broken.empty()) {
			RC_ASSERT(!error.has_value());
		} else {
			RC_ASSERT(error.has_value());
			std::string first;
			for (const auto& field: kyty::core::GameConfigurationFields()) {
				if (broken.contains(std::string(field.name))) {
					first = std::string(field.name);
					break;
				}
			}
			RC_LOG() << "error: " << error->field << ": " << error->message << "\n";
			RC_ASSERT(error->field == first);
			RC_ASSERT(!error->message.empty());
		}

		// Each rule on its own: a field check passes exactly when its rule is kept, and the valid
		// configuration with only one broken rule (the field, and the output a path rule needs)
		// fails on that field.
		for (const auto& field: kyty::core::GameConfigurationFields()) {
			const std::string field_name(field.name);
			const bool        is_broken = broken.contains(field_name);
			const auto        output    = OutputOf(field.name);
			if (output.empty() && field_name != "host_input_mapping") {
				RC_LOG() << "check " << field_name << "\n";
				RC_ASSERT(field.check(field.get(c)).has_value() == is_broken);
			} else if (!output.empty()) {
				RC_ASSERT(kyty::core::IsMissingOutputPath(field.name, c) == is_broken);
			}
			if (!is_broken) {
				continue;
			}
			GameConfiguration single = valid;
			RC_ASSERT(field.set(single, field.get(c)) || field.kind == kyty::core::FieldKind::Enum);
			if (field.kind == kyty::core::FieldKind::Enum) {
				single = c; // an enum value without a key cannot be set(); start from c instead
				for (const auto& other: kyty::core::GameConfigurationFields()) {
					if (other.name != field.name && broken.contains(std::string(other.name))) {
						RC_ASSERT(other.set(single, other.get(valid)));
						if (const auto other_output = OutputOf(other.name); !other_output.empty()) {
							const auto* output_field =
							    kyty::core::FindGameConfigurationField(other_output);
							RC_ASSERT(output_field->set(single, output_field->get(valid)));
						}
					}
				}
			}
			if (!output.empty()) {
				const auto* output_field = kyty::core::FindGameConfigurationField(output);
				RC_ASSERT(output_field->set(single, output_field->get(c)));
			}
			const auto single_error = kyty::core::Validate(single);
			RC_LOG() << "only " << field_name << " broken\n";
			RC_ASSERT(single_error.has_value());
			RC_ASSERT(single_error->field == field_name);
		}
		RC_TAG(broken.empty()       ? "valid"
		       : broken.size() == 1 ? "one rule broken"
		                            : "several broken");
	});
}
