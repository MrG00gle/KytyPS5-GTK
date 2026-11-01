#include "launcher/gtk/settingsWrite.h"

#include "launcher/gtk/alerts.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"

#include <cerrno>
#include <cstdarg>
#include <filesystem>
#include <system_error>
#include <utility>

namespace kyty::gtk {

namespace {

namespace fs = std::filesystem;

std::string Format(const char* format, ...) G_GNUC_PRINTF(1, 2);

std::string Format(const char* format, ...) {
	va_list arguments;
	va_start(arguments, format);
	const GCharPtr text(g_strdup_vprintf(format, arguments));
	va_end(arguments);
	return std::string(text.View());
}

// The nearest existing ancestor directory entry of file that is not a directory: what makes a
// save fail with ENOTDIR. The parent of file when none is found.
fs::path NotADirectoryAncestor(const fs::path& file) {
	for (fs::path dir = file.parent_path(); !dir.empty(); dir = dir.parent_path()) {
		std::error_code ec;
		const auto      status = fs::status(dir, ec);
		if (!ec && fs::exists(status)) {
			if (!fs::is_directory(status)) {
				return dir;
			}
			break;
		}
		if (dir == dir.parent_path()) {
			break;
		}
	}
	return file.parent_path();
}

std::string NotADirectoryText(const fs::path& file) {
	return Format(_("%s is not a directory, so the settings file cannot be created there."),
	              NotADirectoryAncestor(file).c_str());
}

} // namespace

std::string SettingsRefusalText(const core::SettingsStoreFile& store) {
	const core::IoError& error = *store.load_error;
	if (error.error_number == ENOTDIR) {
		return NotADirectoryText(store.path) + "\n\n" +
		       _("Move or rename it, then choose Overwrite to save the settings.");
	}
	return Format(_("%s exists but could not be loaded:\n%s\n\nOverwrite replaces it with these "
	                "settings."),
	              store.path.c_str(), error.message.c_str());
}

std::string SettingsSaveErrorText(const core::IoError& error) {
	if (error.error_number == ENOTDIR) {
		return NotADirectoryText(error.path);
	}
	return Format(_("The settings file %s was not changed:\n%s"), error.path.c_str(),
	              error.message.c_str());
}

void SaveSettingsInteractively(GtkWindow* parent, LauncherContext& context,
                               core::LauncherSettings next, GCancellable* cancellable,
                               std::function<void()> saved) {
	auto& store   = context.settings;
	auto  written = WriteSettingsStore(context, next);
	if (written) {
		store.loaded.settings = std::move(next);
		if (saved) {
			saved();
		}
		return;
	}
	if (store.load_error && !store.overwrite_confirmed) {
		Confirm(
		    parent, _("The settings file could not be loaded"), SettingsRefusalText(store),
		    _("Overwrite"), _("Cancel"), true,
		    [parent, &context, next, cancellable = GObjectPtr<GCancellable>::Ref(cancellable),
		     saved](bool accepted) {
			    if (!accepted) {
				    return;
			    }
			    core::ConfirmOverwrite(context.settings);
			    SaveSettingsInteractively(parent, context, next, cancellable.get(), saved);
		    },
		    cancellable);
		return;
	}
	g_message("cannot save %s: %s", written.error().path.c_str(), written.error().message.c_str());
	ShowError(parent, _("The settings could not be saved"), SettingsSaveErrorText(written.error()),
	          cancellable);
}

} // namespace kyty::gtk
