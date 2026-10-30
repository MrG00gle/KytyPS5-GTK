// Feature: linux-gtk-modernization, Property 23: Emulator option parser outcomes
//
// Validates Requirements 3.8, 5.5, 5.9, and 15.13. Each case builds an argument list from valid
// options of Emulator::Options(): --game once (an existing directory or ELF), other options at
// random with valid values (--keymap up to three times), now and then --help or -h, and now and
// then --guest-exec with guest arguments at the end. Then at most one fault is injected: an
// unknown option (--redzone, another --name, or -x), a value option without its value at the
// end, a malformed or out-of-range value, a --game path that does not exist, or a --game path
// that exists but is not readable. With a fault, ParseOptions returns Error and the message names
// the option or the path; without one, it returns Help when --help or -h is present and Run
// otherwise. UsageText() names every option of the table.
//
// Unreadable paths cannot be made with chmod when the tests run as root (the CI container does),
// so this file interposes access(): a call for a path below the marked prefix with a mode other
// than F_OK fails with EACCES while a case marks it, and every other call passes through. P21 and
// P22 share this executable and never mark a prefix.

#include "emulatorOptions.h"
#include "support/configurationGenerators.h"
#include "support/property.h"
#include "support/tempDir.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

std::string& UnreadablePrefix() {
	static std::string prefix; // empty: every call passes through
	return prefix;
}

int g_denied_calls = 0;

} // namespace

extern "C" int access(const char* path, int mode) noexcept {
	const auto& prefix = UnreadablePrefix();
	if (!prefix.empty() && mode != F_OK && std::strncmp(path, prefix.c_str(), prefix.size()) == 0) {
		g_denied_calls++;
		errno = EACCES;
		return -1;
	}
	using AccessFunction      = int (*)(const char*, int);
	static const auto forward = reinterpret_cast<AccessFunction>(dlsym(RTLD_NEXT, "access"));
	return forward(path, mode);
}

namespace {

namespace ConfigGen = KytyTests::ConfigGen;

using ConfigGen::Choose;
using ConfigGen::OneIn;
using ConfigGen::PickOf;

// Paths for --game and --game-patch, made once per process.
struct Fixture {
	KytyTests::TempDir root {"kyty_p23"};
	std::string        game_dir;
	std::string        game_elf;
	std::string        patch;
	std::string        missing;
	std::string        unreadable_prefix;
	std::string        unreadable_dir;
	std::string        unreadable_elf;

	Fixture() {
		game_dir          = root.MakeDirectories("game dir").string();
		game_elf          = root.WriteFile("elf/game.elf", "elf").string();
		patch             = root.WriteFile("_Patches/PPSA01234.json", "{}").string();
		missing           = (root.Path() / "missing" / "game").string();
		unreadable_prefix = (root.Path() / "unreadable").string() + "/";
		unreadable_dir    = root.MakeDirectories("unreadable/game").string();
		unreadable_elf    = root.WriteFile("unreadable/game.elf", "elf").string();
	}
};

const Fixture& Paths() {
	static const Fixture fixture;
	return fixture;
}

// Marks the unreadable prefix for one case; unmarks it however the case ends.
class UnreadableScope {
public:
	explicit UnreadableScope(const std::string& prefix) { UnreadablePrefix() = prefix; }
	~UnreadableScope() { UnreadablePrefix().clear(); }
	UnreadableScope(const UnreadableScope&)            = delete;
	UnreadableScope& operator=(const UnreadableScope&) = delete;
};

std::string Decimal(uint64_t min, uint64_t max) {
	return std::to_string(Choose<uint64_t>(min, max + 1));
}

std::string AnyText() {
	return ConfigGen::AnyText(12);
}

std::string OneOf(const std::vector<std::string>& values) {
	return PickOf(values);
}

// A value that the option accepts, or nothing for an option this property does not know (a new
// option of the table must get a generator here).
std::optional<std::string> ValidValue(std::string_view name) {
	const auto& paths = Paths();
	if (name == "--game") {
		return OneIn(3) ? paths.game_elf : paths.game_dir;
	}
	if (name == "--game-patch") {
		return paths.patch;
	}
	if (name == "--addcont-dir" || name == "--mic" || name == "--shader-log-folder" ||
	    name == "--command-buffer-dump-folder" || name == "--printf-output-file") {
		return AnyText();
	}
	if (name == "--screen-width" || name == "--screen-height") {
		return Decimal(1, std::numeric_limits<uint32_t>::max());
	}
	if (name == "--user-name") {
		std::string user;
		const int   length = Choose(1, 17);
		for (int i = 0; i < length; i++) {
			user += static_cast<char>(Choose(0x20, 0x7F));
		}
		if (length <= 14 && OneIn(3)) {
			user += "\xC3\xA9"; // still at most 16 bytes
		}
		return user;
	}
	if (name == "--user-id") {
		const auto id = Choose<int32_t>(0, Config::MAX_USER_ID + 1);
		return std::to_string(id == 254 || id == 255 ? 1000 : id);
	}
	if (name == "--controller-color") {
		static const std::string digits = "0123456789abcdefABCDEF";
		std::string              color  = "#";
		for (int i = 0; i < 6; i++) {
			color += digits[Choose<size_t>(0, digits.size())];
		}
		return color;
	}
	if (name == "--controller-volume" || name == "--controller-vibration") {
		return Decimal(0, 100);
	}
	if (name == "--keyboard-player") {
		return Decimal(0, Config::MAX_PLAYERS);
	}
	if (name == "--present-mode") {
		return OneOf({"Fifo", "Mailbox", "Immediate"});
	}
	if (name == "--gpu") {
		return std::to_string(Choose<int64_t>(std::numeric_limits<int32_t>::min(),
		                                      int64_t {std::numeric_limits<int32_t>::max()} + 1));
	}
	if (name == "--vblank-frequency" || name == "--trophy-duration") {
		return Decimal(0, std::numeric_limits<uint32_t>::max());
	}
	if (name == "--console-language") {
		return Decimal(0, Config::MAX_CONSOLE_LANGUAGE);
	}
	if (name == "--shader-optimization-type") {
		return OneOf({"None", "Size", "Performance"});
	}
	if (name == "--shader-log-direction" || name == "--printf-direction") {
		return OneOf({"Silent", "Console", "File"});
	}
	if (name == "--keymap") {
		std::string control = "Pad:Cross";
		if (OneIn(2)) {
			control = ConfigGen::VisibleText(6);
			for (auto& c: control) {
				if (c == '=') {
					c = 'e';
				}
			}
		}
		return control + "=" + (OneIn(2) ? "J" : ConfigGen::VisibleText(6));
	}
	if (name == "--guest-exec") {
		return OneIn(2) ? std::string("/app0/other.elf") : ConfigGen::VisibleText(8);
	}
	return std::nullopt;
}

// Values that the option rejects; empty for an option that accepts every value.
std::vector<std::string> InvalidValues(std::string_view name) {
	if (name == "--screen-width" || name == "--screen-height") {
		return {"0", "-1", "abc", "4294967296", "", " 1", "1x", "+5", "1.5"};
	}
	if (name == "--user-name") {
		return {"", "abcdefghijklmnopq",
		        "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"};
	}
	if (name == "--user-id") {
		return {"254", "255", "-1", "2147482880", "2147483648", "abc", ""};
	}
	if (name == "--controller-color") {
		return {"red", "#12345", "#1234567", "#12345g", "0070d1", "", "#-12345"};
	}
	if (name == "--controller-volume" || name == "--controller-vibration") {
		return {"101", "-1", "abc", "", "4294967296"};
	}
	if (name == "--keyboard-player") {
		return {"5", "-1", "x", ""};
	}
	if (name == "--present-mode") {
		return {"mailbox", "FIFO", "", "3", "Vsync"};
	}
	if (name == "--gpu") {
		return {"x", "2147483648", "-2147483649", "", "1.0"};
	}
	if (name == "--vblank-frequency" || name == "--trophy-duration") {
		return {"-1", "abc", "4294967296", ""};
	}
	if (name == "--console-language") {
		return {"30", "-1", "x", ""};
	}
	if (name == "--shader-optimization-type") {
		return {"Fast", "none", ""};
	}
	if (name == "--shader-log-direction" || name == "--printf-direction") {
		return {"Loud", "file", ""};
	}
	if (name == "--keymap") {
		return {"NoEquals", "=x", "x=", "", "="};
	}
	if (name == "--game-patch") {
		return {Paths().missing};
	}
	return {};
}

// One option with its value, if it takes one.
struct Item {
	std::string                name;
	std::optional<std::string> value;
};

enum class Fault { None, UnknownOption, MissingValue, InvalidValue, GameMissing, GameUnreadable };

const char* FaultName(Fault fault) {
	switch (fault) {
		case Fault::None: return "no fault";
		case Fault::UnknownOption: return "unknown option";
		case Fault::MissingValue: return "missing value";
		case Fault::InvalidValue: return "invalid value";
		case Fault::GameMissing: return "--game does not exist";
		case Fault::GameUnreadable: return "--game is not readable";
	}
	return "?";
}

bool IsKnown(std::string_view arg) {
	for (const auto& option: Emulator::Options()) {
		if (option.name == arg) {
			return true;
		}
	}
	return false;
}

// True when text has an option line for name: two spaces, the name, then a space or the end.
bool ListsOption(const std::string& text, std::string_view name) {
	const std::string line = "\n  " + std::string(name);
	for (auto position = text.find(line); position != std::string::npos;
	     position      = text.find(line, position + 1)) {
		const auto next = position + line.size();
		if (next == text.size() || text[next] == ' ' || text[next] == '\n') {
			return true;
		}
	}
	return false;
}

} // namespace

KYTY_PROPERTY(23, "Emulator option parser outcomes") {
	return rc::check(name, []() {
		const auto& paths   = Paths();
		const auto  options = Emulator::Options();

		const auto usage = Emulator::UsageText();
		for (const auto& option: options) {
			RC_ASSERT(ListsOption(usage, option.name));
		}

		const auto fault = static_cast<Fault>(Choose(0, 6));
		RC_LOG() << FaultName(fault) << "\n";

		// The valid options.
		std::vector<Item>        items;
		std::optional<Item>      terminal;
		std::vector<std::string> guest_args;
		for (const auto& option: options) {
			const std::string option_name(option.name);
			switch (option.kind) {
				case Emulator::OptionKind::Flag:
					if (OneIn(3)) {
						items.push_back({option_name, std::nullopt});
					}
					break;
				case Emulator::OptionKind::Value: {
					const int count = option_name == "--game"     ? 1
					                  : option_name == "--keymap" ? Choose(0, 4)
					                  : OneIn(3)                  ? 1
					                                              : 0;
					for (int i = 0; i < count; i++) {
						const auto value = ValidValue(option_name);
						if (!value) {
							RC_FAIL("no valid value for " + option_name + "; add one to P23");
						}
						items.push_back({option_name, *value});
					}
					break;
				}
				case Emulator::OptionKind::Terminal:
					if (fault != Fault::MissingValue && OneIn(4)) {
						const auto value = ValidValue(option_name);
						if (!value) {
							RC_FAIL("no valid value for " + option_name + "; add one to P23");
						}
						terminal         = Item {option_name, *value};
						const int guests = Choose(0, 4);
						for (int i = 0; i < guests; i++) {
							guest_args.push_back(OneIn(3) ? std::string("--redzone") : AnyText());
						}
					}
					break;
				case Emulator::OptionKind::Help: break;
			}
		}
		// Random order, then --help or -h at a random place.
		for (size_t i = items.size(); i > 1; i--) {
			std::swap(items[i - 1], items[Choose<size_t>(0, i)]);
		}
		const bool help = OneIn(4);
		if (help) {
			const auto at = Choose<size_t>(0, items.size() + 1);
			items.insert(items.begin() + static_cast<std::ptrdiff_t>(at),
			             Item {OneIn(2) ? "--help" : "-h", std::nullopt});
		}
		const auto game = static_cast<size_t>(
		    std::find_if(items.begin(), items.end(),
		                 [](const Item& item) { return item.name == "--game"; }) -
		    items.begin());
		RC_ASSERT(game < items.size());

		// At most one fault, and the text its message must name.
		std::string named;
		bool        trailing_missing = false;
		switch (fault) {
			case Fault::None: break;
			case Fault::UnknownOption: {
				std::string unknown = "--redzone";
				if (OneIn(3)) {
					unknown = "-" + std::string(1, static_cast<char>(Choose<int>('a', 'z' + 1)));
					if (unknown == "-h") {
						unknown = "-x";
					}
				} else if (OneIn(2)) {
					unknown          = "--";
					const int length = Choose(1, 12);
					for (int i = 0; i < length; i++) {
						unknown += static_cast<char>(Choose<int>('a', 'z' + 1));
					}
					if (IsKnown(unknown)) {
						unknown = "--redzone";
					}
				}
				RC_ASSERT(!IsKnown(unknown));
				const auto at = Choose<size_t>(0, items.size() + 1);
				items.insert(items.begin() + static_cast<std::ptrdiff_t>(at),
				             Item {unknown, std::nullopt});
				named = unknown;
				break;
			}
			case Fault::MissingValue: {
				std::vector<std::string> value_options;
				for (const auto& option: options) {
					if (option.kind == Emulator::OptionKind::Value ||
					    option.kind == Emulator::OptionKind::Terminal) {
						value_options.emplace_back(option.name);
					}
				}
				named            = PickOf(value_options);
				trailing_missing = true;
				break;
			}
			case Fault::InvalidValue: {
				std::vector<std::string> checked;
				for (const auto& option: options) {
					if (!InvalidValues(option.name).empty()) {
						checked.emplace_back(option.name);
					}
				}
				const auto option_name = PickOf(checked);
				// A second --game-patch would fail as "only once"; replace the valid one.
				std::erase_if(items, [&](const Item& item) {
					return item.name == option_name && option_name == "--game-patch";
				});
				const auto at = Choose<size_t>(0, items.size() + 1);
				items.insert(items.begin() + static_cast<std::ptrdiff_t>(at),
				             Item {option_name, PickOf(InvalidValues(option_name))});
				named = option_name;
				break;
			}
			case Fault::GameMissing:
				items[game].value = paths.missing;
				named             = paths.missing;
				break;
			case Fault::GameUnreadable:
				items[game].value = OneIn(2) ? paths.unreadable_dir : paths.unreadable_elf;
				named             = *items[game].value;
				break;
		}

		std::vector<std::string> args;
		for (const auto& item: items) {
			args.push_back(item.name);
			if (item.value) {
				args.push_back(*item.value);
			}
		}
		if (trailing_missing) {
			args.push_back(named);
		}
		if (terminal) {
			args.push_back(terminal->name);
			args.push_back(*terminal->value);
			args.insert(args.end(), guest_args.begin(), guest_args.end());
		}
		for (const auto& arg: args) {
			RC_LOG() << "'" << arg << "' ";
		}
		RC_LOG() << "\n";

		const int                      denied_before = g_denied_calls;
		std::optional<UnreadableScope> unreadable;
		if (fault == Fault::GameUnreadable) {
			unreadable.emplace(paths.unreadable_prefix);
		}
		const auto outcome = Emulator::ParseOptions(args);
		unreadable.reset();
		RC_LOG() << "status " << static_cast<int>(outcome.status) << " '" << outcome.error << "'\n";

		if (fault != Fault::None) {
			RC_ASSERT(outcome.status == Emulator::ParseOutcome::Error);
			RC_ASSERT(outcome.error.find(named) != std::string::npos);
			if (fault == Fault::GameUnreadable) {
				RC_ASSERT(g_denied_calls > denied_before);
				RC_ASSERT(outcome.error.find("not readable") != std::string::npos);
			}
			if (fault == Fault::GameMissing) {
				RC_ASSERT(outcome.error.find("does not exist") != std::string::npos);
			}
		} else if (help) {
			RC_ASSERT(outcome.status == Emulator::ParseOutcome::Help);
			RC_ASSERT(outcome.error.empty());
		} else {
			RC_ASSERT(outcome.status == Emulator::ParseOutcome::Run);
			RC_ASSERT(outcome.error.empty());
			if (terminal) {
				RC_ASSERT(outcome.options.config.guest_arguments == guest_args);
			}
		}

		RC_TAG(FaultName(fault));
		RC_CLASSIFY(help, "with --help or -h");
		RC_CLASSIFY(terminal.has_value(), "with --guest-exec");
	});
}
