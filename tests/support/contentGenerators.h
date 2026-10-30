#pragma once

// Fixtures for the content tool tests (task 16.5): trophy packages, unlock files, add-on packages,
// save data trees, and scripted archive packers, for tests/launcher_core/*Tests.cpp and the content
// properties (Properties 26-28 and 34-36). Header only and free of RapidCheck, because the unit
// tests build without exceptions and without the RapidCheck include path: the properties draw
// their inputs themselves and pass them to these builders.
//
// - Trophy packages: TrophyPackageBytes(spec) gives a .ucp file in the layout that Common::Trophies
//   reads (the WritePackage layout of tests/TrophySystemTests.cpp: magic 0xb228c60a, version 1, a
//   table of named files), with tropconf.json, tropmeta_<language>.json, and trop<NNNN>.png icons.
//   ExpectedTrophyPackage(spec, ...) is the kyty::core::TrophyPackage that LoadGameTrophies must
//   give for it. No binary fixture is committed (plan D20).
// - Unlock files: WriteUnlocks() through Common::Trophies::SaveUnlockData.
// - Add-on packages: AddOnContentId() and AddOnParamJson() for Common::FindAddContent.
// - Trees: SnapshotTree() records every entry below a directory (without following links), so a
//   test can check that a tree is byte-identical before and after an operation.
// - FailingRemover: a kyty::core::PathRemover that fails on chosen paths and records its calls.
// - Packer scripts: ScriptedPacker() follows a PackerScript (progress calls, then success,
//   cancellation at a call, or failure after a number of calls).
// - Cheat documents (Properties 29-33): CheatDocumentSpec builds cheat file JSON as text, member by
//   member, so a test knows the exact document (the tests link no JSON library); AllDisabledSpec is
//   what an import must write; RemoteCheatSpec is a document ValidateRemoteImport accepts.
// - Remote responses: CheatIndexLine and CheatFileName build index lines and collection names;
//   ResponseOfSize pads a body to an exact size for the 2 MiB and 8 MiB limits.
// - Mapping edits (Property 37): MappingEntryPool and KeyNamePool are host_input_mapping entries
//   and key names of every kind; MappingEdit and ApplyMappingEdit run one editor operation.

#include "common/trophies.h"
#include "launcher/core/archiveJob.h"
#include "launcher/core/cheats.h"
#include "launcher/core/inputMapping.h"
#include "launcher/core/saveData.h"
#include "launcher/core/trophyModel.h"
#include "support/fakeHttpClient.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace KytyTests::ContentGen {

// ---- JSON text

// text as a JSON string literal: quotes, backslashes, and control characters escaped; other bytes
// are copied (text must be valid UTF-8 for nlohmann to accept it).
inline std::string JsonString(std::string_view text) {
	static constexpr char kHex[] = "0123456789abcdef";
	std::string           out    = "\"";
	for (const char c: text) {
		const auto byte = static_cast<unsigned char>(c);
		if (c == '"' || c == '\\') {
			out += '\\';
			out += c;
		} else if (byte < 0x20) {
			out += "\\u00";
			out += kHex[byte >> 4u];
			out += kHex[byte & 0xFu];
		} else {
			out += c;
		}
	}
	out += '"';
	return out;
}

// ---- Trophy packages

// A package file: the named files in the table layout Common::Trophies reads. Names must be shorter
// than 32 bytes.
inline std::string UcpBytes(const std::map<std::string, std::string>& files) {
	const auto put = [](std::string& bytes, std::size_t offset, std::uint64_t value,
	                    std::size_t length) {
		for (std::size_t i = 0; i < length; i++) {
			bytes[offset + length - 1 - i] = static_cast<char>(value & 0xFFu);
			value >>= 8u;
		}
	};
	std::size_t offset = 0x40 + files.size() * 0x40;
	std::size_t size   = offset;
	for (const auto& [name, contents]: files) {
		size += contents.size();
	}
	std::string bytes(size, '\0');
	put(bytes, 0, 0xb228c60a, 4);
	put(bytes, 4, 1, 4);
	put(bytes, 8, size, 8);
	put(bytes, 0x10, files.size(), 4);
	put(bytes, 0x14, 0x20, 4);
	std::size_t entry = 0x40;
	for (const auto& [name, contents]: files) {
		std::memcpy(bytes.data() + entry, name.data(), std::min<std::size_t>(name.size(), 0x1F));
		put(bytes, entry + 0x20, offset, 8);
		put(bytes, entry + 0x28, contents.size(), 8);
		std::memcpy(bytes.data() + offset, contents.data(), contents.size());
		offset += contents.size();
		entry += 0x40;
	}
	return bytes;
}

struct TrophySpec {
	int  id         = 0;
	char grade      = 'B'; // 'P', 'G', 'S', or 'B'
	bool hidden     = false;
	bool has_reward = false;
	// Absent texts are left out of tropmeta_<language>.json (the trophy then has none).
	std::optional<std::string> name;
	std::optional<std::string> detail;
	std::optional<std::string> reward;
	std::optional<std::string> icon; // the bytes of trop<NNNN>.png
};

struct TrophyPackageSpec {
	std::string             title            = "Test game";
	std::string             default_language = "en-US";
	std::vector<TrophySpec> trophies; // unique IDs, at most 1000
	// More files of the package (another tropmeta_<language>.json, for example).
	std::map<std::string, std::string> extra_files;
};

inline kyty::core::TrophyGrade GradeOf(char grade) {
	switch (grade) {
		case 'P': return kyty::core::TrophyGrade::Platinum;
		case 'G': return kyty::core::TrophyGrade::Gold;
		case 'S': return kyty::core::TrophyGrade::Silver;
		default: return kyty::core::TrophyGrade::Bronze;
	}
}

inline std::string TropconfJson(const TrophyPackageSpec& spec) {
	std::string json =
	    "{\"defaultLanguage\":" + JsonString(spec.default_language) + ",\"trophies\":[";
	for (std::size_t i = 0; i < spec.trophies.size(); i++) {
		const auto& trophy = spec.trophies[i];
		json += i == 0 ? "" : ",";
		json += "{\"id\":\"" + std::to_string(trophy.id) + "\",\"grade\":\"" +
		        std::string(1, trophy.grade) +
		        "\",\"hidden\":" + (trophy.hidden ? "true" : "false") +
		        ",\"hasReward\":" + (trophy.has_reward ? "true" : "false") + "}";
	}
	return json + "]}";
}

// tropmeta_<language>.json with the title and the texts of trophies.
inline std::string TropmetaJson(std::string_view title, const std::vector<TrophySpec>& trophies) {
	std::string json  = "{\"metadata\":{\"titleMetadata\":{\"name\":" + JsonString(title) +
	                    "},\"trophyMetadata\":[";
	bool        first = true;
	for (const auto& trophy: trophies) {
		if (!trophy.name && !trophy.detail && !trophy.reward) {
			continue;
		}
		json += first ? "" : ",";
		first = false;
		json += "{\"id\":" + std::to_string(trophy.id);
		for (const auto& [key, text]:
		     {std::pair {"name", &trophy.name}, std::pair {"detail", &trophy.detail},
		      std::pair {"reward", &trophy.reward}}) {
			if (*text) {
				json += std::string(",\"") + key + "\":" + JsonString(**text);
			}
		}
		json += "}";
	}
	return json + "]}}";
}

inline std::string IconFileName(int id) {
	char name[32];
	std::snprintf(name, sizeof(name), "trop%04d.png", id);
	return name;
}

inline std::map<std::string, std::string> TrophyPackageFiles(const TrophyPackageSpec& spec) {
	std::map<std::string, std::string> files             = spec.extra_files;
	files["tropconf.json"]                               = TropconfJson(spec);
	files["tropmeta_" + spec.default_language + ".json"] = TropmetaJson(spec.title, spec.trophies);
	for (const auto& trophy: spec.trophies) {
		if (trophy.icon) {
			files[IconFileName(trophy.id)] = *trophy.icon;
		}
	}
	return files;
}

inline std::string TrophyPackageBytes(const TrophyPackageSpec& spec) {
	return UcpBytes(TrophyPackageFiles(spec));
}

// The package that LoadGameTrophies gives for spec read in its default language: the trophies in
// ID order with the texts the spec wrote ("" for absent ones).
inline kyty::core::TrophyPackage ExpectedTrophyPackage(const TrophyPackageSpec& spec,
                                                       std::string              file_name,
                                                       std::uint32_t            service_label) {
	kyty::core::TrophyPackage package;
	package.file_name     = std::move(file_name);
	package.service_label = service_label;
	package.title         = spec.title;
	for (const auto& trophy: spec.trophies) {
		kyty::core::TrophyDefinition definition;
		definition.id          = trophy.id;
		definition.grade       = GradeOf(trophy.grade);
		definition.hidden      = trophy.hidden;
		definition.has_reward  = trophy.has_reward;
		definition.name        = trophy.name.value_or("");
		definition.description = trophy.detail.value_or("");
		definition.reward      = trophy.reward.value_or("");
		definition.icon_png    = trophy.icon.value_or("");
		package.trophies.push_back(std::move(definition));
	}
	std::sort(package.trophies.begin(), package.trophies.end(),
	          [](const auto& a, const auto& b) { return a.id < b.id; });
	return package;
}

// ---- Unlock files

// Writes the unlock file of package <service_label> for title_id and user_id below data_dir
// (<data_dir>/_Trophies/<title_id>/trophies_<user_id>_<label>.json).
inline bool WriteUnlocks(const std::filesystem::path& data_dir, std::string_view title_id,
                         int user_id, std::uint32_t service_label,
                         const kyty::core::TrophyUnlocks& unlocks) {
	Common::Trophies::UnlockData data;
	data.unlocked   = unlocks.unlocked;
	data.timestamps = unlocks.timestamps;
	return Common::Trophies::SaveUnlockData(
	    Common::Trophies::UnlocksPath(data_dir, title_id, user_id, service_label), data);
}

// What LoadUnlockData reads back from a file WriteUnlocks wrote: the unlocked IDs (at least 0) and
// the times of unlocked IDs from 1970-01-01 up to the year 9999.
inline kyty::core::TrophyUnlocks StoredUnlocks(const kyty::core::TrophyUnlocks& unlocks) {
	constexpr std::uint64_t   kLastTick = 315537897599999999ULL;
	kyty::core::TrophyUnlocks stored;
	for (const int id: unlocks.unlocked) {
		if (id >= 0) {
			stored.unlocked.insert(id);
		}
	}
	for (const auto& [id, tick]: unlocks.timestamps) {
		if (stored.unlocked.contains(id) && tick >= Common::Trophies::UnixEpochTick &&
		    tick <= kLastTick) {
			stored.timestamps.emplace(id, tick);
		}
	}
	return stored;
}

// ---- Add-on packages

// "UP0000-<title_id>_00-<label>": title_id is 4 upper-case letters and 5 digits, label 16
// upper-case letters or digits, as Common::FindAddContent requires.
inline std::string AddOnContentId(std::string_view title_id, std::string_view label) {
	return "UP0000-" + std::string(title_id) + "_00-" + std::string(label);
}

// The sce_sys/param.json of an add-on package (or of an application with application = true, which
// FindAddContent skips). A non-empty title is the en-US localized title name.
inline std::string AddOnParamJson(std::string_view content_id, std::string_view title,
                                  bool application = false) {
	std::string json = "{\"contentId\":" + JsonString(content_id);
	if (!title.empty()) {
		json +=
		    ",\"localizedParameters\":{\"defaultLanguage\":\"en-US\",\"en-US\":{\"titleName\":" +
		    JsonString(title) + "}}";
	}
	if (application) {
		json += ",\"applicationCategoryType\":0";
	}
	return json + "}";
}

// ---- Trees

// Every entry below root by generic relative path: "D" for a directory, "F:<bytes>" for a regular
// file, "L:<target>" for a symbolic link (never followed), "?" for anything else.
inline std::map<std::string, std::string> SnapshotTree(const std::filesystem::path& root) {
	namespace fs = std::filesystem;
	std::map<std::string, std::string> tree;
	std::error_code                    ec;
	for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
		const auto      relative = it->path().lexically_relative(root).generic_string();
		std::error_code status_ec;
		switch (it->symlink_status(status_ec).type()) {
			case fs::file_type::directory: tree[relative] = "D"; break;
			case fs::file_type::regular: {
				std::ifstream stream(it->path(), std::ios::binary);
				tree[relative] = "F:" + std::string(std::istreambuf_iterator<char>(stream),
				                                    std::istreambuf_iterator<char>());
				break;
			}
			case fs::file_type::symlink:
				tree[relative] = "L:" + fs::read_symlink(it->path(), status_ec).string();
				break;
			default: tree[relative] = "?"; break;
		}
	}
	if (ec) {
		tree["<error>"] = ec.message();
	}
	return tree;
}

// Writes bytes to path, creating its parent directories.
inline bool WriteHostFile(const std::filesystem::path& path, std::string_view bytes) {
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	return static_cast<bool>(stream);
}

// ---- Save data removal

// Fails, without removing anything, for the paths given to FailOn; passes every other path to
// kyty::core::HostPathRemover(). before_remove runs before each call (a test can change the tree
// there).
class FailingRemover final: public kyty::core::PathRemover {
public:
	void FailOn(const std::filesystem::path& path, std::errc error = std::errc::permission_denied) {
		m_failures[path] = std::make_error_code(error);
	}

	std::error_code Remove(const std::filesystem::path& path) override {
		m_calls.push_back(path);
		if (before_remove) {
			before_remove(path);
		}
		if (const auto found = m_failures.find(path); found != m_failures.end()) {
			return found->second;
		}
		return kyty::core::HostPathRemover().Remove(path);
	}

	// Every path Remove was called with, in order.
	[[nodiscard]] const std::vector<std::filesystem::path>& Calls() const { return m_calls; }

	std::function<void(const std::filesystem::path&)> before_remove;

private:
	std::map<std::filesystem::path, std::error_code> m_failures;
	std::vector<std::filesystem::path>               m_calls;
};

// ---- Packer scripts

struct PackerScript {
	enum class End {
		Success, // every progress call is made, then the packer succeeds
		Cancel,  // cancellation is requested just before progress call `at`
		Fail,    // the packer fails after `at` progress calls (at <= calls.size())
	};
	std::vector<std::pair<std::uint64_t, std::uint64_t>> calls; // the (done, total) arguments
	End                                                  end = End::Success;
	std::size_t                                          at  = 0;
	// The packer writes the archive before its first progress call, and leaves it when it fails or
	// stops (a packer that does not clean up; Common::PackArchive removes its own).
	bool create_archive = true;
	bool leave_partial  = true;
};

struct PackerLog {
	bool        invoked = false;
	std::size_t calls   = 0;     // progress calls made
	bool        refused = false; // a progress call returned false
};

// A packer that follows script. request_cancel is called just before call `at` of a Cancel script
// (the test cancels its CancelToken there).
inline kyty::core::ArchivePacker ScriptedPacker(PackerScript               script,
                                                std::function<void()>      request_cancel,
                                                std::shared_ptr<PackerLog> log) {
	return [script = std::move(script), request_cancel = std::move(request_cancel),
	        log = std::move(log)](
	           const std::filesystem::path& /*directory*/, const std::filesystem::path& archive,
	           const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
		log->invoked = true;
		if (script.create_archive && !WriteHostFile(archive, "partial archive")) {
			return false;
		}
		const auto stop = [&]() {
			if (!script.leave_partial) {
				std::error_code ec;
				std::filesystem::remove(archive, ec);
			}
			return false;
		};
		for (std::size_t i = 0; i < script.calls.size(); i++) {
			if (script.end == PackerScript::End::Fail && i == script.at) {
				return stop();
			}
			if (script.end == PackerScript::End::Cancel && i == script.at && request_cancel) {
				request_cancel();
			}
			log->calls++;
			if (!progress(script.calls[i].first, script.calls[i].second)) {
				log->refused = true;
				return stop();
			}
		}
		if (script.end == PackerScript::End::Fail) {
			return stop();
		}
		return true;
	};
}

// ---- Cheat documents

// One member of a JSON object: its key and its value as JSON text.
struct JsonMember {
	std::string key;
	std::string value;
};
using JsonObjectSpec = std::vector<JsonMember>;

// The object's JSON text, members in order (keys must be unique for a predictable document).
inline std::string JsonObjectText(const JsonObjectSpec& members) {
	std::string text = "{";
	for (std::size_t i = 0; i < members.size(); i++) {
		text += (i == 0 ? "" : ",") + JsonString(members[i].key) + ":" + members[i].value;
	}
	return text + "}";
}

inline std::string JsonArrayText(const std::vector<std::string>& values) {
	std::string text = "[";
	for (std::size_t i = 0; i < values.size(); i++) {
		text += (i == 0 ? "" : ",") + values[i];
	}
	return text + "]";
}

// A cheat file: the top-level members before "mods", the mods, and the members after it.
struct CheatDocumentSpec {
	JsonObjectSpec              before;
	std::vector<JsonObjectSpec> mods;
	JsonObjectSpec              after;
};

inline std::string CheatDocumentText(const CheatDocumentSpec& spec) {
	std::vector<std::string> mods;
	for (const auto& mod: spec.mods) {
		mods.push_back(JsonObjectText(mod));
	}
	JsonObjectSpec members = spec.before;
	members.push_back({"mods", JsonArrayText(mods)});
	members.insert(members.end(), spec.after.begin(), spec.after.end());
	return JsonObjectText(members);
}

// The document an import writes for spec: every mod's "enabled" is false, in its place when the
// mod had one, else appended (as nlohmann::ordered_json adds a member).
inline CheatDocumentSpec AllDisabledSpec(CheatDocumentSpec spec) {
	for (auto& mod: spec.mods) {
		const auto found = std::find_if(mod.begin(), mod.end(),
		                                [](const JsonMember& m) { return m.key == "enabled"; });
		if (found != mod.end()) {
			found->value = "false";
		} else {
			mod.push_back({"enabled", "false"});
		}
	}
	return spec;
}

// A memory write that ValidateRemoteImport accepts.
inline std::string MemoryWriteJson(std::string_view offset, std::string_view on,
                                   std::string_view off) {
	return JsonObjectText(
	    {{"offset", JsonString(offset)}, {"on", JsonString(on)}, {"off", JsonString(off)}});
}

// A document that ValidateRemoteImport accepts for title_id, version, and process: one mod per
// name, each with one memory write.
inline CheatDocumentSpec RemoteCheatSpec(std::string_view title_id, std::string_view version,
                                         std::string_view                process,
                                         const std::vector<std::string>& names) {
	CheatDocumentSpec spec;
	spec.before = {{"id", JsonString(title_id)},
	               {"name", JsonString("Test game")},
	               {"version", JsonString(version)},
	               {"process", JsonString(process)},
	               {"credits", JsonArrayText({JsonString("Tester")})}};
	for (const auto& name: names) {
		spec.mods.push_back(
		    {{"name", JsonString(name)},
		     {"memory", JsonArrayText({MemoryWriteJson("0x1234", "9090", "0F85")})}});
	}
	return spec;
}

// ---- Remote responses

// A collection file name, "<title_id>_<version>_<rest>.<format>".
inline std::string CheatFileName(std::string_view title_id, std::string_view version,
                                 std::string_view rest, std::string_view format) {
	return std::string(title_id) + "_" + std::string(version) + "_" + std::string(rest) + "." +
	       std::string(format);
}

// An index line, "<name>=<title>".
inline std::string CheatIndexLine(std::string_view name, std::string_view title) {
	return std::string(name) + "=" + std::string(title);
}

// prefix, then filler bytes, then suffix: exactly size bytes (at least prefix + suffix).
inline std::string BodyOfSize(std::string_view prefix, std::string_view suffix, std::size_t size,
                              char filler = ' ') {
	std::string body(prefix);
	if (size > prefix.size() + suffix.size()) {
		body.append(size - prefix.size() - suffix.size(), filler);
	}
	body += suffix;
	return body;
}

// A successful response with body, delivered in the given chunks.
inline FakeResponse ResponseOf(std::string body, std::vector<std::size_t> chunk_sizes = {}) {
	FakeResponse response;
	response.body        = std::move(body);
	response.chunk_sizes = std::move(chunk_sizes);
	return response;
}

// ---- Input mapping

// Key names the Emulator accepts, of every kind the editor produces, plus mouse buttons.
inline const std::vector<std::string>& KeyNamePool() {
	static const std::vector<std::string> names = {"J",
	                                               "K",
	                                               "L",
	                                               "A",
	                                               "1",
	                                               "0",
	                                               "F6",
	                                               "F12",
	                                               "Space",
	                                               "Return",
	                                               "Backspace",
	                                               "Tab",
	                                               "Left Shift",
	                                               "Left Ctrl",
	                                               "Left Alt",
	                                               "Left GUI",
	                                               "Up",
	                                               "PageDown",
	                                               "Keypad 5",
	                                               "Keypad ,",
	                                               "Keypad Enter",
	                                               ",",
	                                               "=",
	                                               "\"",
	                                               "\xC3\x89",
	                                               "Mouse:Left",
	                                               "Mouse:Right",
	                                               "Mouse:X1",
	                                               "Mouse:Middle",
	                                               "Mouse:X2"};
	return names;
}

// host_input_mapping entries of every kind: bindings, the section and touch pad settings with
// valid and odd values, and entries the model keeps as unknown (some without "=", which
// Validate refuses).
inline const std::vector<std::string>& MappingEntryPool() {
	static const std::vector<std::string> entries = {
	    "Cross=J",
	    "Circle=L",
	    "Cross=K",
	    "R2=Mouse:Right",
	    "L3=Left Shift",
	    "TouchPadMode=F6",
	    "LeftStickUp=W",
	    "SpeakerVolume=1",
	    "Options=j",
	    "Triangle=\xC3\x89",
	    "Square=Keypad ,",
	    "Keyboard=Disabled",
	    "Pad=Disabled",
	    "Pad:Cross=South",
	    "Pad:Circle=South",
	    "Pad:L2=LeftTrigger",
	    "Pad:TouchPadRight=Misc1",
	    "Pad:SpeakerVolume=LeftPaddle1",
	    "Pad:LeftStickUp=North",
	    "Pad:Cross=Nothing",
	    "Pad:Unknown=South",
	    "MouseSensitivity=2.5",
	    "MouseSensitivity=0.04",
	    "MouseSensitivity=9",
	    "MouseSensitivity= 1.26x",
	    "MouseSensitivity=nan",
	    "MouseSensitivity=",
	    "TouchParticleSkin=Crimson",
	    "TouchParticleSkin=BlueNeon",
	    "TouchParticleSkin=Sparkles",
	    "TouchCursorImage=Off",
	    "TouchCursorImage=On",
	    "Foo=Bar",
	    "Keyboard=disabled",
	    "Cross=",
	    "=J",
	    "NoEquals",
	    "",
	};
	return entries;
}

struct MappingEdit {
	enum class Kind { Assign, Unmap, Restore, Sensitivity, Skin, Cursor };
	Kind                          kind   = Kind::Assign;
	kyty::core::InputDevice       device = kyty::core::InputDevice::KeyboardMouse;
	std::string                   control;
	std::string                   input;
	kyty::core::InputTab          tab           = kyty::core::InputTab::Gamepad;
	int                           tenths        = kyty::core::kDefaultMouseSensitivityTenths;
	kyty::core::TouchParticleSkin particle_skin = kyty::core::TouchParticleSkin::Classic;
	bool                          touch_cursor  = true;
};

// Runs one editor operation; the result of Assign or Unmap (true for the others).
inline bool ApplyMappingEdit(kyty::core::InputMappingModel& model, const MappingEdit& edit) {
	using Kind = MappingEdit::Kind;
	switch (edit.kind) {
		case Kind::Assign: return kyty::core::Assign(model, edit.device, edit.control, edit.input);
		case Kind::Unmap: return kyty::core::Unmap(model, edit.device, edit.control);
		case Kind::Restore: kyty::core::RestoreDefaults(model, edit.tab); return true;
		case Kind::Sensitivity: kyty::core::SetMouseSensitivity(model, edit.tenths); return true;
		case Kind::Skin: model.particle_skin = edit.particle_skin; return true;
		case Kind::Cursor: model.touch_cursor = edit.touch_cursor; return true;
	}
	return false;
}

} // namespace KytyTests::ContentGen
