#include "launcher/gtk/compressionDialog.h"

#include "launcher/core/cancelToken.h"
#include "launcher/gtk/alerts.h"
#include "launcher/gtk/backgroundTask.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"

#include <cstdarg>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace kyty::gtk {

struct CompressionDialogState {
	core::GameEntry                    entry;
	std::shared_ptr<core::ArchiveJob>  job; // after Prepare succeeded
	std::shared_ptr<core::CancelToken> cancel     = std::make_shared<core::CancelToken>();
	bool                               running    = false;
	bool                               cancelling = false;
	bool                               disposed   = false;
	// False from dispose on: progress and the job's result are dropped.
	std::shared_ptr<bool> alive = std::make_shared<bool>(true);
	// Cancelled when the dialog goes away: the delete question closes without calling back.
	GObjectPtr<GCancellable> cancellable = GObjectPtr<GCancellable>::Adopt(g_cancellable_new());
	std::vector<SignalScope> signals;
};

} // namespace kyty::gtk

struct _KytyCompressionDialog {
	AdwWindow       parent_instance;
	GtkStack*       content_stack;
	GtkLabel*       progress_label;
	GtkProgressBar* progress_bar;
	AdwStatusPage*  warning_page;
	AdwStatusPage*  failed_page;
	AdwStatusPage*  completed_page;
	GtkWidget*      cancel_button;
	GtkWidget*      close_button;

	kyty::gtk::CompressionDialogState* state;
};

G_DEFINE_FINAL_TYPE(KytyCompressionDialog, kyty_compression_dialog, ADW_TYPE_WINDOW)

namespace {

namespace core = kyty::core;
namespace fs   = std::filesystem;
using kyty::gtk::GCharPtr;

constexpr const char* kTemplateResource =
    "/io/github/MrG00gle/KytyPS5_GTK/ui/compression-dialog.xml";

guint g_archive_created_signal = 0;

std::string Format(const char* format, ...) G_GNUC_PRINTF(1, 2);
std::string Format(const char* format, ...) {
	va_list arguments;
	va_start(arguments, format);
	const GCharPtr text(g_strdup_vprintf(format, arguments));
	va_end(arguments);
	return std::string(text.View());
}

bool Alive(KytyCompressionDialog* self) {
	return self->state != nullptr && !self->state->disposed;
}

// A status page's title and description (the description is markup, so it is escaped).
void SetPage(AdwStatusPage* page, const std::string& title, const std::string& description) {
	adw_status_page_set_title(page, title.c_str());
	const GCharPtr markup(g_markup_escape_text(description.c_str(), -1));
	adw_status_page_set_description(page, description.empty() ? nullptr : markup.get());
}

// The progress page and Cancel while the job runs; Close otherwise.
void ShowPage(KytyCompressionDialog* self, const char* name) {
	const bool progress = g_strcmp0(name, "progress") == 0;
	gtk_stack_set_visible_child_name(self->content_stack, name);
	gtk_widget_set_visible(self->cancel_button, progress ? TRUE : FALSE);
	gtk_widget_set_visible(self->close_button, progress ? FALSE : TRUE);
	if (!progress) {
		gtk_widget_grab_focus(self->close_button);
	}
}

void ShowProgress(KytyCompressionDialog* self, int percent) {
	gtk_progress_bar_set_fraction(self->progress_bar, percent / 100.0);
	const std::string text =
	    // TRANSLATORS: the compression progress, a percentage.
	    Format(C_("compression progress", "%d%%"), percent);
	gtk_progress_bar_set_text(self->progress_bar, text.c_str());
}

// One percentage from the job's thread, applied on the main thread.
struct ProgressUpdate {
	KytyCompressionDialog* self = nullptr;
	std::shared_ptr<bool>  alive;
	int                    percent = 0;
};

gboolean ApplyProgress(gpointer data) {
	const auto* update = static_cast<const ProgressUpdate*>(data);
	if (*update->alive && update->self->state->running) {
		ShowProgress(update->self, update->percent);
	}
	return G_SOURCE_REMOVE;
}

void RequestCancel(KytyCompressionDialog* self) {
	auto& state = *self->state;
	if (!state.running || state.cancelling) {
		return;
	}
	state.cancelling = true;
	state.cancel->Cancel();
	gtk_widget_set_sensitive(self->cancel_button, FALSE);
	gtk_label_set_text(self->progress_label, _("Cancelling…"));
	g_debug("compression of %s: cancel requested", state.entry.path.c_str());
}

// Requirement 11.11, after the user answered the delete question: the original directory is
// removed only on "Delete folder"; then the caller rescans.
void OnDeleteAnswer(KytyCompressionDialog* self, bool remove) {
	auto&           state  = *self->state;
	const fs::path& source = state.job->Source();
	std::error_code ec;
	if (remove) {
		fs::remove_all(source, ec);
		g_debug("compression: removing %s: %s", source.c_str(), ec ? ec.message().c_str() : "done");
	}
	g_signal_emit(self, g_archive_created_signal, 0);
	if (!ec) {
		gtk_window_destroy(GTK_WINDOW(self));
		return;
	}
	SetPage(self->failed_page, _("The original folder could not be deleted"),
	        // TRANSLATORS: the original game folder, then the system's reason.
	        Format(_("Could not remove %s: %s"), source.c_str(), ec.message().c_str()));
	ShowPage(self, "failed");
}

void OnCompleted(KytyCompressionDialog* self) {
	auto&       state = *self->state;
	const auto& job   = *state.job;
	ShowProgress(self, 100);
	// TRANSLATORS: %s is the archive that was written.
	const std::string created = Format(_("Created %s."), job.Archive().c_str());
	SetPage(self->completed_page, _("Compression finished"), created);
	ShowPage(self, "completed");
	// Keep is the default and the Escape button (alerts.h: the cancel button).
	kyty::gtk::Confirm(
	    GTK_WINDOW(self), _("Delete the original folder?"),
	    // TRANSLATORS: the archive, then the original game folder.
	    Format(_("Created %s.\n\nDelete the original folder %s?"), job.Archive().c_str(),
	           job.Source().c_str()),
	    _("Delete folder"), _("Keep folder"), true,
	    [self](bool remove) {
		    if (Alive(self)) {
			    OnDeleteAnswer(self, remove);
		    }
	    },
	    state.cancellable.get());
}

void OnFinished(KytyCompressionDialog* self) {
	auto&       state = *self->state;
	const auto& job   = *state.job;
	state.running     = false;
	g_debug("compression of %s: outcome %d", job.Source().c_str(), static_cast<int>(job.Outcome()));
	switch (job.Outcome()) {
		case core::ArchiveOutcome::Completed: OnCompleted(self); return;
		case core::ArchiveOutcome::Cancelled:
			// 11.10: the partial archive is gone and no message is shown.
			gtk_window_destroy(GTK_WINDOW(self));
			return;
		case core::ArchiveOutcome::Failed:
		case core::ArchiveOutcome::VerifyFailed: {
			// HC-31: both name the source directory; Failed adds the job's error when it says more
			// than that (a reason, or the archive that could not be removed).
			std::string message =
			    // TRANSLATORS: %s is the game folder that could not be compressed.
			    Format(_("Could not compress %s."), job.Source().c_str());
			const bool plain = job.Error() == "Could not compress " + job.Source().string() + ".";
			if (job.Outcome() == core::ArchiveOutcome::Failed && !plain) {
				message += "\n\n" + job.Error();
			}
			g_message("compression of %s failed: %s", job.Source().c_str(),
			          job.Error().empty() ? "the archive cannot be opened" : job.Error().c_str());
			SetPage(self->failed_page, _("Compression failed"), message);
			ShowPage(self, "failed");
			return;
		}
		case core::ArchiveOutcome::Pending: break;
	}
	ShowPage(self, "failed");
}

void Start(KytyCompressionDialog* self) {
	auto& state   = *self->state;
	state.running = true;
	const std::string compressing =
	    // TRANSLATORS: %s is the game folder being compressed.
	    Format(_("Compressing %s…"), state.entry.path.c_str());
	gtk_label_set_text(self->progress_label, compressing.c_str());
	ShowProgress(self, 0);
	ShowPage(self, "progress");
	g_debug("compression of %s into %s started", state.job->Source().c_str(),
	        state.job->Archive().c_str());
	auto job      = state.job;
	auto cancel   = state.cancel;
	auto progress = [self, alive = state.alive](int percent) {
		// From the job's thread: an idle source of the default main context runs it on the main
		// thread, in order.
		g_idle_add_full(G_PRIORITY_DEFAULT, &ApplyProgress,
		                new ProgressUpdate {self, alive, percent},
		                [](gpointer data) { delete static_cast<ProgressUpdate*>(data); });
	};
	kyty::gtk::RunInBackground([job, cancel, progress]() { job->Run(progress, *cancel); },
	                           [self, alive = state.alive]() {
		                           if (*alive) {
			                           OnFinished(self);
		                           }
	                           });
}

gboolean OnCloseRequest(GtkWindow* window, gpointer /*data*/) {
	auto* self = KYTY_COMPRESSION_DIALOG(window);
	if (Alive(self) && self->state->running) {
		// Closing is Cancel; the dialog closes once the job has stopped.
		RequestCancel(self);
		return TRUE;
	}
	return FALSE;
}

void OnCancel(GtkButton* /*button*/, gpointer data) {
	auto* self = KYTY_COMPRESSION_DIALOG(data);
	if (Alive(self)) {
		RequestCancel(self);
	}
}

} // namespace

static void kyty_compression_dialog_init(KytyCompressionDialog* self) {
	self->state = new kyty::gtk::CompressionDialogState;
	gtk_widget_init_template(GTK_WIDGET(self));
}

static void kyty_compression_dialog_dispose(GObject* object) {
	auto* self = KYTY_COMPRESSION_DIALOG(object);
	if (auto* state = self->state; state != nullptr && !state->disposed) {
		state->disposed = true;
		*state->alive   = false;
		// A job that still runs stops at its next progress call and deletes its partial archive.
		state->cancel->Cancel();
		g_cancellable_cancel(state->cancellable.get());
		state->signals.clear();
	}
	gtk_widget_dispose_template(GTK_WIDGET(self), KYTY_TYPE_COMPRESSION_DIALOG);
	G_OBJECT_CLASS(kyty_compression_dialog_parent_class)->dispose(object);
}

static void kyty_compression_dialog_finalize(GObject* object) {
	auto* self = KYTY_COMPRESSION_DIALOG(object);
	delete self->state;
	self->state = nullptr;
	G_OBJECT_CLASS(kyty_compression_dialog_parent_class)->finalize(object);
}

static void kyty_compression_dialog_class_init(KytyCompressionDialogClass* klass) {
	auto* object_class     = G_OBJECT_CLASS(klass);
	auto* widget_class     = GTK_WIDGET_CLASS(klass);
	object_class->dispose  = kyty_compression_dialog_dispose;
	object_class->finalize = kyty_compression_dialog_finalize;
	g_archive_created_signal =
	    g_signal_new("archive-created", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr,
	                 nullptr, nullptr, G_TYPE_NONE, 0);
	gtk_widget_class_set_template_from_resource(widget_class, kTemplateResource);
	gtk_widget_class_bind_template_child(widget_class, KytyCompressionDialog, content_stack);
	gtk_widget_class_bind_template_child(widget_class, KytyCompressionDialog, progress_label);
	gtk_widget_class_bind_template_child(widget_class, KytyCompressionDialog, progress_bar);
	gtk_widget_class_bind_template_child(widget_class, KytyCompressionDialog, warning_page);
	gtk_widget_class_bind_template_child(widget_class, KytyCompressionDialog, failed_page);
	gtk_widget_class_bind_template_child(widget_class, KytyCompressionDialog, completed_page);
	gtk_widget_class_bind_template_child(widget_class, KytyCompressionDialog, cancel_button);
	gtk_widget_class_bind_template_child(widget_class, KytyCompressionDialog, close_button);
	// Escape: Cancel while the job runs (close-request), else Close.
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_Escape, GdkModifierType(0),
	                                    "window.close", nullptr);
}

KytyCompressionDialog* kyty_compression_dialog_new(GtkWindow*                   parent,
                                                   const kyty::core::GameEntry& entry,
                                                   kyty::core::ArchivePacker    packer) {
	auto* self  = KYTY_COMPRESSION_DIALOG(g_object_new(KYTY_TYPE_COMPRESSION_DIALOG, nullptr));
	auto& state = *self->state;
	state.entry = entry;
	if (parent != nullptr) {
		gtk_window_set_transient_for(GTK_WINDOW(self), parent);
	}
	state.signals.push_back(kyty::gtk::SignalScope::Connect(self->cancel_button, "clicked",
	                                                        G_CALLBACK(&OnCancel), self));
	state.signals.push_back(kyty::gtk::SignalScope::Connect(self, "close-request",
	                                                        G_CALLBACK(&OnCloseRequest), nullptr));

	auto prepared = core::ArchiveJob::Prepare(entry.path, std::move(packer));
	if (!prepared) {
		// 11.9: nothing is started and the existing file stays as it is.
		const fs::path  archive = core::ArchivePathFor(entry.path);
		std::error_code ec;
		const auto      type = fs::symlink_status(archive, ec).type();
		g_message("compression of %s: %s", entry.path.c_str(), prepared.error().c_str());
		if (type != fs::file_type::not_found && type != fs::file_type::none) {
			// TRANSLATORS: %s is the archive path.
			const std::string exists = Format(_("%s already exists."), archive.c_str());
			SetPage(self->warning_page, _("The archive already exists"), exists);
		} else {
			// The core's message names the archive and the reason.
			SetPage(self->warning_page, _("The archive cannot be created"), prepared.error());
		}
		ShowPage(self, "warning");
		return self;
	}
	state.job = std::make_shared<core::ArchiveJob>(std::move(*prepared));
	Start(self);
	return self;
}

kyty::core::ArchiveOutcome kyty_compression_dialog_get_outcome(KytyCompressionDialog* dialog) {
	g_return_val_if_fail(KYTY_IS_COMPRESSION_DIALOG(dialog), kyty::core::ArchiveOutcome::Pending);
	const auto* state = dialog->state;
	// The job's thread writes the outcome until the dialog has seen the job finish.
	return state != nullptr && state->job && !state->running ? state->job->Outcome()
	                                                         : kyty::core::ArchiveOutcome::Pending;
}
