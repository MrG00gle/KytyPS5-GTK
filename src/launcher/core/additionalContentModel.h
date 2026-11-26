#pragma once

// The additional content (add-on) listing of a Game_Entry (task 16.1; design.md, "Over
// core::AdditionalContentModel"; Requirement 11.3), ported from the Qt launcher's additional
// content dialog (src/launcher/src/additionalContentDialog.cpp). ListAdditionalContent is pure;
// LoadAdditionalContent finds the packages in the add-ons folder through Common::FindAddContent and
// gives the dialog's status message. The user add-ons per title come from upstream KytyPS5's
// launcher (commit 66e21a62).

#include "launcher/core/expected.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kyty::core {

// One add-on package in the add-ons folder (Common::AddContentEntry).
struct AddOnPackage {
	std::string           title_id;          // the title the package belongs to: contentId[7, 16)
	std::string           content_id;        // "UP0000-PPSA01234_00-<16 characters>"
	std::string           entitlement_label; // contentId[20, 36)
	std::string           title;             // the package's title name; may be empty
	std::filesystem::path directory;         // the package root, which holds sce_sys/param.json
};

// One listed add-on.
struct AdditionalContentItem {
	std::string           display_title; // the title, or the entitlement label when it is empty
	std::string           content_id;    // the Qt launcher's tooltip
	std::filesystem::path directory;     // where the item's sce_sys/icon0.png is
	// A user add-on (see below): its entry as stored, "" for a package of the add-ons folder. Its
	// display_title is the entitlement label, content_id is empty, and directory is the data folder
	// (empty for a license only).
	std::string user_entry;
	// A user add-on whose data folder is not an existing directory (links are followed).
	bool missing_directory = false;

	friend bool operator==(const AdditionalContentItem&, const AdditionalContentItem&) = default;
};

// ---- User add-ons (upstream KytyPS5's launcher): a game's own list of add-ons, stored by title ID
// in Kyty.ini's [AdditionalContent] group (settingsStore.h). An entry is "LABEL", a license
// without extra data, or "LABEL=DIR", an add-on whose data is in the folder DIR; the Emulator gets
// each one as --addcont <entry> (Common::ParseAddContent), where it replaces a package of the
// add-ons folder with the same entitlement label.

// The entitlement label of an entry: the text before the first '=' (the whole entry without one).
std::string_view UserAddOnLabel(std::string_view entry);
// The data folder of an entry: the text after the first '=', or "" for a license only.
std::string_view UserAddOnDirectory(std::string_view entry);
// An entitlement label as the Emulator accepts it: 16 characters, each A-Z or 0-9.
bool IsEntitlementLabel(std::string_view label);
// The form --addcont accepts, without looking at the file system: an entitlement label, then
// nothing, or '=' and a non-empty folder.
bool IsUserAddOnEntry(std::string_view entry);

enum class UserAddOnError {
	InvalidLabel,  // the label is not an entitlement label
	MissingFolder, // the data folder is not an existing directory
};

// The entry that the add-ons window adds, made as upstream's launcher makes it: of
// label_or_content_id (trimmed), the part after its last '-', so that a content ID
// ("UP0000-PPSA01234_00-<label>") gives its label; and, for a non-empty folder (trimmed), "=" and
// the folder made absolute (std::filesystem::absolute, lexically normal, without a trailing
// separator). The label must be an entitlement label and a non-empty folder an existing
// directory (links are followed), else the error.
Expected<std::string, UserAddOnError> MakeUserAddOn(std::string_view label_or_content_id,
                                                    std::string_view folder);
// Adds entry at the end of entries; an entry with the same label goes (one entry per label, as
// upstream's launcher keeps them).
void AddUserAddOn(std::vector<std::string>& entries, std::string entry);
// Removes the first entry equal to entry; false when there is none.
bool RemoveUserAddOn(std::vector<std::string>& entries, std::string_view entry);

// The packages whose title ID equals title_id after NormalizeTitleId on both (titleId.h: trimmed,
// ASCII letters upper-cased), in input order; nothing when the normalized title_id is empty (the
// Qt launcher listed nothing for an empty title ID).
std::vector<AdditionalContentItem> ListAdditionalContent(std::span<const AddOnPackage> packages,
                                                         std::string_view              title_id);

enum class AdditionalContentStatus {
	Listed,        // at least one item (a package or a user add-on)
	FolderNotSet,  // no item, and the add-ons folder setting is empty
	FolderMissing, // no item, and the add-ons folder is not an existing directory
	NoItems, // the folder exists and has no add-on for the title ID, and there is no user add-on
};

// The status line of the additional content dialog, the texts of upstream's launcher:
// - Listed: "<count> add-on(s)";
// - FolderNotSet: "No add-ons folder is set. Open Global settings and set the Add-ons folder to the
//   location of your extracted add-on packages.";
// - FolderMissing: "The add-ons folder does not exist:\n<folder>\nCheck the path in Global
//   settings.";
// - NoItems: "No add-ons found for this game in:\n<folder>".
std::string AdditionalContentMessage(AdditionalContentStatus      status,
                                     const std::filesystem::path& folder, std::size_t count);

struct AdditionalContentListing {
	std::vector<AdditionalContentItem> items;
	AdditionalContentStatus            status = AdditionalContentStatus::FolderNotSet;
	bool        folder_exists = false; // the Qt launcher enabled "Open folder" only then
	std::string message;               // AdditionalContentMessage(status, folder, items.size())
};

// The add-ons of title_id in addons_dir (the global add-ons folder): Common::FindAddContent over
// the folder for NormalizeTitleId(title_id) (every sce_sys/param.json below it of a package, not an
// application, whose content ID names the title; one package per entitlement label, sorted by
// label), then ListAdditionalContent, then one item per entry of user_addons (the game's user
// add-ons, in their order, as stored). The status is Listed when there is an item; otherwise
// FolderNotSet for an empty addons_dir, FolderMissing when it is not a directory (links are
// followed), and NoItems.
AdditionalContentListing LoadAdditionalContent(const std::filesystem::path& addons_dir,
                                               std::string_view             title_id,
                                               std::span<const std::string> user_addons = {});

} // namespace kyty::core
