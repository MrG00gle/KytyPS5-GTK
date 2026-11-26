#include "launcher/core/additionalContentModel.h"

#include "common/additionalContent.h"
#include "launcher/core/text.h"
#include "launcher/core/titleId.h"

#include <algorithm>
#include <system_error>
#include <utility>

namespace kyty::core {

namespace {

// Common::FindAddContent accepts only content IDs of this shape, so the title ID and the
// entitlement label are at fixed offsets.
constexpr std::size_t kTitleIdOffset = 7;
constexpr std::size_t kTitleIdLength = 9;

// Common::ParseAddContent's label: ^[A-Z0-9]{16}$.
constexpr std::size_t kEntitlementLabelLength = 16;

bool IsDirectory(const std::filesystem::path& path) {
	std::error_code ec;
	return std::filesystem::is_directory(path, ec);
}

} // namespace

std::string_view UserAddOnLabel(std::string_view entry) {
	return entry.substr(0, entry.find('='));
}

std::string_view UserAddOnDirectory(std::string_view entry) {
	const auto equals = entry.find('=');
	return equals == std::string_view::npos ? std::string_view() : entry.substr(equals + 1);
}

bool IsEntitlementLabel(std::string_view label) {
	return label.size() == kEntitlementLabelLength &&
	       std::all_of(label.begin(), label.end(),
	                   [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
}

bool IsUserAddOnEntry(std::string_view entry) {
	return IsEntitlementLabel(UserAddOnLabel(entry)) &&
	       (entry.find('=') == std::string_view::npos || !UserAddOnDirectory(entry).empty());
}

Expected<std::string, UserAddOnError> MakeUserAddOn(std::string_view label_or_content_id,
                                                    std::string_view folder) {
	auto label = TrimWhitespace(label_or_content_id);
	if (const auto dash = label.rfind('-'); dash != std::string_view::npos) {
		label = label.substr(dash + 1);
	}
	if (!IsEntitlementLabel(label)) {
		return MakeUnexpected(UserAddOnError::InvalidLabel);
	}
	const auto directory = TrimWhitespace(folder);
	if (directory.empty()) {
		return std::string(label);
	}
	std::error_code ec;
	auto            absolute = std::filesystem::absolute(std::filesystem::path(directory), ec);
	if (ec) {
		absolute = std::filesystem::path(directory);
	}
	absolute = absolute.lexically_normal();
	// QDir::absolutePath() has no trailing separator.
	if (!absolute.has_filename() && absolute.has_relative_path()) {
		absolute = absolute.parent_path();
	}
	if (!IsDirectory(absolute)) {
		return MakeUnexpected(UserAddOnError::MissingFolder);
	}
	return std::string(label) + "=" + absolute.string();
}

void AddUserAddOn(std::vector<std::string>& entries, std::string entry) {
	const std::string label(UserAddOnLabel(entry));
	std::erase_if(entries,
	              [&label](const std::string& other) { return UserAddOnLabel(other) == label; });
	entries.push_back(std::move(entry));
}

bool RemoveUserAddOn(std::vector<std::string>& entries, std::string_view entry) {
	const auto found = std::find(entries.begin(), entries.end(), entry);
	if (found == entries.end()) {
		return false;
	}
	entries.erase(found);
	return true;
}

std::vector<AdditionalContentItem> ListAdditionalContent(std::span<const AddOnPackage> packages,
                                                         std::string_view              title_id) {
	std::vector<AdditionalContentItem> items;
	const auto                         wanted = NormalizeTitleId(title_id);
	if (wanted.empty()) {
		return items;
	}
	for (const auto& package: packages) {
		if (NormalizeTitleId(package.title_id) == wanted) {
			items.push_back({package.title.empty() ? package.entitlement_label : package.title,
			                 package.content_id, package.directory});
		}
	}
	return items;
}

std::string AdditionalContentMessage(AdditionalContentStatus      status,
                                     const std::filesystem::path& folder, std::size_t count) {
	switch (status) {
		case AdditionalContentStatus::Listed: return std::to_string(count) + " add-on(s)";
		case AdditionalContentStatus::FolderNotSet:
			return "No add-ons folder is set. Open Global settings and set the Add-ons "
			       "folder to the location of your extracted add-on packages.";
		case AdditionalContentStatus::FolderMissing:
			return "The add-ons folder does not exist:\n" + folder.string() +
			       "\nCheck the path in Global settings.";
		case AdditionalContentStatus::NoItems:
			return "No add-ons found for this game in:\n" + folder.string();
	}
	return {};
}

AdditionalContentListing LoadAdditionalContent(const std::filesystem::path& addons_dir,
                                               std::string_view             title_id,
                                               std::span<const std::string> user_addons) {
	AdditionalContentListing listing;
	listing.folder_exists = !addons_dir.empty() && IsDirectory(addons_dir);

	std::vector<AddOnPackage> packages;
	for (auto& entry: Common::FindAddContent(addons_dir, NormalizeTitleId(title_id))) {
		AddOnPackage package;
		package.title_id          = entry.content_id.substr(kTitleIdOffset, kTitleIdLength);
		package.content_id        = std::move(entry.content_id);
		package.entitlement_label = std::move(entry.entitlement_label);
		package.title             = std::move(entry.title);
		package.directory         = std::move(entry.mount_dir);
		packages.push_back(std::move(package));
	}
	listing.items = ListAdditionalContent(packages, title_id);
	for (const auto& entry: user_addons) {
		AdditionalContentItem item;
		item.display_title     = std::string(UserAddOnLabel(entry));
		item.directory         = std::filesystem::path(UserAddOnDirectory(entry));
		item.user_entry        = entry;
		item.missing_directory = !item.directory.empty() && !IsDirectory(item.directory);
		listing.items.push_back(std::move(item));
	}

	listing.status  = !listing.items.empty()   ? AdditionalContentStatus::Listed
	                  : addons_dir.empty()     ? AdditionalContentStatus::FolderNotSet
	                  : !listing.folder_exists ? AdditionalContentStatus::FolderMissing
	                                           : AdditionalContentStatus::NoItems;
	listing.message = AdditionalContentMessage(listing.status, addons_dir, listing.items.size());
	return listing;
}

} // namespace kyty::core
