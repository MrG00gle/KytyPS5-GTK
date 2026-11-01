#include "launcher/gtk/logModel.h"

#include "launcher/gtk/gobjectPtr.h"

#include <algorithm>
#include <cstdint>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

// The batch interval of plan D35.
constexpr guint kSyncIntervalMs = 100;

struct LogModelData {
	explicit LogModelData(std::size_t retained_lines): collector(retained_lines) {}

	kyty::core::LogCollector collector;
	// The rows are the lines with the absolute numbers first ... first + size - 1, counted from the
	// last Clear() (the collector's line at index i has the number DroppedCount() + i).
	uint64_t            first = 0;
	guint               size  = 0;
	kyty::gtk::SourceId timer;
};

} // namespace

struct _KytyLogModel {
	GObject       parent_instance;
	LogModelData* data;
};

static void kyty_log_model_list_model_init(GListModelInterface* iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(KytyLogModel, kyty_log_model, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL,
                                                    kyty_log_model_list_model_init))

static GType kyty_log_model_get_item_type(GListModel* /*list*/) {
	return GTK_TYPE_STRING_OBJECT;
}

static guint kyty_log_model_get_n_items(GListModel* list) {
	return KYTY_LOG_MODEL(list)->data->size;
}

static gpointer kyty_log_model_get_item(GListModel* list, guint position) {
	const LogModelData& data = *KYTY_LOG_MODEL(list)->data;
	if (position >= data.size) {
		return nullptr;
	}
	const auto&    lines    = data.collector.Lines();
	const uint64_t number   = data.first + position;
	const uint64_t dropped  = lines.DroppedCount();
	const bool     retained = number >= dropped && number - dropped < lines.Size();
	return gtk_string_object_new(
	    retained ? lines[static_cast<std::size_t>(number - dropped)].c_str() : "");
}

static void kyty_log_model_list_model_init(GListModelInterface* iface) {
	iface->get_item_type = kyty_log_model_get_item_type;
	iface->get_n_items   = kyty_log_model_get_n_items;
	iface->get_item      = kyty_log_model_get_item;
}

static void kyty_log_model_init(KytyLogModel* /*model*/) {}

static void kyty_log_model_dispose(GObject* object) {
	if (auto* data = KYTY_LOG_MODEL(object)->data; data != nullptr) {
		data->timer.Remove();
	}
	G_OBJECT_CLASS(kyty_log_model_parent_class)->dispose(object);
}

static void kyty_log_model_finalize(GObject* object) {
	auto* model = KYTY_LOG_MODEL(object);
	delete model->data;
	model->data = nullptr;
	G_OBJECT_CLASS(kyty_log_model_parent_class)->finalize(object);
}

static void kyty_log_model_class_init(KytyLogModelClass* klass) {
	GObjectClass* object_class = G_OBJECT_CLASS(klass);
	object_class->dispose      = kyty_log_model_dispose;
	object_class->finalize     = kyty_log_model_finalize;
}

KytyLogModel* kyty_log_model_new(std::size_t retained_lines) {
	auto* model = KYTY_LOG_MODEL(g_object_new(KYTY_TYPE_LOG_MODEL, nullptr));
	model->data = new LogModelData(retained_lines);
	return model;
}

kyty::core::LogCollector& kyty_log_model_get_collector(KytyLogModel* model) {
	return model->data->collector;
}

namespace {

gboolean OnSyncTimer(gpointer data) {
	kyty_log_model_sync(KYTY_LOG_MODEL(data));
	return G_SOURCE_REMOVE;
}

} // namespace

void kyty_log_model_schedule_sync(KytyLogModel* model) {
	g_return_if_fail(KYTY_IS_LOG_MODEL(model));
	auto& data = *model->data;
	if (!data.timer.Active()) {
		data.timer = kyty::gtk::SourceId(g_timeout_add(kSyncIntervalMs, &OnSyncTimer, model));
	}
}

void kyty_log_model_sync(KytyLogModel* model) {
	g_return_if_fail(KYTY_IS_LOG_MODEL(model));
	auto& data = *model->data;
	data.timer.Remove();
	const std::vector<std::string> fresh = data.collector.TakeNewLines();
	if (fresh.empty()) {
		return;
	}
	// HC-26: the rows of the lines the collector dropped go first, then the new rows are appended.
	const uint64_t capacity = data.collector.Lines().Capacity();
	const uint64_t total    = static_cast<uint64_t>(data.size) + fresh.size();
	const auto     removed =
	    static_cast<guint>(std::min<uint64_t>(data.size, total > capacity ? total - capacity : 0));
	if (removed > 0) {
		data.first += removed;
		data.size -= removed;
		g_list_model_items_changed(G_LIST_MODEL(model), 0, removed, 0);
	}
	// The new rows start at the oldest retained line: when more lines than the capacity arrived,
	// the collector dropped some of them (and every row) before they could be shown.
	data.first           = data.collector.Lines().DroppedCount();
	const guint position = data.size;
	const auto  added    = static_cast<guint>(fresh.size());
	data.size += added;
	g_list_model_items_changed(G_LIST_MODEL(model), position, 0, added);
}

void kyty_log_model_clear(KytyLogModel* model) {
	g_return_if_fail(KYTY_IS_LOG_MODEL(model));
	auto& data = *model->data;
	data.timer.Remove();
	data.collector.Clear();
	const guint removed = data.size;
	data.first          = 0;
	data.size           = 0;
	if (removed > 0) {
		g_list_model_items_changed(G_LIST_MODEL(model), 0, removed, 0);
	}
}

void kyty_log_model_append_line(KytyLogModel* model, std::string_view line) {
	g_return_if_fail(KYTY_IS_LOG_MODEL(model));
	std::string text(line);
	text += '\n';
	model->data->collector.Append(kyty::core::Stream::Stdout,
	                              std::as_bytes(std::span(text.data(), text.size())));
	kyty_log_model_sync(model);
}

kyty::core::Expected<void, kyty::core::IoError>
kyty_log_model_save(KytyLogModel* model, const std::filesystem::path& path) {
	kyty_log_model_sync(model);
	return kyty::core::WriteFileAtomically(path, model->data->collector.ToText());
}
