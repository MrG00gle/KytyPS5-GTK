#ifndef EMULATOR_INCLUDE_EMULATOR_EMULATOROPTIONS_H_
#define EMULATOR_INCLUDE_EMULATOR_EMULATOROPTIONS_H_

#include "emulator.h"

#include <span>
#include <string>
#include <string_view>

namespace Emulator {

enum class OptionKind {
	Flag,     // Takes no value.
	Value,    // Takes the next argument as its value.
	Help,     // --help and -h.
	Terminal, // --guest-exec: takes the next argument and passes every later one to the guest.
};

struct OptionSpec {
	std::string_view name;
	OptionKind       kind;
	std::string_view value_hint;
	std::string_view help;
};

// The single option table. ParseOptions and UsageText both read it.
std::span<const OptionSpec> Options();

struct ParseOutcome {
	enum Status { Run, Help, Error } status;
	RunOptions  options;
	std::string error;
};

// args holds argv[1..argc). No arguments gives Help.
ParseOutcome ParseOptions(std::span<const std::string> args);

// The synopsis followed by one line per Options() entry.
std::string UsageText();

} // namespace Emulator

#endif /* EMULATOR_INCLUDE_EMULATOR_EMULATOROPTIONS_H_ */
