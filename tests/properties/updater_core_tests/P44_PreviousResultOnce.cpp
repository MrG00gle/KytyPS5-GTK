// Feature: linux-gtk-modernization, Property 44: Previous update result is reported once
//
// Validates Requirement 13.14. Each case draws a status (success or failure) and a UTF-8 message
// of 0 to 65,536 bytes (empty, short, exactly 64 KiB, just below it, or any size up to it) made of
// ASCII characters (line feeds and NUL bytes included) and 2-, 3-, and 4-byte characters; in about
// one case in ten the message is longer than 64 KiB. Before the write, the result path in a real
// installation directory may hold an older result, a symbolic link to a file outside it, or an
// empty directory. WriteUpdateResult and then ReadAndClearPreviousResult twice must give the
// status and the message the first time (a longer message cut to the longest prefix of whole
// characters within 64 KiB, which the model tracks while it builds the message) and nothing the
// second time; nothing may stay at the path, and the file outside must be unchanged.

#include "support/property.h"
#include "support/tempDir.h"
#include "support/updaterGenerators.h"
#include "updater/core/updateResult.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace UpdaterGen = KytyTests::UpdaterGen;

constexpr size_t kLimit = kyty::updater::kMaxUpdateResultMessageBytes;

template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

// One character of a message.
std::string RandomCharacter() {
	switch (Choose(0, 10)) {
		case 0: return "\n";
		case 1: return std::string(1, '\0');
		case 2: return UpdaterGen::Utf8Character(Choose<uint32_t>(0x80, 0x800));
		case 3: {
			uint32_t code_point = Choose<uint32_t>(0x800, 0x10000);
			if (code_point >= 0xD800 && code_point <= 0xDFFF) {
				code_point = 0x20AC; // no surrogates in UTF-8
			}
			return UpdaterGen::Utf8Character(code_point);
		}
		case 4: return UpdaterGen::Utf8Character(Choose<uint32_t>(0x10000, 0x110000));
		default: return std::string(1, static_cast<char>(Choose(0x20, 0x7F)));
	}
}

size_t RandomSize() {
	switch (Choose(0, 10)) {
		case 0: return 0;
		case 1: return kLimit;
		case 2: return Choose<size_t>(kLimit - 8, kLimit + 1);
		case 3: return Choose<size_t>(kLimit + 1, kLimit + 5000);
		case 4:
		case 5: return Choose<size_t>(0, kLimit + 1);
		default: return Choose<size_t>(0, 300);
	}
}

bool Exists(const fs::path& path) {
	std::error_code ec;
	return fs::symlink_status(path, ec).type() != fs::file_type::not_found && !ec;
}

} // namespace

KYTY_PROPERTY(44, "Previous update result is reported once") {
	return rc::check(name, []() {
		const KytyTests::TempDir temp("kyty_p44");
		const auto               install = temp.MakeDirectories("install");
		const auto               outside = temp.WriteFile("outside", "failure\nnot this one");
		const auto               path    = install / kyty::updater::kUpdateResultFileName;
		temp.WriteFile("install/kyty_launcher", "launcher");

		// The message, and the longest prefix of whole characters within the limit.
		const bool   success = OneIn(2);
		const size_t size    = RandomSize();
		std::string  message;
		size_t       kept = 0;
		while (message.size() < size) {
			std::string character = RandomCharacter();
			if (message.size() + character.size() > size) {
				character = "y";
			}
			message += character;
			if (message.size() <= kLimit) {
				kept = message.size();
			}
		}
		const std::string expected = message.substr(0, kept);

		// What is at the path before the write.
		const int before = Choose(0, 8);
		switch (before) {
			case 0:
				temp.WriteFile("install/.kyty-update-result", "success\nan older result");
				break;
			case 1: temp.MakeSymlink(outside, "install/.kyty-update-result"); break;
			case 2: temp.MakeDirectories("install/.kyty-update-result"); break;
			default: break;
		}
		RC_LOG() << (success ? "success" : "failure") << ", " << message.size() << " bytes, "
		         << expected.size() << " expected, before " << before << "\n";

		const auto written = kyty::updater::WriteUpdateResult(install, {success, message});
		RC_ASSERT(written.has_value());
		const auto first = kyty::updater::ReadAndClearPreviousResult(install);
		RC_ASSERT(first.has_value());
		RC_ASSERT(first->success == success);
		RC_ASSERT(first->message.size() == expected.size());
		RC_ASSERT(first->message == expected);
		RC_ASSERT(!Exists(path));
		RC_ASSERT(!kyty::updater::ReadAndClearPreviousResult(install).has_value());
		RC_ASSERT(!Exists(path));
		RC_ASSERT(UpdaterGen::ReadFile(outside) == std::string("failure\nnot this one"));
		RC_TAG(message.size() > kLimit ? "cut" : message.size() == kLimit ? "64 KiB" : "within");
	});
}
