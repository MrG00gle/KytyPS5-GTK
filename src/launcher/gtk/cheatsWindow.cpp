#include "launcher/gtk/cheatsWindow.h"

#include "launcher/core/cheatCatalog.h"
#include "launcher/core/cheats.h"
#include "launcher/core/links.h"
#include "launcher/core/remoteCheats.h"
#include "launcher/core/settingsStore.h"
#include "launcher/core/titleId.h"
#include "launcher/gtk/alerts.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/services/desktopLauncher.h"

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kyty::gtk {

struct CheatsWindowState {
	LauncherContext* context = nullptr;
	core::GameEntry  entry;
	std::string      title_id; // NormalizeTitleId(entry.title_id)
	std::string      version;  // the installed version (entry.game_version)
	std::string      process;  // CheatProcessName of the game's effective elf setting
	std::unique_ptr<core::CheatCatalog> catalog;
	std::unique_ptr<core::RemoteCheats> remote;
	// The listed remote files, in row order.
	std::vector<core::RemoteCheatFile> files;
	// The previewed document while it is importable (Import to Local is enabled).
	std::optional<core::CheatDocument> importable;
	// The local cheat file as the Local view read it, and the check box of each of its cheats (the
	// rows of cheats_list own them).
	core::LocalCheatFile         local;
	std::vector<GtkCheckButton*> checks;
	GObjectPtr<GtkSizeGroup>     version_group;
	GObjectPtr<GtkSizeGroup>     format_group;
	bool                         filling  = false; // the file list is rebuilt: no preview
	bool                         disposed = false;
	// Cancelled when the window goes away: no link, alert, or confirmation callback runs after it.
	GObjectPtr<GCancellable> cancellable = GObjectPtr<GCancellable>::Adopt(g_cancellable_new());
	std::vector<SignalScope> signals;
};

} // namespace kyty::gtk

struct _KytyCheatsWindow {
	AdwWindow     parent_instance;
	GtkLabel*     game_label;
	AdwViewStack* stack;
	GtkLabel*     credit_label;
	GtkWidget*    version_header;
	GtkWidget*    format_header;
	GtkStack*     files_stack;
	GtkSpinner*   files_spinner;
	GtkListBox*   files_list;
	GtkListBox*   preview_list;
	GtkLabel*     remote_status;
	GtkLabel*     preview_status;
	GtkWidget*    refresh_button;
	GtkWidget*    import_button;
	GtkStack*     local_stack;
	GtkListBox*   cheats_list;
	GtkLabel*     local_status;
	GtkWidget*    reload_button;
	GtkWidget*    apply_button;
	GtkWidget*    enable_all_button;
	GtkWidget*    disable_all_button;

	kyty::gtk::CheatsWindowState* state;
};

G_DEFINE_FINAL_TYPE(KytyCheatsWindow, kyty_cheats_window, ADW_TYPE_WINDOW)

namespace {

namespace core = kyty::core;
using kyty::gtk::GCharPtr;
using kyty::gtk::GObjectPtr;
using kyty::gtk::SignalScope;

constexpr const char* kTemplateResource = "/io/github/MrG00gle/KytyPS5_GTK/ui/cheats-window.xml";

// How often an import reads the local file again and retries by itself when the file appears or
// changes between the read and the write, before it asks the user to reload.
constexpr int kMaxImportRetries = 3;

std::string Format(const char* format, ...) G_GNUC_PRINTF(1, 2);
std::string Format(const char* format, ...) {
	va_list arguments;
	va_start(arguments, format);
	const GCharPtr text(g_strdup_vprintf(format, arguments));
	va_end(arguments);
	return std::string(text.View());
}

bool Alive(KytyCheatsWindow* self) {
	return self->state != nullptr && !self->state->disposed;
}

std::string Upper(std::string_view text) {
	std::string upper(text);
	std::transform(upper.begin(), upper.end(), upper.begin(),
	               [](char c) { return g_ascii_toupper(c); });
	return upper;
}

void SetSensitive(GtkWidget* widget, bool sensitive) {
	gtk_widget_set_sensitive(widget, sensitive ? TRUE : FALSE);
}

// Moves the keyboard focus to target (or nowhere) when it is inside list, before list's rows are
// removed. Otherwise GTK moves the focus away from the removed widget only after the next frame,
// and when that move lands on a selectable label (the status lines), GTK 4.22's
// gtk_label_grab_focus reads the removed widget after the window dropped its last reference
// (Gtk-CRITICAL gtk_widget_is_ancestor: assertion 'GTK_IS_WIDGET (widget)' failed).
void MoveFocusOutOf(KytyCheatsWindow* self, GtkListBox* list, GtkWidget* target) {
	GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(self));
	if (focus == nullptr ||
	    (focus != GTK_WIDGET(list) && gtk_widget_is_ancestor(focus, GTK_WIDGET(list)) == FALSE)) {
		return;
	}
	if (target == nullptr || gtk_widget_grab_focus(target) == FALSE) {
		gtk_root_set_focus(GTK_ROOT(self), nullptr);
	}
}

// ---- Local view (Requirement 11.6, HC-33)

// The Local view's status line: the launcher's text for each state it can tell apart (D46), with
// the core's dynamic message (the read failure, the parse error that names the file) inside it.
std::string LocalStatusText(const core::LocalCheatFile& local) {
	if (local.path.empty()) {
		return _("Unsupported title ID.");
	}
	if (!local.readable) {
		return Format(_("Could not read the local cheat file %s:\n%s"), local.path.c_str(),
		              local.message.c_str());
	}
	if (!local.bytes) {
		return Format(_("No local cheat file: %s"), local.path.c_str());
	}
	if (!local.document) {
		return Format(_("The local cheat file is not valid:\n%s"), local.message.c_str());
	}
	const auto count = static_cast<unsigned long>(local.document->cheats.size());
	return Format(
	    kyty::gtk::NGettext("Loaded %d cheat from %s.", "Loaded %d cheats from %s.", count),
	    static_cast<int>(count), local.path.c_str());
}

GtkWidget* NewCheatRow(const core::Cheat& cheat, GtkCheckButton** check_out) {
	GtkWidget* row = adw_action_row_new();
	// Cheat names are file data, never markup.
	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), cheat.name.c_str());
	GtkWidget* check = gtk_check_button_new();
	gtk_check_button_set_active(GTK_CHECK_BUTTON(check), cheat.enabled ? TRUE : FALSE);
	gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
	gtk_accessible_update_property(GTK_ACCESSIBLE(check), GTK_ACCESSIBLE_PROPERTY_LABEL,
	                               cheat.name.c_str(), -1);
	adw_action_row_add_prefix(ADW_ACTION_ROW(row), check);
	// Activating the row (a click, Space, or Enter) toggles the check box.
	adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), check);
	*check_out = GTK_CHECK_BUTTON(check);
	return row;
}

void LoadLocal(KytyCheatsWindow* self) {
	auto& state = *self->state;
	state.checks.clear();
	MoveFocusOutOf(self, self->cheats_list, self->reload_button);
	gtk_list_box_remove_all(self->cheats_list);
	state.local       = core::LoadLocalCheats(state.context->paths.data_dir, state.title_id);
	std::size_t count = 0;
	if (state.local.document) {
		for (const auto& cheat: state.local.document->cheats) {
			GtkCheckButton* check = nullptr;
			gtk_list_box_append(self->cheats_list, NewCheatRow(cheat, &check));
			state.checks.push_back(check);
		}
		count = state.local.document->cheats.size();
	}
	gtk_stack_set_visible_child_name(self->local_stack, count > 0 ? "cheats" : "empty");
	SetSensitive(self->apply_button, count > 0);
	SetSensitive(self->enable_all_button, count > 0);
	SetSensitive(self->disable_all_button, count > 0);
	gtk_label_set_text(self->local_status, LocalStatusText(state.local).c_str());
	g_debug("cheats: local file \"%s\": readable %d, %zu cheat(s)", state.local.path.c_str(),
	        state.local.readable ? 1 : 0, count);
}

void OnReload(GtkButton* /*button*/, gpointer data) {
	auto* self = KYTY_CHEATS_WINDOW(data);
	if (Alive(self)) {
		LoadLocal(self);
	}
}

void SetAll(KytyCheatsWindow* self, bool enabled) {
	for (GtkCheckButton* check: self->state->checks) {
		gtk_check_button_set_active(check, enabled ? TRUE : FALSE);
	}
}

void OnEnableAll(GtkButton* /*button*/, gpointer data) {
	auto* self = KYTY_CHEATS_WINDOW(data);
	if (Alive(self)) {
		SetAll(self, true);
	}
}

void OnDisableAll(GtkButton* /*button*/, gpointer data) {
	auto* self = KYTY_CHEATS_WINDOW(data);
	if (Alive(self)) {
		SetAll(self, false);
	}
}

// Apply selection (HC-33): the document as read with the checked states, written only when the
// file still holds the bytes that were read; then the file is read again.
void OnApply(GtkButton* /*button*/, gpointer data) {
	auto* self = KYTY_CHEATS_WINDOW(data);
	if (!Alive(self)) {
		return;
	}
	auto& state = *self->state;
	if (!state.local.document || !state.local.bytes) {
		return;
	}
	core::CheatDocument selection = *state.local.document;
	for (std::size_t i = 0; i < selection.cheats.size() && i < state.checks.size(); i++) {
		selection.cheats[i].enabled = gtk_check_button_get_active(state.checks[i]) != FALSE;
	}
	const auto saved =
	    core::SaveIfUnchanged(state.local.path, selection, std::string_view(*state.local.bytes));
	if (!saved) {
		const auto& error = saved.error();
		g_message("cheats: cannot save the selection to %s: %s", state.local.path.c_str(),
		          error.message.c_str());
		const std::string text =
		    error.kind == core::CheatSaveErrorKind::ChangedOnDisk
		        ? std::string(_("Cheat file changed; reload the dialog."))
		        : Format(_("The cheat selection could not be saved:\n%s"), error.message.c_str());
		gtk_label_set_text(self->local_status, text.c_str());
		return;
	}
	LoadLocal(self);
	gtk_label_set_text(self->local_status,
	                   _("Cheat selection saved. Changes take effect on the next game launch."));
}

// ---- Remote view (Requirements 11.4, 11.5, 11.17; HC-33)

GtkWidget* NewColumnLabel(const std::string& text, GtkSizeGroup* group) {
	GtkWidget* label = gtk_label_new(text.c_str());
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	if (group != nullptr) {
		gtk_size_group_add_widget(group, label);
	}
	return label;
}

// One row of the file list: version ("01.000 (installed)"), format, and file name under the
// column headers; a file that cannot be previewed (MC4, SHN) is disabled.
GtkWidget* NewFileRow(KytyCheatsWindow* self, const core::RemoteCheatFile& file) {
	const auto&       state     = *self->state;
	const bool        supported = core::IsSupportedRemoteCheatFile(file);
	const bool        installed = !state.version.empty() && file.version == state.version;
	const std::string format    = Upper(file.format);
	const std::string version =
	    // TRANSLATORS: a remote cheat file for the installed version of the game: "01.000
	    // (installed)".
	    installed ? Format(_("%s (installed)"), file.version.c_str()) : file.version;
	GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_set_margin_top(box, 6);
	gtk_widget_set_margin_bottom(box, 6);
	gtk_widget_set_margin_start(box, 6);
	gtk_widget_set_margin_end(box, 6);
	gtk_box_append(GTK_BOX(box), NewColumnLabel(version, state.version_group.get()));
	gtk_box_append(GTK_BOX(box), NewColumnLabel(format, state.format_group.get()));
	GtkWidget* name = NewColumnLabel(file.name, nullptr);
	gtk_widget_set_hexpand(name, TRUE);
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
	gtk_box_append(GTK_BOX(box), name);
	GtkWidget* row = gtk_list_box_row_new();
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	// The collection's title of the file is game data (a plain-text tooltip).
	const std::string tooltip =
	    supported ? file.title
	              // TRANSLATORS: the tooltip of a disabled remote cheat file: "<title> — MC4 is
	              // not supported."
	              : Format(_("%s — %s is not supported."), file.title.c_str(), format.c_str());
	gtk_widget_set_tooltip_text(row, tooltip.c_str());
	// TRANSLATORS: the accessible name of a remote cheat file: "<version>, <format>, <file>".
	const std::string accessible = Format(C_("remote cheat file", "%s, %s, %s"), version.c_str(),
	                                      format.c_str(), file.name.c_str());
	gtk_accessible_update_property(GTK_ACCESSIBLE(row), GTK_ACCESSIBLE_PROPERTY_LABEL,
	                               accessible.c_str(), -1);
	if (!supported) {
		gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
		gtk_widget_set_sensitive(row, FALSE);
	}
	return row;
}

// Clears the preview and disables Import to Local; a pending download is cancelled (its result is
// never delivered).
void ClearPreview(KytyCheatsWindow* self) {
	auto& state = *self->state;
	state.remote->Cancel();
	state.importable.reset();
	MoveFocusOutOf(self, self->preview_list, self->refresh_button);
	gtk_list_box_remove_all(self->preview_list);
	SetSensitive(self->import_button, false);
	gtk_label_set_text(self->preview_status, "");
}

void LoadRemote(KytyCheatsWindow* self) {
	auto& state = *self->state;
	// Refresh cancels a pending preview first (phase 5 review u6-4).
	ClearPreview(self);
	MoveFocusOutOf(self, self->files_list, self->refresh_button);
	state.filling = true;
	gtk_list_box_remove_all(self->files_list);
	state.filling = false;
	state.files.clear();
	SetSensitive(self->refresh_button, false);
	gtk_stack_set_visible_child_name(self->files_stack, "loading");
	gtk_spinner_start(self->files_spinner);
	gtk_label_set_text(self->remote_status, _("Loading remote cheats…"));
	state.catalog->Load(state.title_id);
}

void OnRefresh(GtkButton* /*button*/, gpointer data) {
	auto* self = KYTY_CHEATS_WINDOW(data);
	if (Alive(self)) {
		LoadRemote(self);
	}
}

void OnCatalogLoaded(KytyCheatsWindow* self, const core::CheatCatalogResult& result) {
	auto& state = *self->state;
	state.files = result.files;
	core::PutInstalledVersionFirst(state.files, state.version);
	state.filling = true;
	for (const auto& file: state.files) {
		gtk_list_box_append(self->files_list, NewFileRow(self, file));
	}
	state.filling = false;
	gtk_spinner_stop(self->files_spinner);
	gtk_stack_set_visible_child_name(self->files_stack, state.files.empty() ? "empty" : "files");
	const auto  count  = static_cast<unsigned long>(state.files.size());
	std::string status = count == 0 && result.error.empty()
	                         ? Format(_("No remote cheats found for %s."), state.title_id.c_str())
	                         : Format(kyty::gtk::NGettext("%d remote file for %s.",
	                                                      "%d remote files for %s.", count),
	                                  static_cast<int>(count), state.title_id.c_str());
	if (!result.error.empty()) {
		// The core's lines "<FORMAT>: <reason>", one per index that failed (D46: dynamic messages
		// inside the launcher's text).
		g_message("cheats: the remote list of %s is incomplete: %s", state.title_id.c_str(),
		          result.error.c_str());
		status +=
		    "\n" + Format(_("Could not load the complete collection: %s\nUse Refresh to retry."),
		                  result.error.c_str());
	}
	gtk_label_set_text(self->remote_status, status.c_str());
	SetSensitive(self->refresh_button, true);
	g_debug("cheats: %zu remote file(s) for %s", state.files.size(), state.title_id.c_str());
}

void Preview(KytyCheatsWindow* self, GtkListBoxRow* row) {
	auto& state = *self->state;
	ClearPreview(self);
	const int index = row != nullptr ? gtk_list_box_row_get_index(row) : -1;
	if (index < 0 || static_cast<std::size_t>(index) >= state.files.size() ||
	    !core::IsSupportedRemoteCheatFile(state.files[static_cast<std::size_t>(index)])) {
		return;
	}
	gtk_label_set_text(self->preview_status, _("Loading cheat preview…"));
	const auto fetched = state.remote->Fetch(state.files[static_cast<std::size_t>(index)],
	                                         {state.title_id, state.version, state.process});
	if (!fetched) {
		gtk_label_set_text(self->preview_status, _("Only JSON cheat files are supported."));
	}
}

void OnFileSelected(GtkListBox* /*list*/, GtkListBoxRow* row, gpointer data) {
	auto* self = KYTY_CHEATS_WINDOW(data);
	if (Alive(self) && !self->state->filling) {
		Preview(self, row);
	}
}

void OnPreviewLoaded(KytyCheatsWindow* self, const core::RemoteCheatResult& result) {
	auto& state = *self->state;
	if (!result.document) {
		// The download failure or the parse error (it names the file), as the core says it.
		g_message("cheats: cannot preview %s: %s", result.file.name.c_str(), result.error.c_str());
		gtk_label_set_text(
		    self->preview_status,
		    Format(_("Could not preview this file: %s\nSelect another file or refresh "
		             "to retry."),
		           result.error.c_str())
		        .c_str());
		return;
	}
	const auto& document = *result.document;
	for (const auto& cheat: document.cheats) {
		GtkWidget* row = adw_action_row_new();
		adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), cheat.name.c_str());
		gtk_list_box_append(self->preview_list, row);
	}
	std::string credits;
	for (const auto& credit: document.credits) {
		credits += (credits.empty() ? "" : ", ") + credit;
	}
	if (document.credits.empty()) {
		credits = document.author;
	}
	// TRANSLATORS: the previewed cheat file: its name, then "Credits: <names> · Version:
	// <version>".
	std::string text = Format(_("%s\nCredits: %s · Version: %s"), document.name.c_str(),
	                          credits.c_str(), document.version.c_str());
	text += "\n\n";
	if (result.importable) {
		text += _("Import to Local, then select the cheats to enable. Changes take effect on the "
		          "next game launch.");
		state.importable = document;
	} else {
		// Why it does not match the game (Requirement 11.17), as the core says it.
		g_message("cheats: %s cannot be imported: %s", result.file.name.c_str(),
		          result.error.c_str());
		text += Format(_("This file cannot be imported: %s"), result.error.c_str());
	}
	gtk_label_set_text(self->preview_status, text.c_str());
	SetSensitive(self->import_button, result.importable);
}

// ---- Import to Local (Requirement 11.5, HC-33)

void StartImport(KytyCheatsWindow* self, int retries);

// Writes the previewed document with every cheat off over the local file that holds expected (no
// file for nullopt). When the file appeared or changed since it was read, it is read again and the
// import starts over (with the question when there is a file now).
void Import(KytyCheatsWindow* self, const std::optional<std::string>& expected, int retries) {
	auto& state = *self->state;
	if (!state.importable) {
		return;
	}
	const auto imported = core::ImportRemoteCheats(
	    state.context->paths.data_dir, state.title_id, *state.importable,
	    expected ? std::optional<std::string_view>(*expected) : std::nullopt);
	if (!imported) {
		const auto& error = imported.error();
		g_message("cheats: import for %s: %s", state.title_id.c_str(), error.message.c_str());
		if (error.kind == core::CheatSaveErrorKind::NeedsConfirmation ||
		    error.kind == core::CheatSaveErrorKind::ChangedOnDisk) {
			LoadLocal(self);
			if (retries < kMaxImportRetries) {
				StartImport(self, retries + 1);
			} else {
				gtk_label_set_text(self->preview_status,
				                   _("Cheat file changed; reload the dialog."));
			}
			return;
		}
		gtk_label_set_text(
		    self->preview_status,
		    Format(_("The cheats could not be imported:\n%s"), error.message.c_str()).c_str());
		// Import is disabled after a failure (task 21.4) until another file is previewed.
		state.importable.reset();
		SetSensitive(self->import_button, false);
		return;
	}
	const auto count = static_cast<unsigned long>(state.importable->cheats.size());
	g_debug("cheats: imported %lu cheat(s) to %s", count, imported->c_str());
	LoadLocal(self);
	gtk_label_set_text(
	    self->local_status,
	    Format(kyty::gtk::NGettext("Imported %d cheat. Select cheats and apply your "
	                               "selection for the next game launch.",
	                               "Imported %d cheats. Select cheats and apply your "
	                               "selection for the next game launch.",
	                               count),
	           static_cast<int>(count))
	        .c_str());
	adw_view_stack_set_visible_child_name(self->stack, "local");
}

void StartImport(KytyCheatsWindow* self, int retries) {
	auto& state = *self->state;
	if (!state.local.readable) {
		// Qt's rule: never import over a file that could not be read.
		gtk_label_set_text(self->preview_status,
		                   _("Could not read the local cheat file. Open Local "
		                     "and reload it before importing."));
		return;
	}
	if (!state.local.bytes) {
		Import(self, std::nullopt, retries);
		return;
	}
	const std::string body =
	    Format(_("Replace the local cheat file and selections for %s? Imported cheats will start "
	             "unchecked."),
	           state.title_id.c_str());
	kyty::gtk::Confirm(
	    GTK_WINDOW(self), _("Replace local cheats?"), body, _("Replace"), _("Cancel"), true,
	    [self, bytes = *state.local.bytes, retries](bool accepted) {
		    if (accepted && Alive(self)) {
			    Import(self, bytes, retries);
		    }
	    },
	    state.cancellable.get());
}

void OnImport(GtkButton* /*button*/, gpointer data) {
	auto* self = KYTY_CHEATS_WINDOW(data);
	if (Alive(self) && self->state->importable) {
		StartImport(self, 0);
	}
}

// The credit link opens the HEN Cheats Collection repository (Requirement 11.4) through the
// DesktopLauncher, not GTK's default handler.
gboolean OnActivateLink(GtkLabel* /*label*/, const char* /*uri*/, gpointer data) {
	auto* self = KYTY_CHEATS_WINDOW(data);
	if (!Alive(self)) {
		return TRUE;
	}
	auto& state = *self->state;
	state.context->services.desktop_launcher.OpenUri(
	    GTK_WINDOW(self), core::kHenCheatsRepository,
	    [self](const std::string& error) {
		    if (Alive(self) && !error.empty()) {
			    g_message("cheats: cannot open %s: %s", core::kHenCheatsRepository, error.c_str());
			    kyty::gtk::ShowError(GTK_WINDOW(self), _("The link could not be opened"), error,
			                         self->state->cancellable.get());
		    }
	    },
	    state.cancellable.get());
	return TRUE;
}

} // namespace

static void kyty_cheats_window_init(KytyCheatsWindow* self) {
	self->state = new kyty::gtk::CheatsWindowState;
	gtk_widget_init_template(GTK_WIDGET(self));
	auto& state = *self->state;
	state.version_group =
	    GObjectPtr<GtkSizeGroup>::Adopt(gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL));
	state.format_group =
	    GObjectPtr<GtkSizeGroup>::Adopt(gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL));
	gtk_size_group_add_widget(state.version_group.get(), self->version_header);
	gtk_size_group_add_widget(state.format_group.get(), self->format_header);
}

static void kyty_cheats_window_dispose(GObject* object) {
	auto* self = KYTY_CHEATS_WINDOW(object);
	if (auto* state = self->state; state != nullptr && !state->disposed) {
		state->disposed = true;
		g_cancellable_cancel(state->cancellable.get());
		state->signals.clear();
		// Cancels the pending downloads; their results are never delivered.
		state->catalog.reset();
		state->remote.reset();
		state->checks.clear();
	}
	gtk_widget_dispose_template(GTK_WIDGET(self), KYTY_TYPE_CHEATS_WINDOW);
	G_OBJECT_CLASS(kyty_cheats_window_parent_class)->dispose(object);
}

static void kyty_cheats_window_finalize(GObject* object) {
	auto* self = KYTY_CHEATS_WINDOW(object);
	delete self->state;
	self->state = nullptr;
	G_OBJECT_CLASS(kyty_cheats_window_parent_class)->finalize(object);
}

static void kyty_cheats_window_class_init(KytyCheatsWindowClass* klass) {
	auto* object_class     = G_OBJECT_CLASS(klass);
	auto* widget_class     = GTK_WIDGET_CLASS(klass);
	object_class->dispose  = kyty_cheats_window_dispose;
	object_class->finalize = kyty_cheats_window_finalize;
	gtk_widget_class_set_template_from_resource(widget_class, kTemplateResource);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, game_label);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, stack);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, credit_label);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, version_header);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, format_header);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, files_stack);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, files_spinner);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, files_list);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, preview_list);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, remote_status);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, preview_status);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, refresh_button);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, import_button);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, local_stack);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, cheats_list);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, local_status);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, reload_button);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, apply_button);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, enable_all_button);
	gtk_widget_class_bind_template_child(widget_class, KytyCheatsWindow, disable_all_button);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_Escape, GdkModifierType(0),
	                                    "window.close", nullptr);
}

KytyCheatsWindow* kyty_cheats_window_new(kyty::gtk::LauncherContext& context, GtkWindow* parent,
                                         const kyty::core::GameEntry& entry) {
	auto* self    = KYTY_CHEATS_WINDOW(g_object_new(KYTY_TYPE_CHEATS_WINDOW, nullptr));
	auto& state   = *self->state;
	state.context = &context;
	state.entry   = entry;
	if (parent != nullptr) {
		gtk_window_set_transient_for(GTK_WINDOW(self), parent);
	}
	state.title_id = core::NormalizeTitleId(entry.title_id);
	state.version  = entry.game_version;
	state.process  = core::CheatProcessName(
	    core::ResolveEffectiveConfiguration(context.settings.loaded.settings, entry, std::nullopt)
	        .configuration.elf);

	const std::string name = entry.title_name.empty() ? state.title_id : entry.title_name;
	const std::string title =
	    // TRANSLATORS: the cheats window's title; %s is the game's name.
	    Format(_("Cheats (experimental) — %s"), name.c_str());
	gtk_window_set_title(GTK_WINDOW(self), title.c_str());
	const std::string game =
	    // TRANSLATORS: the game of the cheats window: "<title ID> · Installed version: <version>".
	    Format(_("%s · Installed version: %s"), state.title_id.c_str(),
	           state.version.empty() ? C_("game version", "Unknown") : state.version.c_str());
	gtk_label_set_text(self->game_label, game.c_str());
	const GCharPtr credit(g_markup_printf_escaped(
	    // TRANSLATORS: Pango markup; keep the <a href="%s"> element (%s is the link's address).
	    _("Cheats by <a href=\"%s\">TeeKay87 / HEN Cheats Collection</a> and its contributors."),
	    core::kHenCheatsRepository));
	gtk_label_set_markup(self->credit_label, credit ? credit.get() : "");

	state.catalog = std::make_unique<core::CheatCatalog>(
	    context.services.http, [self](const core::CheatCatalogResult& result) {
		    if (Alive(self)) {
			    OnCatalogLoaded(self, result);
		    }
	    });
	state.remote = std::make_unique<core::RemoteCheats>(
	    context.services.http, [self](const core::RemoteCheatResult& result) {
		    if (Alive(self)) {
			    OnPreviewLoaded(self, result);
		    }
	    });

	const auto connect = [&state, self](gpointer instance, const char* signal, GCallback callback) {
		state.signals.push_back(SignalScope::Connect(instance, signal, callback, self));
	};
	connect(self->credit_label, "activate-link", G_CALLBACK(&OnActivateLink));
	connect(self->files_list, "row-selected", G_CALLBACK(&OnFileSelected));
	connect(self->refresh_button, "clicked", G_CALLBACK(&OnRefresh));
	connect(self->import_button, "clicked", G_CALLBACK(&OnImport));
	connect(self->reload_button, "clicked", G_CALLBACK(&OnReload));
	connect(self->apply_button, "clicked", G_CALLBACK(&OnApply));
	connect(self->enable_all_button, "clicked", G_CALLBACK(&OnEnableAll));
	connect(self->disable_all_button, "clicked", G_CALLBACK(&OnDisableAll));

	LoadLocal(self);
	LoadRemote(self);
	return self;
}
