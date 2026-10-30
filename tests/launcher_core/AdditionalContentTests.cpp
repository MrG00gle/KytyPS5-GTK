// Example tests of the additional content listing (src/launcher/core/additionalContentModel.h,
// task 16.1; Requirement 11.3): title ID matching after trimming and case folding, the title or
// entitlement label shown, the status messages, and LoadAdditionalContent over real add-on package
// trees read by Common::FindAddContent.

#include "launcher/core/additionalContentModel.h"
#include "support/contentGenerators.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace ContentGen = KytyTests::ContentGen;

using kyty::core::AdditionalContentItem;
using kyty::core::AdditionalContentStatus;
using kyty::core::AddOnPackage;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "AdditionalContentTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::vector<std::string> Titles(const std::vector<AdditionalContentItem>& items) {
	std::vector<std::string> titles;
	for (const auto& item: items) {
		titles.push_back(item.display_title);
	}
	return titles;
}

void TestList() {
	const std::vector<AddOnPackage> packages = {
	    {"PPSA01234", "UP0000-PPSA01234_00-SEASONPASS000001", "SEASONPASS000001", "Season Pass",
	     "/addons/a"},
	    {"ppsa01234", "UP0000-PPSA01234_00-COSTUMEPACK00002", "COSTUMEPACK00002", "", "/addons/b"},
	    {"PPSA01235", "UP0000-PPSA01235_00-OTHERTITLE000004", "OTHERTITLE000004", "Other",
	     "/addons/c"},
	    {" PPSA01234\t", "content", "SPACED", "Spaced", "/addons/d"},
	    {"PPSA012345", "content", "LONGER", "Longer", "/addons/e"},
	    {"", "content", "EMPTY", "Empty", "/addons/f"},
	};
	for (const std::string_view title: {"PPSA01234", "  ppsa01234 ", "pPsA01234",
	                                    "\xE3\x80\x80Ppsa01234\xC2\xA0", "\nPPSA01234"}) {
		const auto items = kyty::core::ListAdditionalContent(packages, title);
		Check(Titles(items) ==
		          std::vector<std::string> {"Season Pass", "COSTUMEPACK00002", "Spaced"},
		      "'" + std::string(title) +
		          "' lists the trimmed, case-folded matches in input order, "
		          "the label when the title is empty");
		Check(items[0] == AdditionalContentItem {"Season Pass",
		                                         "UP0000-PPSA01234_00-SEASONPASS000001",
		                                         "/addons/a"} &&
		          items[1].content_id == "UP0000-PPSA01234_00-COSTUMEPACK00002" &&
		          items[1].directory == "/addons/b",
		      "content ID and directory of each item");
	}
	for (const std::string_view title: {"", "   ", "PPSA0123", "CUSA01234", "PPSA01234X"}) {
		Check(kyty::core::ListAdditionalContent(packages, title).empty(),
		      "'" + std::string(title) + "' lists nothing (an empty title ID matches nothing)");
	}
	Check(kyty::core::ListAdditionalContent({}, "PPSA01234").empty(), "no packages");
}

void TestMessages() {
	const fs::path folder = "/media/addons";
	Check(kyty::core::AdditionalContentMessage(AdditionalContentStatus::Listed, folder, 1) ==
	          "1 installed add-on(s)",
	      "one item");
	Check(kyty::core::AdditionalContentMessage(AdditionalContentStatus::Listed, folder, 12) ==
	          "12 installed add-on(s)",
	      "twelve items");
	Check(kyty::core::AdditionalContentMessage(AdditionalContentStatus::FolderNotSet, {}, 0) ==
	          "No add-ons folder is set. Open Global settings and set the Add-ons folder to the "
	          "location of your extracted add-on packages.",
	      "folder not set");
	Check(
	    kyty::core::AdditionalContentMessage(AdditionalContentStatus::FolderMissing, folder, 0) ==
	        "The add-ons folder does not exist:\n/media/addons\nCheck the path in Global settings.",
	    "folder missing");
	Check(kyty::core::AdditionalContentMessage(AdditionalContentStatus::NoItems, folder, 0) ==
	          "No add-ons found for this game in:\n/media/addons",
	      "no items");
}

void WriteAddOn(const KytyTests::TempDir& temp, const fs::path& relative, std::string_view title_id,
                std::string_view label, std::string_view title, bool application = false,
                const char* param = "sce_sys/param.json") {
	temp.WriteFile(relative / param,
	               ContentGen::AddOnParamJson(ContentGen::AddOnContentId(title_id, label), title,
	                                          application));
}

void TestLoad() {
	const KytyTests::TempDir temp("kyty_addons");

	const auto not_set = kyty::core::LoadAdditionalContent({}, "PPSA01234");
	Check(not_set.status == AdditionalContentStatus::FolderNotSet && not_set.items.empty() &&
	          !not_set.folder_exists &&
	          not_set.message == kyty::core::AdditionalContentMessage(
	                                 AdditionalContentStatus::FolderNotSet, {}, 0),
	      "an empty add-ons folder setting");

	const auto missing_path = temp.Path() / "missing";
	const auto missing      = kyty::core::LoadAdditionalContent(missing_path, "PPSA01234");
	Check(missing.status == AdditionalContentStatus::FolderMissing && !missing.folder_exists &&
	          missing.message == "The add-ons folder does not exist:\n" + missing_path.string() +
	                                 "\nCheck the path in Global settings.",
	      "a missing add-ons folder");
	const auto file = temp.WriteFile("not-a-folder", "x");
	Check(kyty::core::LoadAdditionalContent(file, "PPSA01234").status ==
	          AdditionalContentStatus::FolderMissing,
	      "a file is not an add-ons folder");

	const fs::path addons = "addons";
	WriteAddOn(temp, addons / "other", "PPSA09999", "OTHERTITLE000004", "Other game");
	WriteAddOn(temp, addons / "app", "PPSA01234", "APPLICATION00005", "The game", true);
	const auto folder = temp.Path() / addons;
	const auto none   = kyty::core::LoadAdditionalContent(folder, "PPSA01234");
	Check(none.status == AdditionalContentStatus::NoItems && none.items.empty() &&
	          none.folder_exists &&
	          none.message == "No add-ons found for this game in:\n" + folder.string(),
	      "add-ons of other titles and an application do not count");

	WriteAddOn(temp, addons / "dlc1", "PPSA01234", "SEASONPASS000001", "Season Pass");
	WriteAddOn(temp, addons / "nested/deeper/dlc2", "PPSA01234", "COSTUMEPACK00002", "");
	WriteAddOn(temp, addons / "dup", "PPSA01234", "SEASONPASS000001", "Duplicate");
	WriteAddOn(temp, addons / "upper", "PPSA01234", "UPPERCASE0000003", "Upper", false,
	           "SCE_SYS/PARAM.JSON");
	const auto listed = kyty::core::LoadAdditionalContent(folder, " ppsa01234 ");
	Check(listed.status == AdditionalContentStatus::Listed && listed.folder_exists &&
	          listed.message == "3 installed add-on(s)",
	      "three add-ons listed: " + listed.message);
	Check(Titles(listed.items) ==
	          std::vector<std::string> {"COSTUMEPACK00002", "Season Pass", "Upper"},
	      "one item per entitlement label in label order; the label when the title is empty");
	Check(listed.items[0] == AdditionalContentItem {"COSTUMEPACK00002",
	                                                "UP0000-PPSA01234_00-COSTUMEPACK00002",
	                                                folder / "nested/deeper/dlc2"} &&
	          listed.items[1].directory == folder / "dlc1" &&
	          listed.items[2].directory == folder / "upper",
	      "the item directories (the first package of a duplicated label wins)");

	const auto link = temp.MakeSymlink(folder, "linked-addons");
	const auto one  = kyty::core::LoadAdditionalContent(link, "PPSA09999");
	Check(one.status == AdditionalContentStatus::Listed && one.folder_exists &&
	          one.message == "1 installed add-on(s)" &&
	          Titles(one.items) == std::vector<std::string> {"Other game"},
	      "a link to the add-ons folder is followed");
	const auto empty_title = kyty::core::LoadAdditionalContent(folder, "  ");
	Check(empty_title.status == AdditionalContentStatus::NoItems && empty_title.items.empty(),
	      "an empty title ID has no add-ons");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("AdditionalContentTests");
	TestList();
	TestMessages();
	TestLoad();
	std::printf("AdditionalContentTests: all tests passed\n");
	return 0;
}
