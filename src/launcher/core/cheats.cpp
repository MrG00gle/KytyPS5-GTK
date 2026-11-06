#include "launcher/core/cheats.h"

#include "launcher/core/atomicFile.h"
#include "launcher/core/dataPaths.h"
#include "launcher/core/jsonErrorCapture.h"
#include "launcher/core/text.h"
#include "launcher/core/titleId.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <utility>

namespace kyty::core {

namespace {

using OrderedJson = nlohmann::ordered_json;

constexpr std::string_view kCheatTitlePrefix = "PPSA";
constexpr std::size_t      kCheatTitleDigits = 5;
constexpr std::size_t      kMaxOffsetDigits  = 16;

constexpr char kChangedOnDisk[]       = "Cheat file changed; reload the dialog.";
constexpr char kUnsupportedMemory[]   = "Unsupported memory entries in cheat \"";
constexpr char kUnsupportedTitleId[]  = "Unsupported title ID.";
constexpr char kInvalidCheatFile[]    = "Invalid cheat file.";
constexpr char kAddressingFeatures[]  = "This JSON uses memory addressing features that are not "
                                        "supported yet.";
constexpr char kModuleSpecificCheat[] = "This JSON uses module-specific cheats, which are not "
                                        "supported yet.";

std::string Dump(const OrderedJson& value) {
	return value.dump(-1, ' ', false, OrderedJson::error_handler_t::replace);
}

// The member key of object, or nullptr when object is not an object or has no such member.
const OrderedJson* Member(const OrderedJson& object, const std::string& key) {
	if (!object.is_object()) {
		return nullptr;
	}
	const auto found = object.find(key);
	return found != object.end() ? &*found : nullptr;
}

// The string member key of object; "" when it is absent or not a string (QJsonValue::toString).
std::string StringMember(const OrderedJson& object, const std::string& key) {
	const auto* value = Member(object, key);
	return value != nullptr && value->is_string() ? value->get_ref<const std::string&>()
	                                              : std::string();
}

// Qt::CaseInsensitive comparison (Unicode case folding through SDL_strcasecmp); text with a NUL
// byte, where that comparison would stop, must match byte for byte.
bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
	if (a.find('\0') != std::string_view::npos || b.find('\0') != std::string_view::npos) {
		return a == b;
	}
	return CompareCaseInsensitive(a, b) == 0;
}

bool IsHexDigit(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool AllHex(std::string_view text) {
	return std::all_of(text.begin(), text.end(), IsHexDigit);
}

// ^(?:0[xX])?[0-9a-fA-F]{1,16}$
bool IsOffset(std::string_view text) {
	if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
		const auto digits = text.substr(2);
		if (digits.size() <= kMaxOffsetDigits && AllHex(digits)) {
			return true;
		}
	}
	return !text.empty() && text.size() <= kMaxOffsetDigits && AllHex(text);
}

// ^(?:[0-9a-fA-F]{2})+$
bool IsByteString(std::string_view text) {
	return !text.empty() && text.size() % 2 == 0 && AllHex(text);
}

// Qt's ValidateDocument: the reason why root is not a cheat document, or nothing.
std::optional<std::string> CheckDocument(const OrderedJson& root) {
	if (!root.is_object()) {
		return "Invalid cheat JSON (the top-level value is not an object).";
	}
	const auto* mods = Member(root, "mods");
	if (mods == nullptr || !mods->is_array()) {
		return "Cheat JSON has no \"mods\" array.";
	}
	std::size_t number = 0;
	for (const auto& mod: *mods) {
		number++;
		const auto entry = "Invalid cheat entry " + std::to_string(number);
		if (!mod.is_object()) {
			return entry + " (it is not an object).";
		}
		const auto* name = Member(mod, "name");
		if (name == nullptr || !name->is_string() || name->get_ref<const std::string&>().empty()) {
			return entry + " (it has no name).";
		}
		const auto* enabled = Member(mod, "enabled");
		if (enabled != nullptr && !enabled->is_boolean()) {
			return entry + " (\"enabled\" is not true or false).";
		}
	}
	return std::nullopt;
}

// The document of a root that CheckDocument accepted.
CheatDocument MakeDocument(const OrderedJson& root) {
	CheatDocument document;
	for (const auto& mod: *Member(root, "mods")) {
		Cheat cheat;
		cheat.name = StringMember(mod, "name");
		if (const auto* enabled = Member(mod, "enabled")) {
			cheat.enabled = enabled->get<bool>();
		}
		if (const auto* memory = Member(mod, "memory")) {
			cheat.memory = Dump(*memory);
		}
		document.cheats.push_back(std::move(cheat));
	}
	document.id      = StringMember(root, "id");
	document.name    = StringMember(root, "name");
	document.version = StringMember(root, "version");
	document.process = StringMember(root, "process");
	document.author  = StringMember(root, "author");
	if (const auto* credits = Member(root, "credits"); credits != nullptr && credits->is_array()) {
		for (const auto& credit: *credits) {
			if (credit.is_string()) {
				document.credits.push_back(credit.get_ref<const std::string&>());
			}
		}
	}
	document.json = Dump(root);
	return document;
}

// The memory checks of Qt's ValidateImport for one mod.
std::optional<std::string> CheckMemory(const OrderedJson& mod) {
	const auto  name   = StringMember(mod, "name");
	const auto* memory = Member(mod, "memory");
	if (memory == nullptr || !memory->is_array() || memory->empty()) {
		return kUnsupportedMemory + name + "\".";
	}
	for (const auto& write: *memory) {
		const auto  offset   = StringMember(write, "offset");
		const auto  off      = StringMember(write, "off");
		const auto  on       = StringMember(write, "on");
		const auto* absolute = Member(write, "absolute");
		const auto* expected = Member(write, "expected");
		const bool  addressing =
		    (absolute != nullptr && (!absolute->is_boolean() || absolute->get<bool>())) ||
		    (expected != nullptr &&
		     (!expected->is_string() ||
		      !EqualsIgnoreCase(expected->get_ref<const std::string&>(), off)));
		bool sections = false;
		for (const char* key:
		     {"section", "sectionName", "sectionProtection", "logicalOffset", "sectionOffset"}) {
			sections = sections || Member(write, key) != nullptr;
		}
		if (addressing || sections) {
			return std::string(kAddressingFeatures);
		}
		if (!write.is_object() || !IsOffset(offset) || !IsByteString(off) || !IsByteString(on) ||
		    off.size() != on.size()) {
			return kUnsupportedMemory + name + "\".";
		}
	}
	return std::nullopt;
}

CheatSaveError SaveError(CheatSaveErrorKind kind, std::string message) {
	return CheatSaveError {kind, std::move(message)};
}

} // namespace

bool IsCheatTitleId(std::string_view title_id) {
	const auto title = NormalizeTitleId(title_id);
	if (title.size() != kCheatTitlePrefix.size() + kCheatTitleDigits ||
	    !title.starts_with(kCheatTitlePrefix)) {
		return false;
	}
	for (std::size_t i = kCheatTitlePrefix.size(); i < title.size(); i++) {
		if (title[i] < '0' || title[i] > '9') {
			return false;
		}
	}
	return true;
}

std::filesystem::path CheatFilePath(const std::filesystem::path& data_dir,
                                    std::string_view             title_id) {
	if (!IsCheatTitleId(title_id)) {
		return {};
	}
	return CheatDirectory(data_dir) / (NormalizeTitleId(title_id) + ".json");
}

std::string CheatProcessName(std::string_view elf) {
	if (elf.empty()) {
		return "eboot.bin";
	}
	return std::filesystem::path(elf).filename().string();
}

Expected<CheatDocument, std::string> ParseCheatDocument(std::string_view bytes,
                                                        std::string_view file_name) {
	const auto prefix = std::string(file_name) + ": ";
	const auto root   = OrderedJson::parse(bytes.begin(), bytes.end(), nullptr, false);
	if (root.is_discarded()) {
		return MakeUnexpected(prefix + "Invalid cheat JSON (" +
		                      DescribeJsonError<OrderedJson>(bytes) + ").");
	}
	if (auto problem = CheckDocument(root)) {
		return MakeUnexpected(prefix + *problem);
	}
	return MakeDocument(root);
}

std::string SerializeCheatDocument(const CheatDocument& document) {
	auto root = OrderedJson::parse(document.json.begin(), document.json.end(), nullptr, false);
	if (root.is_discarded() || !root.is_object()) {
		return {};
	}
	if (const auto mods = root.find("mods"); mods != root.end() && mods->is_array()) {
		const auto count = std::min(mods->size(), document.cheats.size());
		for (std::size_t i = 0; i < count; i++) {
			auto&      mod     = (*mods)[i];
			const bool enabled = document.cheats[i].enabled;
			if (mod.is_object() && (mod.contains("enabled") || !enabled)) {
				mod["enabled"] = enabled;
			}
		}
	}
	return root.dump(4, ' ', false, OrderedJson::error_handler_t::replace) + "\n";
}

std::optional<std::string> ValidateRemoteImport(const CheatDocument& document,
                                                std::string_view title_id, std::string_view version,
                                                std::string_view process) {
	const auto root =
	    OrderedJson::parse(document.json.begin(), document.json.end(), nullptr, false);
	if (root.is_discarded()) {
		return "Invalid cheat JSON.";
	}
	if (auto problem = CheckDocument(root)) {
		return problem;
	}
	if (!IsCheatTitleId(title_id) ||
	    !EqualsIgnoreCase(StringMember(root, "id"), TrimWhitespace(title_id))) {
		return "The cheat file does not match this game's title ID.";
	}
	if (version.empty() || StringMember(root, "version") != version) {
		return "The cheat file does not match this game's version (" + std::string(version) + ").";
	}
	if (process.empty() || !EqualsIgnoreCase(StringMember(root, "process"), process)) {
		return "The cheat file does not match this game's executable (" + std::string(process) +
		       ").";
	}
	const auto& mods = *Member(root, "mods");
	if (mods.empty()) {
		return "The cheat file contains no cheats.";
	}
	if (Member(root, "master") != nullptr) {
		return "This JSON uses master codes, which are not supported yet.";
	}
	for (const auto& mod: mods) {
		if (Member(mod, "module_name") != nullptr) {
			return std::string(kModuleSpecificCheat);
		}
		if (auto problem = CheckMemory(mod)) {
			return problem;
		}
	}
	return std::nullopt;
}

CheatDocument WithAllDisabled(CheatDocument document) {
	for (auto& cheat: document.cheats) {
		cheat.enabled = false;
	}
	return document;
}

Expected<void, CheatSaveError> SaveIfUnchanged(const std::filesystem::path&    path,
                                               const CheatDocument&            document,
                                               std::optional<std::string_view> expected_bytes) {
	const auto text = SerializeCheatDocument(document);
	if (path.empty() || text.empty()) {
		return MakeUnexpected(SaveError(CheatSaveErrorKind::Failed, kInvalidCheatFile));
	}
	auto current = ReadFileLimited(path, kMaxCheatFileBytes);
	if (current) {
		if (!expected_bytes) {
			return MakeUnexpected(
			    SaveError(CheatSaveErrorKind::NeedsConfirmation,
			              "The cheat file " + path.string() + " exists; confirm replacing it."));
		}
		if (*current != *expected_bytes) {
			return MakeUnexpected(SaveError(CheatSaveErrorKind::ChangedOnDisk, kChangedOnDisk));
		}
	} else if (current.error().error_number != ENOENT) {
		return MakeUnexpected(SaveError(CheatSaveErrorKind::Failed,
		                                "Could not read cheat file: " + current.error().message));
	} else if (expected_bytes) {
		return MakeUnexpected(SaveError(CheatSaveErrorKind::ChangedOnDisk, kChangedOnDisk));
	}

	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	if (ec) {
		return MakeUnexpected(SaveError(CheatSaveErrorKind::Failed,
		                                "Could not create the local cheats directory " +
		                                    path.parent_path().string() + ": " + ec.message()));
	}
	if (auto written = WriteFileAtomically(path, text); !written) {
		return MakeUnexpected(SaveError(CheatSaveErrorKind::Failed,
		                                "Could not save cheat file: " + written.error().message));
	}
	return {};
}

Expected<std::filesystem::path, CheatSaveError>
ImportRemoteCheats(const std::filesystem::path& data_dir, std::string_view title_id,
                   const CheatDocument&            document,
                   std::optional<std::string_view> expected_local_bytes) {
	auto path = CheatFilePath(data_dir, title_id);
	if (path.empty()) {
		return MakeUnexpected(SaveError(CheatSaveErrorKind::Failed, kUnsupportedTitleId));
	}
	if (auto saved = SaveIfUnchanged(path, WithAllDisabled(document), expected_local_bytes);
	    !saved) {
		return MakeUnexpected(std::move(saved).error());
	}
	return path;
}

LocalCheatFile LoadLocalCheats(const std::filesystem::path& data_dir, std::string_view title_id) {
	LocalCheatFile local;
	local.path = CheatFilePath(data_dir, title_id);
	if (local.path.empty()) {
		local.message = kUnsupportedTitleId;
		return local;
	}
	auto bytes = ReadFileLimited(local.path, kMaxCheatFileBytes);
	if (!bytes) {
		if (bytes.error().error_number == ENOENT) {
			local.readable = true;
			local.message  = "No local cheat file: " + local.path.string();
		} else {
			local.reason  = std::generic_category().message(bytes.error().error_number);
			local.message = "Could not read cheat file: " + bytes.error().message;
		}
		return local;
	}
	local.readable = true;
	local.bytes    = std::move(*bytes);
	auto document  = ParseCheatDocument(*local.bytes, local.path.string());
	if (!document) {
		local.message = document.error();
		return local;
	}
	local.message  = "Loaded " + std::to_string(document->cheats.size()) + " cheat(s) from " +
	                 local.path.string() + ".";
	local.document = std::move(*document);
	return local;
}

} // namespace kyty::core
