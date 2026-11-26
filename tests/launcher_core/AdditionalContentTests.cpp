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
	          "1 add-on(s)",
	      "one item");
	Check(kyty::core::AdditionalContentMessage(AdditionalContentStatus::Listed, folder, 12) ==
	          "12 add-on(s)",
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
	          listed.message == "3 add-on(s)",
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
	          one.message == "1 add-on(s)" &&
	          Titles(one.items) == std::vector<std::string> {"Other game"},
	      "a link to the add-ons folder is followed");
	const auto empty_title = kyty::core::LoadAdditionalContent(folder, "  ");
	Check(empty_title.status == AdditionalContentStatus::NoItems && empty_title.items.empty(),
	      "an empty title ID has no add-ons");
}

// The entries of upstream's user add-ons: their parts, the form --addcont accepts, the entry the
// add-ons window makes (upstream's AdditionalContentDialog::Add), and the list edits.
void TestUserAddOnEntries() {
	using kyty::core::UserAddOnError;
	Check(kyty::core::UserAddOnLabel("SEASONPASS000001=/dlc/a=b") == "SEASONPASS000001" &&
	          kyty::core::UserAddOnDirectory("SEASONPASS000001=/dlc/a=b") == "/dlc/a=b" &&
	          kyty::core::UserAddOnLabel("LICENSE000000001") == "LICENSE000000001" &&
	          kyty::core::UserAddOnDirectory("LICENSE000000001").empty(),
	      "the label is before the first '=' and the folder after it");
	for (const std::string_view entry:
	     {"LICENSE000000001", "SEASONPASS000001=/dlc", "0123456789ABCDEF=relative/dir"}) {
		Check(kyty::core::IsUserAddOnEntry(entry), "accepted: " + std::string(entry));
	}
	for (const std::string_view entry:
	     {"", "SHORT", "lowercase0000000", "LICENSE0000000001", "LICENSE00000000_",
	      "LICENSE000000001=", "=/dlc", " LICENSE000000001", "LICENSE00000000\xC3\x89"}) {
		Check(!kyty::core::IsUserAddOnEntry(entry), "rejected: '" + std::string(entry) + "'");
	}

	const KytyTests::TempDir temp("kyty_user_addons");
	const auto               data = temp.Path() / "dlc data";
	fs::create_directories(data);
	const auto license = kyty::core::MakeUserAddOn("  LICENSE000000001\t", "  ");
	Check(license && *license == "LICENSE000000001", "a license only: the trimmed label");
	const auto content_id =
	    kyty::core::MakeUserAddOn("UP0000-PPSA01234_00-SEASONPASS000001", data.string() + "/");
	Check(content_id && *content_id == "SEASONPASS000001=" + data.string(),
	      "a content ID gives its label; the folder without its trailing separator");
	const auto dotted = kyty::core::MakeUserAddOn("SEASONPASS000001",
	                                              (data / ".." / "dlc data" / "." / "").string());
	Check(dotted && *dotted == "SEASONPASS000001=" + data.string(),
	      "the folder is made lexically normal");
	const auto relative = kyty::core::MakeUserAddOn("SEASONPASS000001", ".");
	Check(relative && *relative == "SEASONPASS000001=" + fs::current_path().string(),
	      "a relative folder is made absolute");
	const auto file = temp.WriteFile("not-a-folder", "x");
	Check(kyty::core::MakeUserAddOn("SEASONPASS000001", file.string()).error() ==
	              UserAddOnError::MissingFolder &&
	          kyty::core::MakeUserAddOn("SEASONPASS000001", (temp.Path() / "missing").string())
	                  .error() == UserAddOnError::MissingFolder,
	      "the data folder must be an existing directory");
	for (const std::string_view label:
	     {"", "SHORT", "seasonpass000001", "UP0000-PPSA01234_00-", "SEASONPASS0000012"}) {
		Check(kyty::core::MakeUserAddOn(label, data.string()).error() ==
		          UserAddOnError::InvalidLabel,
		      "an invalid label: '" + std::string(label) + "'");
	}

	std::vector<std::string> entries = {"LICENSE000000001", "SEASONPASS000001=/old"};
	kyty::core::AddUserAddOn(entries, "SEASONPASS000001=/new");
	kyty::core::AddUserAddOn(entries, "COSTUMEPACK00002");
	Check(entries == std::vector<std::string> {"LICENSE000000001", "SEASONPASS000001=/new",
	                                           "COSTUMEPACK00002"},
	      "an entry replaces the one with its label and goes to the end");
	Check(kyty::core::RemoveUserAddOn(entries, "SEASONPASS000001=/new") &&
	          !kyty::core::RemoveUserAddOn(entries, "SEASONPASS000001=/new") &&
	          !kyty::core::RemoveUserAddOn(entries, "LICENSE000000001=/x") &&
	          entries == std::vector<std::string> {"LICENSE000000001", "COSTUMEPACK00002"},
	      "Remove takes the equal entry only");
}

// LoadAdditionalContent lists the user add-ons after the packages, with the data folder's state,
// and counts them in the status.
void TestLoadUserAddOns() {
	const KytyTests::TempDir temp("kyty_user_addons_load");
	const auto               data    = temp.Path() / "data";
	const auto               missing = temp.Path() / "missing";
	fs::create_directories(data);
	const std::vector<std::string> user = {"LICENSE000000001", "SEASONPASS000001=" + data.string(),
	                                       "COSTUMEPACK00002=" + missing.string(), "not an entry"};

	const auto not_set = kyty::core::LoadAdditionalContent({}, "PPSA01234", user);
	Check(not_set.status == AdditionalContentStatus::Listed && !not_set.folder_exists &&
	          not_set.message == "4 add-on(s)" && not_set.items.size() == 4,
	      "user add-ons are listed without an add-ons folder: " + not_set.message);
	Check(not_set.items[0] ==
	              AdditionalContentItem {"LICENSE000000001", "", "", "LICENSE000000001", false} &&
	          not_set.items[1] == AdditionalContentItem {"SEASONPASS000001", "", data,
	                                                     "SEASONPASS000001=" + data.string(),
	                                                     false} &&
	          not_set.items[2] == AdditionalContentItem {"COSTUMEPACK00002", "", missing,
	                                                     "COSTUMEPACK00002=" + missing.string(),
	                                                     true} &&
	          not_set.items[3].display_title == "not an entry" &&
	          not_set.items[3].user_entry == "not an entry",
	      "each entry as stored, with its label, folder, and whether the folder is missing");

	const fs::path addons = "addons";
	WriteAddOn(temp, addons / "dlc1", "PPSA01234", "SEASONPASS000001", "Season Pass");
	const auto both =
	    kyty::core::LoadAdditionalContent(temp.Path() / addons, "PPSA01234", {user.data(), 1});
	Check(both.status == AdditionalContentStatus::Listed && both.message == "2 add-on(s)" &&
	          Titles(both.items) == std::vector<std::string> {"Season Pass", "LICENSE000000001"} &&
	          both.items[0].user_entry.empty(),
	      "the packages first, then the user add-ons");
	const auto none = kyty::core::LoadAdditionalContent(temp.Path() / "empty", "PPSA01234", {});
	Check(none.status == AdditionalContentStatus::FolderMissing && none.items.empty(),
	      "without user add-ons the folder's status stays");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("AdditionalContentTests");
	TestList();
	TestMessages();
	TestLoad();
	TestUserAddOnEntries();
	TestLoadUserAddOns();
	std::printf("AdditionalContentTests: all tests passed\n");
	return 0;
}
