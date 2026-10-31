#include "launcher/gtk/services/fileChooser.h"

#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"

#include <utility>

namespace kyty::gtk {

namespace {

// The user closed the dialog, or the caller's cancellable was cancelled.
bool IsDismissed(const GErrorPtr& error) {
	return g_error_matches(error.get(), GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) ||
	       g_error_matches(error.get(), GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) ||
	       g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

std::string NotLocal(GFile* file) {
	const GCharPtr uri(g_file_get_uri(file));
	const GCharPtr text(g_strdup_printf(_("%s is not a local file."), uri.get()));
	return std::string(text.View());
}

// One open dialog: the callback, and what decides whether it may still run.
template <typename Fn>
struct Pending {
	std::shared_ptr<bool>    alive;
	Fn                       done;
	GObjectPtr<GCancellable> cancellable;

	[[nodiscard]] bool MayCall() const {
		return *alive && (!cancellable || g_cancellable_is_cancelled(cancellable.get()) == FALSE);
	}
};

class GtkFileChooser final: public FileChooser {
public:
	~GtkFileChooser() override { *m_alive = false; }

	void SelectFolders(GtkWindow* parent, const std::string& title, FoldersFn done,
	                   GCancellable* cancellable) override {
		auto dialog = NewDialog(title);
		gtk_file_dialog_select_multiple_folders(
		    dialog.get(), parent, cancellable, &OnFolders,
		    new Pending<FoldersFn> {m_alive, std::move(done),
		                            GObjectPtr<GCancellable>::Ref(cancellable)});
	}

	void OpenFile(GtkWindow* parent, const std::string& title,
	              const std::vector<FileFilter>& filters, FileFn done,
	              GCancellable* cancellable) override {
		auto dialog = NewDialog(title);
		if (!filters.empty()) {
			auto store = GObjectPtr<GListStore>::Adopt(g_list_store_new(GTK_TYPE_FILE_FILTER));
			for (const auto& filter: filters) {
				auto item = GObjectPtr<GtkFileFilter>::Adopt(gtk_file_filter_new());
				gtk_file_filter_set_name(item.get(), filter.name.c_str());
				for (const auto& pattern: filter.patterns) {
					gtk_file_filter_add_pattern(item.get(), pattern.c_str());
				}
				g_list_store_append(store.get(), item.get());
			}
			gtk_file_dialog_set_filters(dialog.get(), G_LIST_MODEL(store.get()));
		}
		gtk_file_dialog_open(dialog.get(), parent, cancellable, &OnOpen,
		                     new Pending<FileFn> {m_alive, std::move(done),
		                                          GObjectPtr<GCancellable>::Ref(cancellable)});
	}

	void SaveFile(GtkWindow* parent, const std::string& title, const std::string& suggested_name,
	              FileFn done, GCancellable* cancellable) override {
		auto dialog = NewDialog(title);
		if (!suggested_name.empty()) {
			gtk_file_dialog_set_initial_name(dialog.get(), suggested_name.c_str());
		}
		gtk_file_dialog_save(dialog.get(), parent, cancellable, &OnSave,
		                     new Pending<FileFn> {m_alive, std::move(done),
		                                          GObjectPtr<GCancellable>::Ref(cancellable)});
	}

private:
	static GObjectPtr<GtkFileDialog> NewDialog(const std::string& title) {
		auto dialog = GObjectPtr<GtkFileDialog>::Adopt(gtk_file_dialog_new());
		gtk_file_dialog_set_title(dialog.get(), title.c_str());
		gtk_file_dialog_set_modal(dialog.get(), TRUE);
		return dialog;
	}

	static void OnFolders(GObject* source, GAsyncResult* result, gpointer data) {
		std::unique_ptr<Pending<FoldersFn>> pending(static_cast<Pending<FoldersFn>*>(data));
		GErrorPtr                           error;
		auto files = GObjectPtr<GListModel>::Adopt(gtk_file_dialog_select_multiple_folders_finish(
		    GTK_FILE_DIALOG(source), result, error.Out()));
		if (!pending->MayCall()) {
			return;
		}
		std::vector<std::filesystem::path> folders;
		std::string                        message;
		if (files) {
			const guint count = g_list_model_get_n_items(files.get());
			for (guint i = 0; i < count; i++) {
				auto file = GObjectPtr<GFile>::Adopt(
				    static_cast<GFile*>(g_list_model_get_item(files.get(), i)));
				const GCharPtr path(g_file_get_path(file.get()));
				if (path) {
					folders.emplace_back(path.get());
				} else if (message.empty()) {
					message = NotLocal(file.get());
				}
			}
		} else if (!IsDismissed(error)) {
			message = std::string(error.Message());
		}
		pending->done(std::move(folders), message);
	}

	static void FinishFile(Pending<FileFn>& pending, GFile* file, const GErrorPtr& error) {
		if (!pending.MayCall()) {
			return;
		}
		if (file == nullptr) {
			pending.done(std::nullopt,
			             IsDismissed(error) ? std::string() : std::string(error.Message()));
			return;
		}
		const GCharPtr path(g_file_get_path(file));
		if (!path) {
			pending.done(std::nullopt, NotLocal(file));
			return;
		}
		pending.done(std::filesystem::path(path.get()), std::string());
	}

	static void OnOpen(GObject* source, GAsyncResult* result, gpointer data) {
		std::unique_ptr<Pending<FileFn>> pending(static_cast<Pending<FileFn>*>(data));
		GErrorPtr                        error;
		auto                             file = GObjectPtr<GFile>::Adopt(
		    gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, error.Out()));
		FinishFile(*pending, file.get(), error);
	}

	static void OnSave(GObject* source, GAsyncResult* result, gpointer data) {
		std::unique_ptr<Pending<FileFn>> pending(static_cast<Pending<FileFn>*>(data));
		GErrorPtr                        error;
		auto                             file = GObjectPtr<GFile>::Adopt(
		    gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, error.Out()));
		FinishFile(*pending, file.get(), error);
	}

	std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace

std::unique_ptr<FileChooser> MakeGtkFileChooser() {
	return std::make_unique<GtkFileChooser>();
}

} // namespace kyty::gtk
