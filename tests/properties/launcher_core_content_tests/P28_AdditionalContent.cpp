// Feature: linux-gtk-modernization, Property 28: Additional content listing
//
// Validates Requirement 11.3. Each case draws a title ID (four letters and five digits) and an
// entry title ID that spells it in any letter case with ASCII and Unicode whitespace around it (or
// is empty or whitespace only), and 0-12 add-on packages whose title IDs are spellings of the same
// title, other titles that differ in one character, or empty, each with an empty or non-empty
// title and an entitlement label. ListAdditionalContent must list exactly the packages whose title
// ID equals the entry's after trimming and case folding, in input order, each showing its title or,
// when that is empty, its entitlement label; an empty entry title ID lists nothing. In about one
// case in four the packages are also written as real sce_sys/param.json files (with an application
// of the same title, which never counts) and read through LoadAdditionalContent, which must list
// the same packages in entitlement label order with the matching status message.

#include "launcher/core/additionalContentModel.h"
#include "support/contentGenerators.h"
#include "support/property.h"
#include "support/tempDir.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace ContentGen = KytyTests::ContentGen;

using kyty::core::AdditionalContentItem;
using kyty::core::AdditionalContentStatus;
using kyty::core::AddOnPackage;

template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

constexpr char kLetters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr char kDigits[]  = "0123456789";

std::string TitleId() {
	std::string id;
	for (int i = 0; i < 4; i++) {
		id += kLetters[Choose(0, 4)]; // few letters, so other titles look alike
	}
	for (int i = 0; i < 5; i++) {
		id += kDigits[Choose(0, 3)];
	}
	return id;
}

// The same title ID with one character replaced by another of its kind.
std::string OtherTitleId(std::string id) {
	const auto index = Choose<std::size_t>(0, id.size());
	const char old   = id[index];
	const bool digit = index >= 4;
	do {
		id[index] = digit ? kDigits[Choose(0, 10)] : kLetters[Choose(0, 26)];
	} while (id[index] == old);
	return id;
}

std::string Whitespace() {
	static const std::vector<std::string> kSpaces = {
	    " ", "\t", "\n", "\r", "\v", "\xC2\xA0", "\xE3\x80\x80", "\xE2\x80\x83"};
	std::string text;
	const int   count = OneIn(2) ? 0 : Choose(1, 4);
	for (int i = 0; i < count; i++) {
		text += kSpaces[Choose<std::size_t>(0, kSpaces.size())];
	}
	return text;
}

// id with random letter case, optionally surrounded by whitespace.
std::string Spelling(const std::string& id, bool spaces) {
	std::string text = id;
	for (auto& c: text) {
		if (c >= 'A' && c <= 'Z' && OneIn(2)) {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return spaces ? Whitespace() + text + Whitespace() : text;
}

std::string Label(std::set<std::string>& used) {
	std::string label;
	do {
		label.clear();
		for (int i = 0; i < 16; i++) {
			label += OneIn(2) ? kLetters[Choose(0, 26)] : kDigits[Choose(0, 10)];
		}
	} while (!used.insert(label).second);
	return label;
}

struct Drawn {
	AddOnPackage package;
	std::string  upper_title_id; // the title the package belongs to, upper case; empty for none
	bool         matches = false;
};

} // namespace

KYTY_PROPERTY(28, "Additional content listing") {
	return rc::check(name, []() {
		const auto base        = TitleId();
		const bool empty_entry = OneIn(10);
		const auto entry       = empty_entry ? Whitespace() : Spelling(base, true);

		std::set<std::string> labels;
		std::vector<Drawn>    drawn;
		const int             count = Choose(0, 13);
		for (int i = 0; i < count; i++) {
			Drawn item;
			switch (Choose(0, 5)) {
				case 0:
				case 1:
				case 2:
					item.upper_title_id   = base;
					item.package.title_id = Spelling(base, OneIn(4));
					item.matches          = !empty_entry;
					break;
				case 3:
					item.upper_title_id   = OtherTitleId(base);
					item.package.title_id = Spelling(item.upper_title_id, OneIn(4));
					break;
				default: break; // no title ID
			}
			item.package.entitlement_label = Label(labels);
			item.package.content_id        = ContentGen::AddOnContentId(
			    item.upper_title_id.empty() ? "XXXX00000" : item.upper_title_id,
			    item.package.entitlement_label);
			item.package.title = OneIn(3) ? "" : "Add-on \xE2\x80\x9C" + std::to_string(i) + "\"";
			item.package.directory = fs::path("/addons") / std::to_string(i);
			drawn.push_back(std::move(item));
		}

		std::vector<AddOnPackage>          packages;
		std::vector<AdditionalContentItem> expected;
		for (const auto& item: drawn) {
			packages.push_back(item.package);
			if (item.matches) {
				expected.push_back({item.package.title.empty() ? item.package.entitlement_label
				                                               : item.package.title,
				                    item.package.content_id, item.package.directory});
			}
		}
		RC_LOG() << "entry '" << entry << "', " << packages.size() << " packages, "
		         << expected.size() << " matching\n";
		RC_ASSERT(kyty::core::ListAdditionalContent(packages, entry) == expected);
		RC_CLASSIFY(expected.empty(), "nothing listed");
		RC_CLASSIFY(empty_entry, "empty entry title ID");

		if (!OneIn(4)) {
			return;
		}
		// The same packages as real files (only real title IDs: upper case, as content IDs are).
		const KytyTests::TempDir           temp("kyty_p28");
		const auto                         folder = temp.Path() / "addons";
		std::vector<AdditionalContentItem> on_disk;
		for (std::size_t i = 0; i < drawn.size(); i++) {
			const auto& item = drawn[i];
			if (item.upper_title_id.empty()) {
				continue;
			}
			const auto directory = fs::path("addons") / ("pkg" + std::to_string(i));
			temp.WriteFile(directory / "sce_sys/param.json",
			               ContentGen::AddOnParamJson(item.package.content_id, item.package.title));
			if (item.matches) {
				on_disk.push_back({item.package.title.empty() ? item.package.entitlement_label
				                                              : item.package.title,
				                   item.package.content_id, temp.Path() / directory});
			}
		}
		std::set<std::string> app_labels = labels;
		temp.WriteFile("addons/app/sce_sys/param.json",
		               ContentGen::AddOnParamJson(
		                   ContentGen::AddOnContentId(base, Label(app_labels)), "The game", true));
		std::sort(on_disk.begin(), on_disk.end(), [](const auto& a, const auto& b) {
			return a.content_id.substr(20) < b.content_id.substr(20);
		});
		const auto listing = kyty::core::LoadAdditionalContent(folder, entry);
		RC_ASSERT(listing.folder_exists);
		RC_ASSERT(listing.items == on_disk);
		RC_ASSERT(listing.status == (on_disk.empty() ? AdditionalContentStatus::NoItems
		                                             : AdditionalContentStatus::Listed));
		RC_ASSERT(listing.message ==
		          (on_disk.empty() ? "No add-ons found for this game in:\n" + folder.string()
		                           : std::to_string(on_disk.size()) + " installed add-on(s)"));
		RC_TAG("real files");
	});
}
