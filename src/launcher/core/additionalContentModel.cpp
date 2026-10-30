#include "launcher/core/additionalContentModel.h"

#include "common/additionalContent.h"
#include "launcher/core/titleId.h"

#include <system_error>
#include <utility>

namespace kyty::core {

namespace {

// Common::FindAddContent accepts only content IDs of this shape, so the title ID and the
// entitlement label are at fixed offsets.
constexpr std::size_t kTitleIdOffset = 7;
constexpr std::size_t kTitleIdLength = 9;

} // namespace

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
		case AdditionalContentStatus::Listed: return std::to_string(count) + " installed add-on(s)";
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
                                               std::string_view             title_id) {
	AdditionalContentListing listing;
	std::error_code          ec;
	listing.folder_exists = !addons_dir.empty() && std::filesystem::is_directory(addons_dir, ec);

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

	listing.status  = addons_dir.empty()       ? AdditionalContentStatus::FolderNotSet
	                  : !listing.folder_exists ? AdditionalContentStatus::FolderMissing
	                  : listing.items.empty()  ? AdditionalContentStatus::NoItems
	                                           : AdditionalContentStatus::Listed;
	listing.message = AdditionalContentMessage(listing.status, addons_dir, listing.items.size());
	return listing;
}

} // namespace kyty::core
