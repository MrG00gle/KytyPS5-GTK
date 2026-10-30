#include "launcher/gtk/dialogRegistry.h"

#include "launcher/gtk/alerts.h"
#include "launcher/gtk/i18n.h"

#include <algorithm>
#include <iterator>

namespace kyty::gtk {
namespace {

// The alert types show a representative text: only the UI tests and the screenshot harness open
// them through the registry; the launcher's own confirmations and errors call alerts.h directly.

DialogHandle OpenConfirmationAlert(LauncherContext& /*context*/, GtkWindow* parent,
                                   const DialogArgs& /*args*/) {
	DialogHandle handle;
	handle.cancellable = g_cancellable_new();
	// TRANSLATORS: a sample confirmation that only the UI tests and screenshots show.
	const char* heading = C_("registry sample", "Do you want to continue?");
	const char* body    = C_("registry sample", "The action cannot be undone.");
	const char* accept  = C_("registry sample", "Continue");
	handle.window       = Confirm(
	    parent, heading, body, accept, _("Cancel"), true, [](bool /*accepted*/) {},
	    handle.cancellable);
	return handle;
}

DialogHandle OpenErrorAlert(LauncherContext& /*context*/, GtkWindow* parent,
                            const DialogArgs& /*args*/) {
	DialogHandle handle;
	handle.cancellable = g_cancellable_new();
	// TRANSLATORS: a sample error that only the UI tests and screenshots show.
	const char* heading = C_("registry sample", "The action failed");
	const char* body =
	    C_("registry sample", "The file could not be written: No space left on device.");
	handle.window = ShowError(parent, heading, body, handle.cancellable);
	return handle;
}

void CloseAlert(DialogHandle& handle) {
	if (handle.cancellable != nullptr) {
		g_cancellable_cancel(handle.cancellable);
		g_object_unref(handle.cancellable);
	}
	handle = {};
}

constexpr DialogType kDialogTypes[] = {
    {"confirmation-alert", OpenConfirmationAlert, CloseAlert},
    {"error-alert", OpenErrorAlert, CloseAlert},
};

} // namespace

std::span<const DialogType> DialogTypes() {
	return kDialogTypes;
}

const DialogType* FindDialogType(std::string_view id) {
	const auto* found = std::find_if(std::begin(kDialogTypes), std::end(kDialogTypes),
	                                 [id](const DialogType& type) { return type.id == id; });
	return found != std::end(kDialogTypes) ? found : nullptr;
}

} // namespace kyty::gtk
