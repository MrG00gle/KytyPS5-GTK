#include "launcher/gtk/gameObject.h"

#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"

#include <utility>
#include <vector>

namespace {

struct GameData {
	kyty::core::GameEntry           entry;
	kyty::core::GameRow             row;
	guint                           ordinal = 0;
	kyty::core::CompatibilityStatus status  = kyty::core::CompatibilityStatus::Unknown;
	bool                            running = false;
	GdkTexture*                     texture = nullptr; // owned
};

enum GameProperty {
	kPropTitle = 1,
	kPropStatus,
	kPropComment,
	kPropRunning,
	kPropTexture,
	kPropSize,
	kPropMetadataUnavailable,
	kPropCount
};

GParamSpec* g_game_properties[kPropCount] = {};

} // namespace

struct _KytyGame {
	GObject   parent_instance;
	GameData* data;
};

G_DEFINE_FINAL_TYPE(KytyGame, kyty_game, G_TYPE_OBJECT)

static void kyty_game_init(KytyGame* game) {
	game->data = new GameData;
}

static void kyty_game_finalize(GObject* object) {
	KytyGame* game = KYTY_GAME(object);
	g_clear_object(&game->data->texture);
	delete game->data;
	game->data = nullptr;
	G_OBJECT_CLASS(kyty_game_parent_class)->finalize(object);
}

static void kyty_game_get_property(GObject* object, guint id, GValue* value, GParamSpec* pspec) {
	const GameData& data = *KYTY_GAME(object)->data;
	switch (id) {
		case kPropTitle: g_value_set_string(value, data.entry.title_name.c_str()); break;
		case kPropStatus: g_value_set_int(value, static_cast<int>(data.status)); break;
		case kPropComment: g_value_set_string(value, data.row.comment.c_str()); break;
		case kPropRunning: g_value_set_boolean(value, data.running ? TRUE : FALSE); break;
		case kPropTexture: g_value_set_object(value, data.texture); break;
		case kPropSize: g_value_set_uint64(value, data.row.size_bytes.value_or(G_MAXUINT64)); break;
		case kPropMetadataUnavailable:
			g_value_set_boolean(value, data.entry.metadata_available ? FALSE : TRUE);
			break;
		default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec); break;
	}
}

static void kyty_game_class_init(KytyGameClass* klass) {
	GObjectClass* object_class = G_OBJECT_CLASS(klass);
	object_class->finalize     = kyty_game_finalize;
	object_class->get_property = kyty_game_get_property;

	constexpr auto kFlags =
	    GParamFlags(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
	g_game_properties[kPropTitle] = g_param_spec_string("title", nullptr, nullptr, "", kFlags);
	g_game_properties[kPropStatus] =
	    g_param_spec_int("status", nullptr, nullptr, 0,
	                     static_cast<int>(kyty::core::CompatibilityStatus::DoesntBoot), 0, kFlags);
	g_game_properties[kPropComment] = g_param_spec_string("comment", nullptr, nullptr, "", kFlags);
	g_game_properties[kPropRunning] =
	    g_param_spec_boolean("running", nullptr, nullptr, FALSE, kFlags);
	g_game_properties[kPropTexture] =
	    g_param_spec_object("texture", nullptr, nullptr, GDK_TYPE_TEXTURE, kFlags);
	g_game_properties[kPropSize] =
	    g_param_spec_uint64("size", nullptr, nullptr, 0, G_MAXUINT64, G_MAXUINT64, kFlags);
	g_game_properties[kPropMetadataUnavailable] =
	    g_param_spec_boolean("metadata-unavailable", nullptr, nullptr, FALSE, kFlags);
	g_object_class_install_properties(object_class, kPropCount, g_game_properties);
}

KytyGame* kyty_game_new(kyty::core::GameEntry entry, guint ordinal) {
	auto* game             = static_cast<KytyGame*>(g_object_new(KYTY_TYPE_GAME, nullptr));
	game->data->row        = kyty::core::RowFromEntry(entry);
	game->data->row.status = kyty::gtk::StatusText(kyty::core::CompatibilityStatus::Unknown);
	game->data->entry      = std::move(entry);
	game->data->ordinal    = ordinal;
	return game;
}

const kyty::core::GameEntry& kyty_game_get_entry(KytyGame* game) {
	return game->data->entry;
}

const kyty::core::GameRow& kyty_game_get_row(KytyGame* game) {
	return game->data->row;
}

guint kyty_game_get_ordinal(KytyGame* game) {
	return game->data->ordinal;
}

kyty::core::CompatibilityStatus kyty_game_get_status(KytyGame* game) {
	return game->data->status;
}

void kyty_game_set_status(KytyGame* game, kyty::core::CompatibilityStatus status) {
	g_return_if_fail(KYTY_IS_GAME(game));
	if (game->data->status == status) {
		return;
	}
	game->data->status     = status;
	game->data->row.status = kyty::gtk::StatusText(status);
	g_object_notify_by_pspec(G_OBJECT(game), g_game_properties[kPropStatus]);
}

const std::string& kyty_game_get_comment(KytyGame* game) {
	return game->data->row.comment;
}

void kyty_game_set_comment(KytyGame* game, std::string comment) {
	g_return_if_fail(KYTY_IS_GAME(game));
	if (game->data->row.comment == comment) {
		return;
	}
	game->data->row.comment = std::move(comment);
	g_object_notify_by_pspec(G_OBJECT(game), g_game_properties[kPropComment]);
}

bool kyty_game_get_running(KytyGame* game) {
	return game->data->running;
}

void kyty_game_set_running(KytyGame* game, bool running) {
	g_return_if_fail(KYTY_IS_GAME(game));
	if (game->data->running == running) {
		return;
	}
	game->data->running = running;
	g_object_notify_by_pspec(G_OBJECT(game), g_game_properties[kPropRunning]);
}

GdkTexture* kyty_game_get_texture(KytyGame* game) {
	return game->data->texture;
}

void kyty_game_set_texture(KytyGame* game, GdkTexture* texture) {
	g_return_if_fail(KYTY_IS_GAME(game));
	if (g_set_object(&game->data->texture, texture)) {
		g_object_notify_by_pspec(G_OBJECT(game), g_game_properties[kPropTexture]);
	}
}

std::optional<uint64_t> kyty_game_get_size(KytyGame* game) {
	return game->data->row.size_bytes;
}

void kyty_game_set_size(KytyGame* game, std::optional<uint64_t> size) {
	g_return_if_fail(KYTY_IS_GAME(game));
	if (game->data->row.size_bytes == size) {
		return;
	}
	game->data->row.size_bytes = size;
	g_object_notify_by_pspec(G_OBJECT(game), g_game_properties[kPropSize]);
}

bool kyty_game_get_metadata_unavailable(KytyGame* game) {
	return !game->data->entry.metadata_available;
}

namespace kyty::gtk {

const char* StatusText(core::CompatibilityStatus status) {
	switch (status) {
		case core::CompatibilityStatus::InGame: return C_("compatibility status", "In game");
		case core::CompatibilityStatus::MainMenu: return C_("compatibility status", "Main menu");
		case core::CompatibilityStatus::Logo: return C_("compatibility status", "Logo");
		case core::CompatibilityStatus::DoesntBoot:
			return C_("compatibility status", "Doesn't boot");
		case core::CompatibilityStatus::Unknown: break;
	}
	return C_("compatibility status", "Unknown");
}

std::string GameTooltip(KytyGame* game) {
	const core::GameEntry&   entry = kyty_game_get_entry(game);
	const core::GameRow&     row   = kyty_game_get_row(game);
	std::vector<std::string> lines {entry.title_name};
	const auto               add = [&lines](const char* format, const std::string& value) {
		if (!value.empty()) {
			const GCharPtr line(g_strdup_printf(format, value.c_str()));
			lines.emplace_back(line.View());
		}
	};
	// TRANSLATORS: tooltip lines of a game; %s is the value from the game's metadata.
	add(_("Serial ID: %s"), row.title_id);
	add(_("Game version: %s"), row.game_version);
	add(_("Firmware version: %s"), row.firmware_version);
	add(_("Path: %s"), row.path);
	add(_("Comment: %s"), row.comment);
	if (!entry.metadata_available) {
		lines.emplace_back(_("The game's sce_sys/param.json is missing or unusable, so its "
		                     "metadata is unavailable."));
	}
	std::string text;
	for (const auto& line: lines) {
		if (!text.empty()) {
			text += '\n';
		}
		text += line;
	}
	return text;
}

} // namespace kyty::gtk
