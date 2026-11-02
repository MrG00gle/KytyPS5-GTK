#include "launcher/gtk/gameActions.h"

#include "launcher/core/gameSettingsFile.h"
#include "launcher/core/settingsStore.h"
#include "launcher/core/titleId.h"
#include "launcher/gtk/alerts.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/services/desktopLauncher.h"
#include "launcher/gtk/services/fileChooser.h"
#include "launcher/gtk/settingsWindow.h"
#include "launcher/gtk/settingsWrite.h"

#include <cstdarg>
#include <optional>
#include <string>
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

// The game in the texts: its title, else its title ID (as the Qt launcher).
std::string GameName(const core::GameEntry& entry) {
	return entry.title_name.empty() ? core::NormalizeTitleId(entry.title_id) : entry.title_name;
}

// The game with its title ID, when it has one that differs from the title.
std::string GameLabel(const core::GameEntry& entry) {
	const std::string title_id = core::NormalizeTitleId(entry.title_id);
	const std::string name     = GameName(entry);
	if (title_id.empty() || name == title_id) {
		return name;
	}
	// TRANSLATORS: a name and, in parentheses, its key or title ID.
	return Format(_("%s (%s)"), name.c_str(), title_id.c_str());
}

// ---- Open game folder (10.10, LFI-31)

void ShowFolderError(GtkWindow* parent, const fs::path& folder, const std::string& error,
                     GCancellable* cancellable) {
	g_message("cannot open %s: %s", folder.c_str(), error.c_str());
	ShowError(parent, _("The game folder could not be opened"),
	          Format(_("%s could not be opened:\n%s"), folder.c_str(), error.c_str()), cancellable);
}

void OpenFolder(DesktopLauncher& desktop, GtkWindow* parent, const fs::path& folder,
                GCancellable* cancellable) {
	desktop.OpenFolder(
	    parent, folder,
	    [parent, folder,
	     keep = GObjectPtr<GCancellable>::Ref(cancellable)](const std::string& error) {
		    if (!error.empty()) {
			    ShowFolderError(parent, folder, error, keep.get());
		    }
	    },
	    cancellable);
}

// ---- Remove save data (11.13, LFI-30, HC-32)

std::string Lines(const std::vector<fs::path>& paths) {
	std::string text;
	for (const auto& path: paths) {
		text += (text.empty() ? "" : "\n") + path.string();
	}
	return text;
}

void RemoveConfirmed(LauncherContext& context, GtkWindow* parent, const core::GameEntry& entry,
                     GCancellable* cancellable) {
	// 11.18: the game may not run while its save data goes.
	if (!context.run_state.CanRemoveSaveData(entry.path)) {
		ShowError(parent, _("The save data cannot be removed now"),
		          _("Close the running game first."), cancellable);
		return;
	}
	auto removal =
	    core::RemoveSaveData(context.paths.data_dir, entry.title_id, *context.path_remover);
	if (!removal) {
		g_message("cannot remove the save data of %s: %s", entry.path.c_str(),
		          removal.error().c_str());
		ShowError(parent, _("Nothing to remove"), _("No save data folder found for this game."),
		          cancellable);
		return;
	}
	if (!removal->failures.empty()) {
		g_message("save data of %s: %zu path(s) could not be removed", entry.path.c_str(),
		          removal->failures.size());
		ShowSaveDataFailures(parent, entry, removal->failures, cancellable);
		return;
	}
	g_debug("save data removed: %s", removal->directory.c_str());
}

// ---- Import settings (9.9, 9.10, HC-19)

const char* ImportHeading() {
	return _("The game config could not be imported");
}

void ApplyImport(LauncherContext& context, GtkWindow* parent, const core::GameEntry& entry,
                 const core::GameImport& selected, GCancellable* cancellable,
                 const std::function<void()>& changed) {
	core::LauncherSettings next = context.settings.loaded.settings;
	// Fields the file does not have keep the game's current values (9.9).
	const auto configuration =
	    core::ApplyPatch(core::EnableCustomSettings(next, entry), selected.patch);
	if (auto saved = core::SaveCustomSettings(next, entry, configuration); !saved) {
		ShowError(parent, ImportHeading(),
		          Format(_("The game config of %s does not fit %s. %s"), selected.title.c_str(),
		                 GameName(entry).c_str(), SettingsFieldErrorText(saved.error()).c_str()),
		          cancellable);
		return;
	}
	g_debug("import game config: %zu field(s) of %s for %s", selected.patch.fields.size(),
	        selected.title.c_str(), entry.path.c_str());
	SaveSettingsInteractively(parent, context, std::move(next), cancellable, changed);
}

void ImportFile(LauncherContext& context, GtkWindow* parent, const core::GameEntry& entry,
                const fs::path& file, GCancellable* cancellable,
                const std::function<void()>& changed) {
	auto result = core::ReadGameSettingsFile(file);
	if (!result) {
		ShowError(parent, ImportHeading(), GameSettingsImportErrorText(result.error()),
		          cancellable);
		return;
	}
	auto selected = core::SelectGameImport(*result, entry.title_id);
	if (!selected) {
		ShowError(parent, ImportHeading(), GameSettingsImportErrorText(selected.error()),
		          cancellable);
		return;
	}
	if (!selected->needs_confirmation) {
		ApplyImport(context, parent, entry, *selected, cancellable, changed);
		return;
	}
	ConfirmForeignGameConfig(
	    parent, entry, selected->title,
	    [&context, parent, entry, chosen = *selected,
	     keep = GObjectPtr<GCancellable>::Ref(cancellable), changed](bool accepted) {
		    if (accepted) {
			    ApplyImport(context, parent, entry, chosen, keep.get(), changed);
		    }
	    },
	    cancellable);
}

} // namespace

bool CanExportGameConfig(const core::GameEntry& entry) {
	return !core::NormalizeTitleId(entry.title_id).empty();
}

void OpenGameFolder(LauncherContext& context, GtkWindow* parent, const core::GameEntry& entry,
                    GCancellable* cancellable) {
	const bool      archive = entry.kind == core::GameEntry::Kind::Archive;
	const fs::path  folder  = archive ? entry.path.parent_path() : entry.path;
	std::error_code ec;
	if (!fs::is_directory(folder, ec)) {
		ShowError(parent, _("The game folder could not be opened"),
		          Format(_("The folder %s does not exist."), folder.c_str()), cancellable);
		return;
	}
	auto& desktop = context.services.desktop_launcher;
	if (!archive) {
		OpenFolder(desktop, parent, folder, cancellable);
		return;
	}
	// The archive selected in its folder; a desktop that cannot select files gets the folder.
	desktop.RevealFile(
	    parent, entry.path,
	    [&desktop, parent, path = entry.path, folder,
	     keep = GObjectPtr<GCancellable>::Ref(cancellable)](const std::string& error) {
		    if (!error.empty()) {
			    g_message("cannot show %s in its folder: %s", path.c_str(), error.c_str());
			    OpenFolder(desktop, parent, folder, keep.get());
		    }
	    },
	    cancellable);
}

void ClearGameConfig(LauncherContext& context, GtkWindow* parent, const core::GameEntry& entry,
                     GCancellable* cancellable, std::function<void()> changed) {
	if (!core::HasCustomSettings(context.settings.loaded.settings, entry)) {
		return;
	}
	ConfirmClearGameConfig(
	    parent, entry,
	    [&context, parent, entry, keep = GObjectPtr<GCancellable>::Ref(cancellable),
	     changed = std::move(changed)](bool accepted) {
		    if (!accepted) {
			    return;
		    }
		    core::LauncherSettings next = context.settings.loaded.settings;
		    if (!core::ClearCustomSettings(next, entry)) {
			    return;
		    }
		    g_debug("clear game config: %s", entry.path.c_str());
		    SaveSettingsInteractively(parent, context, std::move(next), keep.get(), changed);
	    },
	    cancellable);
}

void RemoveGameSaveData(LauncherContext& context, GtkWindow* parent, const core::GameEntry& entry,
                        GCancellable* cancellable) {
	if (!context.run_state.CanRemoveSaveData(entry.path)) {
		ShowError(parent, _("The save data cannot be removed now"),
		          _("Close the running game first."), cancellable);
		return;
	}
	auto directories = core::ListSaveData(context.paths.data_dir, entry.title_id);
	if (!directories) {
		g_message("no save data for %s: %s", entry.path.c_str(), directories.error().c_str());
	}
	if (!directories || directories->empty()) {
		ShowError(parent, _("Nothing to remove"), _("No save data folder found for this game."),
		          cancellable);
		return;
	}
	ConfirmRemoveSaveData(
	    parent, entry, *directories,
	    [&context, parent, entry,
	     keep = GObjectPtr<GCancellable>::Ref(cancellable)](bool accepted) {
		    if (accepted) {
			    RemoveConfirmed(context, parent, entry, keep.get());
		    }
	    },
	    cancellable);
}

void ExportGameConfig(LauncherContext& context, GtkWindow* parent, const core::GameEntry& entry,
                      GCancellable* cancellable) {
	const std::string title_id = core::NormalizeTitleId(entry.title_id);
	if (title_id.empty()) {
		return;
	}
	context.services.file_chooser.SaveFile(
	    parent, _("Export game config"), title_id + ".json",
	    [&context, parent, entry, keep = GObjectPtr<GCancellable>::Ref(cancellable)](
	        std::optional<fs::path> file, const std::string& error) {
		    if (!error.empty()) {
			    ShowError(parent, _("The file could not be chosen"), error, keep.get());
			    return;
		    }
		    if (!file) {
			    return;
		    }
		    // The configuration the game runs with: its custom settings, or the global ones.
		    const auto configuration =
		        core::EnableCustomSettings(context.settings.loaded.settings, entry);
		    if (auto written = core::WriteGameSettingsFile(*file, entry.title_id, configuration);
		        !written) {
			    ShowError(parent, _("The game config could not be exported"),
			              Format(_("%s could not be written:\n%s"), file->c_str(),
			                     written.error().message.c_str()),
			              keep.get());
			    return;
		    }
		    g_debug("export game config: %s to %s", entry.path.c_str(), file->c_str());
	    },
	    cancellable);
}

void ImportGameConfig(LauncherContext& context, GtkWindow* parent, const core::GameEntry& entry,
                      GCancellable* cancellable, std::function<void()> changed) {
	context.services.file_chooser.OpenFile(
	    parent, _("Import game config"), {{_("JSON files"), {"*.json"}}},
	    [&context, parent, entry, keep = GObjectPtr<GCancellable>::Ref(cancellable),
	     changed = std::move(changed)](std::optional<fs::path> file, const std::string& error) {
		    if (!error.empty()) {
			    ShowError(parent, _("The file could not be chosen"), error, keep.get());
			    return;
		    }
		    if (file) {
			    ImportFile(context, parent, entry, *file, keep.get(), changed);
		    }
	    },
	    cancellable);
}

GtkWindow* ConfirmClearGameConfig(GtkWindow* parent, const core::GameEntry& entry,
                                  std::function<void(bool accepted)> done,
                                  GCancellable*                      cancellable) {
	return Confirm(parent, _("Clear the game config?"),
	               Format(_("%s will use the global settings. Its own settings are removed."),
	                      GameLabel(entry).c_str()),
	               _("Clear game config"), _("Cancel"), true, std::move(done), cancellable);
}

GtkWindow* ConfirmRemoveSaveData(GtkWindow* parent, const core::GameEntry& entry,
                                 const std::vector<std::filesystem::path>& directories,
                                 std::function<void(bool accepted)>        done,
                                 GCancellable*                             cancellable) {
	return Confirm(
	    parent, Format(_("Remove the save data of %s?"), GameName(entry).c_str()),
	    // TRANSLATORS: %s is the list of directories, one per line.
	    Format(_("This deletes:\n%s\n\nThis cannot be undone."), Lines(directories).c_str()),
	    _("Remove save data"), _("Cancel"), true, std::move(done), cancellable);
}

GtkWindow* ShowSaveDataFailures(GtkWindow* parent, const core::GameEntry& entry,
                                const std::vector<core::RemovalFailure>& failures,
                                GCancellable*                            cancellable) {
	std::string lines;
	for (const auto& failure: failures) {
		// TRANSLATORS: a path that could not be deleted, then the reason.
		lines += (lines.empty() ? "" : "\n") +
		         Format(_("%s: %s"), failure.path.c_str(), failure.error.message().c_str());
	}
	return ShowError(
	    parent, Format(_("Some save data of %s could not be removed"), GameName(entry).c_str()),
	    Format(_("Could not remove:\n%s"), lines.c_str()), cancellable);
}

GtkWindow* ConfirmForeignGameConfig(GtkWindow* parent, const core::GameEntry& entry,
                                    std::string_view                   member_title,
                                    std::function<void(bool accepted)> done,
                                    GCancellable*                      cancellable) {
	const std::string member(member_title);
	return Confirm(parent, _("Import the config of another game?"),
	               Format(_("The file holds the game config of %s. Apply it to %s?"),
	                      member.c_str(), GameLabel(entry).c_str()),
	               _("Import"), _("Cancel"), false, std::move(done), cancellable);
}

} // namespace kyty::gtk
