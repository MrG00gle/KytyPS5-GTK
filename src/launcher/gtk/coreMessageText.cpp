#include "launcher/gtk/coreMessageText.h"

#include "launcher/core/configurationFields.h"
#include "launcher/core/text.h"
#include "launcher/gtk/i18n.h"
#include "updater/core/updatePolicy.h"

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace kyty::gtk {

namespace {

bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}

// format with each conversion replaced by its argument (see CoreText); a missing argument is
// empty, a lone % at the end stays.
std::string Substitute(std::string_view format, const std::vector<std::string>& arguments) {
	constexpr std::string_view kFlags   = "-+ #0'";
	constexpr std::string_view kLengths = "hlLqjzt";
	std::string                out;
	std::size_t                next = 0;
	for (std::size_t i = 0; i < format.size(); i++) {
		if (format[i] != '%') {
			out += format[i];
			continue;
		}
		if (i + 1 < format.size() && format[i + 1] == '%') {
			out += '%';
			i++;
			continue;
		}
		std::size_t                j        = i + 1;
		std::size_t                position = 0;
		std::size_t                k        = j;
		std::optional<std::size_t> index;
		while (k < format.size() && IsDigit(format[k])) {
			position = position * 10 + static_cast<std::size_t>(format[k] - '0');
			k++;
		}
		if (k > j && k < format.size() && format[k] == '$' && position > 0) {
			index = position - 1;
			j     = k + 1;
		}
		while (j < format.size() && kFlags.find(format[j]) != std::string_view::npos) {
			j++;
		}
		while (j < format.size() && (IsDigit(format[j]) || format[j] == '.' || format[j] == '*')) {
			j++;
		}
		while (j < format.size() && kLengths.find(format[j]) != std::string_view::npos) {
			j++;
		}
		if (j >= format.size()) {
			out.append(format.substr(i));
			break;
		}
		const std::size_t argument = index ? *index : next++;
		if (argument < arguments.size()) {
			out += arguments[argument];
		}
		i = j;
	}
	return out;
}

CoreText Text(const char* msgid, std::vector<std::string> arguments = {}) {
	return CoreText {msgid, nullptr, 0, std::move(arguments)};
}

CoreText Plural(PluralText forms, uint64_t count, std::vector<std::string> arguments) {
	return CoreText {forms.singular, forms.plural, static_cast<unsigned long>(count),
	                 std::move(arguments)};
}

// Dynamic data only: the system's or a library's text.
CoreText Verbatim(std::string text) {
	return CoreText {nullptr, nullptr, 0, {std::move(text)}};
}

std::string Number(int64_t value) {
	return std::to_string(value);
}

std::string Number(uint64_t value) {
	return std::to_string(value);
}

std::string Upper(std::string_view text) {
	std::string upper(text);
	for (auto& c: upper) {
		c = g_ascii_toupper(c);
	}
	return upper;
}

// The rule texts of the fields with their own rule (FieldDescriptor::check), and of the output
// paths and the input mapping, by key; nullptr for another key.
const char* FieldRuleMsgid(std::string_view key) {
	if (key == "user_name") {
		return N_("The user name must contain 1 to 16 UTF-8 bytes after leading and trailing "
		          "spaces are removed.");
	}
	if (key == "user_id") {
		return N_("The user ID must be between 0 and 2147482879 and must not be 254 (everyone) or "
		          "255 (system).");
	}
	if (key == "gpu_index") {
		return N_("The GPU must be Automatic or a device index from 0.");
	}
	if (key == "trophy_duration_seconds") {
		return N_("The trophy notification duration must be between 5 and 10 seconds.");
	}
	if (key == "vblank_frequency") {
		return N_("The vblank frequency must be between 30 and 360.");
	}
	if (key == "console_language") {
		return N_("The console language must be one of the listed languages.");
	}
	if (key == "elf") {
		return N_("The executable must be a non-empty relative path without a “..” component.");
	}
	if (key == "shader_log_folder") {
		return N_("The shader log folder must not be empty while shader logs go to a file.");
	}
	if (key == "command_buffer_dump_folder") {
		return N_("The command buffer folder must not be empty while command buffer dumps are "
		          "enabled.");
	}
	if (key == "printf_output_file") {
		return N_("The printf output file must not be empty while printf output goes to a file.");
	}
	if (key == "host_input_mapping") {
		return N_("Each input mapping entry must have the form Name=Value with a non-empty name "
		          "and value.");
	}
	return nullptr;
}

// The session's sentence without the unverified frame.
CoreText SessionSentence(const updater::SessionOutcome& outcome) {
	using updater::SessionMessage;
	const auto& detail = outcome.detail;
	const auto  text   = core::SanitizeUtf8(detail.text);
	const auto  path   = core::SanitizeUtf8(detail.path.string());
	const auto  os     = OsErrorText(detail.error_number);
	const auto  number = Number(detail.number);
	switch (detail.kind) {
		case SessionMessage::None: break;
		case SessionMessage::NoNewerRelease: return Text(N_("No newer release is available."));
		case SessionMessage::UnknownReleaseOrder:
			return Text(N_("Could not determine whether the available release is newer."));
		case SessionMessage::CloseGame: return Text(N_("Close the running game first."));
		case SessionMessage::GameRunningCancelled:
			return Text(N_("The update was cancelled. Close the running game first."));
		case SessionMessage::InvalidConfiguration:
			return Text(
			    // TRANSLATORS: a setting name (install_dir, helper, working_dir), then its path.
			    N_("The update configuration is invalid: %s must be an absolute path (%s)."),
			    {text, path});
		case SessionMessage::RefusedUrl:
			// TRANSLATORS: a URL, then the repository (owner/name).
			return Text(N_("Refused to request %s: it is not a release URL of %s."),
			            {text, std::string(updater::kRepository)});
		case SessionMessage::InvalidRelease: return Text(N_("Invalid GitHub release response."));
		case SessionMessage::UnstableRelease:
			return Text(N_("The release is not a published stable release."));
		case SessionMessage::InvalidAsset:
			return Text(N_("The update package size or SHA-256 checksum is invalid."));
		case SessionMessage::MissingAsset:
			return Text(N_("This release has no update package for your platform."));
		case SessionMessage::OtherRelease:
			// TRANSLATORS: %s is a release tag.
			return Text(N_("GitHub answered with the release %s."), {text});
		case SessionMessage::CheckTooLarge:
			return Text(N_("The update response is larger than 2 MiB."));
		case SessionMessage::CheckNetwork:
			// TRANSLATORS: %s is the network's reason.
			return Text(N_("The update check failed: %s"), {text});
		case SessionMessage::CheckStatus:
			// TRANSLATORS: %llu is an HTTP status code, for example 404.
			return Text(N_("The update check failed with HTTP status %llu."), {number});
		case SessionMessage::CheckStalled:
			return Plural(NGettextNoop("The update check received no data for %llu second.",
			                           "The update check received no data for %llu seconds."),
			              static_cast<uint64_t>(detail.number), {number});
		case SessionMessage::CheckTimedOut: return Text(N_("The update check timed out."));
		case SessionMessage::CheckCancelled: return Text(N_("The update check was cancelled."));
		case SessionMessage::CreateWorkspace:
			// TRANSLATORS: the installation directory, then the system's reason.
			return Text(N_("Cannot create an update folder in %s: %s"), {path, os});
		case SessionMessage::CopyHelper:
			// TRANSLATORS: the update helper, the path of its copy, then the system's reason.
			return Text(N_("Cannot copy %s to %s: %s"),
			            {path, core::SanitizeUtf8(detail.other.string()), os});
		case SessionMessage::CreatePayload:
		case SessionMessage::DownloadCreate:
			// TRANSLATORS: a path, then the system's reason.
			return Text(N_("Cannot create %s: %s"), {path, os});
		case SessionMessage::DownloadNetwork:
			// TRANSLATORS: %s is the network's reason.
			return Text(N_("The download failed: %s"), {text});
		case SessionMessage::DownloadStatus:
			// TRANSLATORS: %llu is an HTTP status code, for example 404.
			return Text(N_("The download failed with HTTP status %llu."), {number});
		case SessionMessage::DownloadStalled:
			return Plural(NGettextNoop("The download received no data for %llu second.",
			                           "The download received no data for %llu seconds."),
			              static_cast<uint64_t>(detail.number), {number});
		case SessionMessage::DownloadTimedOut: return Text(N_("The download timed out."));
		case SessionMessage::DownloadCancelled: return Text(N_("The download was cancelled."));
		case SessionMessage::DownloadTooLarge:
			return Text(N_("The downloaded file is larger than the release asset."));
		case SessionMessage::DownloadTooSmall:
			return Plural(
			    NGettextNoop(
			        // TRANSLATORS: the bytes received so far and the size of the update package.
			        "The downloaded file is smaller than the release asset (%llu of %llu byte).",
			        "The downloaded file is smaller than the release asset (%llu of %llu bytes)."),
			    detail.size, {Number(detail.received), Number(detail.size)});
		case SessionMessage::DownloadDigest:
			return Text(N_("The update failed its SHA-256 verification. Please try again."));
		case SessionMessage::DownloadWrite:
			// TRANSLATORS: the update package's path, then the system's reason.
			return Text(N_("Cannot write %s: %s"), {path, os});
		case SessionMessage::DownloadFinished: return Text(N_("The download is already finished."));
		case SessionMessage::StepExited:
		case SessionMessage::StepSignaled:
		case SessionMessage::StepWaitStatus:
		case SessionMessage::StepNotStarted: {
			CoreText what;
			if (detail.kind == SessionMessage::StepExited) {
				// TRANSLATORS: a program ("/usr/bin/tar"), then its exit code.
				what = Text(N_("%s exited with code %lld"), {text, number});
			} else if (detail.kind == SessionMessage::StepSignaled) {
				// TRANSLATORS: a program ("/usr/bin/tar"), then a signal number.
				what = Text(N_("%s was ended by signal %lld"), {text, number});
			} else if (detail.kind == SessionMessage::StepWaitStatus) {
				// TRANSLATORS: a program ("/usr/bin/tar"), then a waitpid() status.
				what = Text(N_("%s ended with wait status %lld"), {text, number});
			} else {
				what = ProcessErrorText(detail.process);
			}
			if (detail.standard_error.empty()) {
				return Text(N_("Could not prepare the update: %s."), {Render(what)});
			}
			// TRANSLATORS: why the update could not be prepared, then the program's own output.
			return Text(N_("Could not prepare the update: %s:\n%s"),
			            {Render(what), core::SanitizeUtf8(detail.standard_error)});
		}
		case SessionMessage::InstallerNotStarted:
			return Text(N_("Could not start the update installer: %s"),
			            {Render(ProcessErrorText(detail.process))});
	}
	return Verbatim(core::SanitizeUtf8(outcome.message));
}

} // namespace

std::string Render(const CoreText& text) {
	if (text.msgid == nullptr) {
		return text.arguments.empty() ? std::string() : text.arguments.front();
	}
	const char* translated = text.plural_msgid != nullptr
	                             ? NGettext(text.msgid, text.plural_msgid, text.count)
	                             : g_dgettext(GETTEXT_PACKAGE, text.msgid);
	return Substitute(translated, text.arguments);
}

std::string OsErrorText(int error_number) {
	return g_strerror(error_number);
}

CoreText IoErrorText(const core::IoError& error) {
	using core::IoErrorKind;
	const auto path   = error.path.string();
	const auto detail = error.detail.string();
	const auto os     = OsErrorText(error.error_number);
	// The "cannot ..." texts end a sentence such as "The settings file %s was not changed:\n%s".
	// xgettext takes a translator comment only from the line right above the msgid's line, so each
	// case has its own.
	switch (error.kind) {
		// TRANSLATORS: ends a longer sentence; a path, then the system's reason.
		case IoErrorKind::Open: return Text(N_("cannot open %s: %s"), {path, os});
		// TRANSLATORS: ends a longer sentence; a path, then the system's reason.
		case IoErrorKind::Read: return Text(N_("cannot read %s: %s"), {path, os});
		case IoErrorKind::TooLarge:
			// TRANSLATORS: ends a sentence; a path, the limit in bytes, the system's reason.
			return Plural(NGettextNoop("cannot read %s (limit %llu byte): %s",
			                           "cannot read %s (limit %llu bytes): %s"),
			              error.limit, {path, Number(error.limit), os});
		case IoErrorKind::ReadLink:
			// TRANSLATORS: ends a sentence; the link's path, then the system's reason.
			return Text(N_("cannot read the symbolic link %s: %s"), {detail, os});
		// TRANSLATORS: ends a sentence; a path, then the system's reason.
		case IoErrorKind::ResolveLinks: return Text(N_("cannot resolve %s: %s"), {path, os});
		// TRANSLATORS: ends a sentence; a path, then the system's reason.
		case IoErrorKind::NoFileName: return Text(N_("cannot write %s: %s"), {path, os});
		case IoErrorKind::CreateTemp:
			// TRANSLATORS: ends a sentence; a directory, then the system's reason.
			return Text(N_("cannot create a temporary file in %s: %s"), {detail, os});
		case IoErrorKind::WriteTemp: return Text(N_("cannot write %s: %s"), {detail, os});
		// TRANSLATORS: ends a sentence; a temporary file, then the system's reason.
		case IoErrorKind::SyncTemp: return Text(N_("cannot sync %s: %s"), {detail, os});
		// TRANSLATORS: ends a sentence; a temporary file, then the system's reason.
		case IoErrorKind::CloseTemp: return Text(N_("cannot close %s: %s"), {detail, os});
		case IoErrorKind::Rename:
			// TRANSLATORS: ends a sentence; a temporary file, a path, the system's reason.
			return Text(N_("cannot rename %s to %s: %s"), {detail, error.other.string(), os});
		case IoErrorKind::CreateDirectory:
			// TRANSLATORS: ends a sentence; a directory, then the system's reason.
			return Text(N_("cannot create the directory %s: %s"), {detail, os});
		case IoErrorKind::NotLoaded:
			return Text(
			    // TRANSLATORS: a settings file, then the system's reason it could not be loaded.
			    N_("%s was not saved because it exists but could not be loaded (%s). Fix or "
			       "move the file, or confirm replacing it."),
			    {path, os});
		case IoErrorKind::NoTitleId:
			// TRANSLATORS: %s is the path the game settings were to be exported to.
			return Text(N_("Cannot export game settings to %s: the game has no title ID"), {path});
	}
	return Text(N_("cannot open %s: %s"), {path, os});
}

CoreText ValidationRuleText(const core::ValidationError& error) {
	switch (error.rule) {
		case core::ValidationRule::Field: break;
		case core::ValidationRule::MissingOutputPath:
			if (const char* msgid = FieldRuleMsgid(error.field); msgid != nullptr) {
				return Text(msgid);
			}
			break;
		case core::ValidationRule::InvalidMappingEntries:
			return Text(FieldRuleMsgid("host_input_mapping"));
	}
	if (const char* msgid = FieldRuleMsgid(error.field); msgid != nullptr) {
		return Text(msgid);
	}
	if (const auto* field = core::FindGameConfigurationField(error.field); field != nullptr) {
		if (field->kind == core::FieldKind::Enum) {
			return Text(N_("The value must be one of the listed choices."));
		}
		if (field->kind == core::FieldKind::Bool) {
			return Text(N_("The value must be true or false."));
		}
	}
	// A field without a rule text of its own (none of the current ones).
	return Text(N_("The value is not valid."));
}

CoreText CheatProblemText(const core::CheatProblem& problem) {
	using core::CheatProblemKind;
	const auto entry = std::to_string(problem.entry);
	CoreText   reason;
	switch (problem.kind) {
		case CheatProblemKind::InvalidJson:
			// TRANSLATORS: %s is the JSON parser's message.
			reason = Text(N_("Invalid cheat JSON (%s)."), {problem.detail});
			break;
		case CheatProblemKind::NotAnObject:
			reason = Text(N_("Invalid cheat JSON (the top-level value is not an object)."));
			break;
		case CheatProblemKind::NoModsArray:
			reason = Text(N_("Cheat JSON has no \"mods\" array."));
			break;
		case CheatProblemKind::EntryNotAnObject:
			// TRANSLATORS: %llu is the number of the cheat in the file, from 1.
			reason = Text(N_("Invalid cheat entry %llu (it is not an object)."), {entry});
			break;
		case CheatProblemKind::EntryWithoutName:
			// TRANSLATORS: %llu is the number of the cheat in the file, from 1.
			reason = Text(N_("Invalid cheat entry %llu (it has no name)."), {entry});
			break;
		case CheatProblemKind::EntryEnabledNotBool:
			reason = Text(
			    // TRANSLATORS: %llu is the number of the cheat in the file, from 1.
			    N_("Invalid cheat entry %llu (\"enabled\" is not true or false)."), {entry});
			break;
		case CheatProblemKind::DocumentNotJson: reason = Text(N_("Invalid cheat JSON.")); break;
		case CheatProblemKind::TitleMismatch:
			reason = Text(N_("The cheat file does not match this game's title ID."));
			break;
		case CheatProblemKind::VersionMismatch:
			// TRANSLATORS: %s is the game's version ("01.000").
			reason = Text(N_("The cheat file does not match this game's version (%s)."),
			              {problem.detail});
			break;
		case CheatProblemKind::ProcessMismatch:
			// TRANSLATORS: %s is the game's executable ("eboot.bin").
			reason = Text(N_("The cheat file does not match this game's executable (%s)."),
			              {problem.detail});
			break;
		case CheatProblemKind::NoCheats:
			reason = Text(N_("The cheat file contains no cheats."));
			break;
		case CheatProblemKind::MasterCodes:
			reason = Text(N_("This JSON uses master codes, which are not supported yet."));
			break;
		case CheatProblemKind::ModuleSpecific:
			reason =
			    Text(N_("This JSON uses module-specific cheats, which are not supported yet."));
			break;
		case CheatProblemKind::AddressingFeatures:
			reason =
			    Text(N_("This JSON uses memory addressing features that are not supported yet."));
			break;
		case CheatProblemKind::UnsupportedMemory:
			// TRANSLATORS: %s is the name of a cheat.
			reason = Text(N_("Unsupported memory entries in cheat \"%s\"."), {problem.detail});
			break;
	}
	if (problem.file.empty()) {
		return reason;
	}
	// TRANSLATORS: a file name or path, then what is wrong with it.
	return Text(N_("%s: %s"), {problem.file, Render(reason)});
}

CoreText CheatSaveErrorText(const core::CheatSaveError& error) {
	using core::CheatSaveFailure;
	switch (error.kind) {
		case core::CheatSaveErrorKind::NeedsConfirmation:
			// TRANSLATORS: %s is the path of the local cheat file.
			return Text(N_("The cheat file %s exists; confirm replacing it."),
			            {error.io.path.string()});
		case core::CheatSaveErrorKind::ChangedOnDisk:
			return Text(N_("Cheat file changed; reload the dialog."));
		case core::CheatSaveErrorKind::Failed: break;
	}
	switch (error.failure) {
		case CheatSaveFailure::None:
		case CheatSaveFailure::InvalidCheatFile: break;
		case CheatSaveFailure::UnsupportedTitleId: return Text(N_("Unsupported title ID."));
		case CheatSaveFailure::Read:
			// TRANSLATORS: %s is why the file cannot be read ("cannot open <path>: <reason>").
			return Text(N_("Could not read cheat file: %s"), {Render(IoErrorText(error.io))});
		case CheatSaveFailure::CreateDirectory:
			// TRANSLATORS: a directory, then the system's reason.
			return Text(N_("Could not create the local cheats directory %s: %s"),
			            {error.io.detail.string(), OsErrorText(error.io.error_number)});
		case CheatSaveFailure::Write:
			// TRANSLATORS: %s is why the file cannot be written ("cannot rename <a> to <b>: ...").
			return Text(N_("Could not save cheat file: %s"), {Render(IoErrorText(error.io))});
	}
	return Text(N_("Invalid cheat file."));
}

CoreText CheatFetchErrorText(const core::CheatFetchError& error) {
	switch (error.failure) {
		case core::HttpFailure::None:
		case core::HttpFailure::Cancelled: break;
		case core::HttpFailure::Network:
			// TRANSLATORS: %s is the network's reason.
			return Text(N_("Cheat download failed: %s."), {error.detail});
		case core::HttpFailure::Status:
			// TRANSLATORS: %d is an HTTP status code, for example 404.
			return Text(N_("Unexpected response from the cheat repository (HTTP %d)."),
			            {std::to_string(error.status)});
		case core::HttpFailure::TooLarge:
			// TRANSLATORS: %llu is a size in mebibytes (MiB).
			return Text(N_("Cheat download exceeds the size limit of %llu MiB."),
			            {Number(error.limit_mib)});
		case core::HttpFailure::Stalled:
			// TRANSLATORS: %s is the network's reason.
			return Text(N_("Cheat download stalled: %s."), {error.detail});
		case core::HttpFailure::TimedOut:
			return Plural(NGettextNoop("Cheat download timed out after %lld second.",
			                           "Cheat download timed out after %lld seconds."),
			              static_cast<uint64_t>(error.seconds), {Number(error.seconds)});
	}
	return Text(N_("Cheat download was cancelled."));
}

CoreText CheatIndexFailureText(const core::CheatIndexFailure& failure) {
	CoreText reason;
	if (failure.fetch.failure != core::HttpFailure::None) {
		reason = CheatFetchErrorText(failure.fetch);
	} else if (failure.problem == core::CheatIndexProblem::SkippedEntries) {
		reason = Plural(NGettextNoop("Skipped %llu invalid cheat index entry.",
		                             "Skipped %llu invalid cheat index entries."),
		                failure.skipped, {Number(static_cast<uint64_t>(failure.skipped))});
	} else {
		reason = Text(N_("Invalid cheat index."));
	}
	// TRANSLATORS: a cheat file format ("JSON", "MC4", "SHN"), then what went wrong with its list.
	return Text(N_("%s: %s"), {Upper(failure.format), Render(reason)});
}

std::string CheatCatalogErrorText(const core::CheatCatalogResult& result) {
	std::string text;
	for (const auto& failure: result.failures) {
		text += (text.empty() ? "" : "\n") + Render(CheatIndexFailureText(failure));
	}
	return text;
}

CoreText RemoteCheatErrorText(const core::RemoteCheatResult& result) {
	if (result.fetch.failure != core::HttpFailure::None) {
		return CheatFetchErrorText(result.fetch);
	}
	if (result.problem) {
		return CheatProblemText(*result.problem);
	}
	return {};
}

CoreText WritableDirectoryErrorText(const core::WritableDirectoryError& error) {
	const auto dir   = error.dir.string();
	const auto probe = error.probe.string();
	const auto os    = OsErrorText(error.error_number);
	switch (error.step) {
		case core::WritableProbeStep::CreateDirectories: break;
		case core::WritableProbeStep::Create:
			// TRANSLATORS: the data directory, a test file's path, then the system's reason.
			return Text(N_("%s: cannot create the write probe %s: %s"), {dir, probe, os});
		case core::WritableProbeStep::Write:
			// TRANSLATORS: the data directory, a test file's path, then the system's reason.
			return Text(N_("%s: cannot write the write probe %s: %s"), {dir, probe, os});
		case core::WritableProbeStep::Close:
			// TRANSLATORS: the data directory, a test file's path, then the system's reason.
			return Text(N_("%s: cannot close the write probe %s: %s"), {dir, probe, os});
		case core::WritableProbeStep::Remove:
			// TRANSLATORS: the data directory, a test file's path, then the system's reason.
			return Text(N_("%s: cannot remove the write probe %s: %s"), {dir, probe, os});
	}
	// TRANSLATORS: the data directory, then the system's reason.
	return Text(N_("%s: cannot create the directory: %s"), {dir, os});
}

CoreText SpawnReasonText(const core::SpawnError& error) {
	const auto os = OsErrorText(error.error_number);
	switch (error.cause) {
		case core::SpawnCause::ErrorNumber: break;
		case core::SpawnCause::NotRegularFile: return Text(N_("not a regular file"));
		case core::SpawnCause::NulArgument:
			// TRANSLATORS: %llu is the number of an argument, from 1.
			return Text(N_("argument %llu contains a NUL byte"),
			            {Number(static_cast<uint64_t>(error.argument))});
		case core::SpawnCause::Pipe: return Text(N_("cannot create a pipe: %s"), {os});
		case core::SpawnCause::FileActions:
			return Text(N_("cannot prepare the spawn file actions: %s"), {os});
		case core::SpawnCause::Attributes:
			return Text(N_("cannot prepare the spawn attributes: %s"), {os});
		case core::SpawnCause::Spawn:
			if (!error.cwd.empty()) {
				// TRANSLATORS: the system's reason, then the directory the game was started in.
				return Text(N_("%s (working directory %s)"), {os, error.cwd.string()});
			}
			break;
	}
	return Verbatim(os);
}

CoreText CompatibilityFileErrorText(const core::CompatibilityFileError& error) {
	switch (error.kind) {
		case core::CompatibilityFileErrorKind::Unreadable: break;
		case core::CompatibilityFileErrorKind::InvalidJson:
			// TRANSLATORS: the file's path, then the JSON parser's message.
			return Text(N_("The compatibility database %s does not parse: the content is not valid "
			               "JSON: %s"),
			            {error.path.string(), error.detail});
		case core::CompatibilityFileErrorKind::NotAnObject:
			return Text(N_("The compatibility database %s does not parse: the top-level JSON value "
			               "is not an object"),
			            {error.path.string()});
	}
	// TRANSLATORS: %s is why the file cannot be read ("cannot open <path>: <reason>").
	return Text(N_("Cannot read the compatibility database: %s"), {Render(IoErrorText(error.io))});
}

CoreText ArchiveErrorText(const core::ArchiveError& error) {
	const auto archive = error.archive.string();
	CoreText   reason;
	switch (error.kind) {
		case core::ArchiveErrorKind::None: return {};
		case core::ArchiveErrorKind::Exists:
			// TRANSLATORS: %s is the archive path.
			reason = Text(N_("%s already exists."), {archive});
			break;
		case core::ArchiveErrorKind::CheckFailed:
			// TRANSLATORS: the archive path, then the system's reason.
			reason = Text(N_("%s: cannot check whether it exists: %s."),
			              {archive, OsErrorText(error.error_number)});
			break;
		case core::ArchiveErrorKind::RemoveFailed:
			// TRANSLATORS: the archive path, then the system's reason.
			reason = Text(N_("cannot remove the partial archive %s: %s."),
			              {archive, OsErrorText(error.error_number)});
			break;
		case core::ArchiveErrorKind::CompressFailed:
			// TRANSLATORS: %s is the game folder that could not be compressed.
			return Text(N_("Could not compress %s."), {error.source.string()});
	}
	if (error.source.empty()) {
		return reason;
	}
	// TRANSLATORS: the game folder, then why it could not be compressed.
	return Text(N_("Could not compress %s: %s"), {error.source.string(), Render(reason)});
}

CoreText ScanWarningReasonText(const core::ScanWarning& warning) {
	switch (warning.kind) {
		case core::ScanWarningKind::Unreadable: break;
		case core::ScanWarningKind::NoCanonicalPath:
			return Text(N_("cannot resolve the canonical path"));
	}
	// The system's text: ScanFileSystem::List has no errno to give.
	return Verbatim(warning.reason);
}

CoreText ProcessErrorText(const updater::ProcessError& error) {
	using updater::ProcessErrorKind;
	const auto program = core::SanitizeUtf8(error.program.string());
	const auto path    = core::SanitizeUtf8(error.path.string());
	const auto os      = OsErrorText(error.error_number);
	switch (error.kind) {
		case ProcessErrorKind::None: break;
		case ProcessErrorKind::NulArgument:
			// TRANSLATORS: a program ("/usr/bin/tar"), then the number of an argument, from 1.
			return Text(N_("Cannot start %s: argument %llu contains a NUL byte"),
			            {program, Number(static_cast<uint64_t>(error.argument))});
		case ProcessErrorKind::Pipe:
			// TRANSLATORS: a program ("/usr/bin/tar"), then the system's reason.
			return Text(N_("Cannot start %s: cannot create a pipe: %s"), {program, os});
		case ProcessErrorKind::Prepare:
			// TRANSLATORS: a program ("/usr/bin/tar"), then the system's reason.
			return Text(N_("Cannot start %s: cannot prepare the spawn: %s"), {program, os});
		case ProcessErrorKind::Spawn:
			if (!error.path.empty()) {
				// TRANSLATORS: a program, the system's reason, then the directory it started in.
				return Text(N_("Cannot start %s: %s (working directory %s)"), {program, os, path});
			}
			// TRANSLATORS: a program ("/usr/bin/tar"), then the system's reason.
			return Text(N_("Cannot start %s: %s"), {program, os});
		case ProcessErrorKind::OpenLog:
			// TRANSLATORS: a program, the path of its log file, then the system's reason.
			return Text(N_("Cannot start %s: cannot open the log file %s: %s"),
			            {program, path, os});
		// TRANSLATORS: a program ("/usr/bin/tar"), then the system's reason.
		case ProcessErrorKind::Wait: return Text(N_("Cannot wait for %s: %s"), {program, os});
	}
	// A runner that gave only its text.
	return Verbatim(core::SanitizeUtf8(error.message));
}

CoreText SessionMessageText(const updater::SessionOutcome& outcome) {
	if (outcome.detail.kind == updater::SessionMessage::None) {
		// No message, or one the session does not classify (none is known): as it is.
		return outcome.message.empty() ? CoreText {}
		                               : Verbatim(core::SanitizeUtf8(outcome.message));
	}
	auto sentence = SessionSentence(outcome);
	if (!outcome.detail.installed_unverified) {
		return sentence;
	}
	// TRANSLATORS: %s is the reason (an English message of the updater or of the network).
	return Text(N_("Could not verify the currently installed release. %s"), {Render(sentence)});
}

} // namespace kyty::gtk
