#pragma once

// The additional content (add-on) listing of a Game_Entry (task 16.1; design.md, "Over
// core::AdditionalContentModel"; Requirement 11.3), ported from the Qt launcher's additional
// content dialog (src/launcher/src/additionalContentDialog.cpp). ListAdditionalContent is pure;
// LoadAdditionalContent finds the packages in the add-ons folder through Common::FindAddContent and
// gives the dialog's status message.

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

	friend bool operator==(const AdditionalContentItem&, const AdditionalContentItem&) = default;
};

// The packages whose title ID equals title_id after NormalizeTitleId on both (titleId.h: trimmed,
// ASCII letters upper-cased), in input order; nothing when the normalized title_id is empty (the
// Qt launcher listed nothing for an empty title ID).
std::vector<AdditionalContentItem> ListAdditionalContent(std::span<const AddOnPackage> packages,
                                                         std::string_view              title_id);

enum class AdditionalContentStatus {
	Listed,        // at least one item
	FolderNotSet,  // the add-ons folder setting is empty
	FolderMissing, // the add-ons folder is not an existing directory
	NoItems,       // the folder exists and has no add-on for the title ID
};

// The status line of the additional content dialog, the Qt launcher's texts:
// - Listed: "<count> installed add-on(s)";
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
// label), then ListAdditionalContent. The status is FolderNotSet for an empty addons_dir,
// FolderMissing when it is not a directory (links are followed), NoItems when nothing is listed,
// and Listed otherwise.
AdditionalContentListing LoadAdditionalContent(const std::filesystem::path& addons_dir,
                                               std::string_view             title_id);

} // namespace kyty::core
