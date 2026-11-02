// The fake desktop portal of the UI tests (task 22.1; phase 7 plan D8): ui_fake_portal owns
// org.freedesktop.portal.Desktop on the session bus of tests/ui/with-display.sh and exports
// /org/freedesktop/portal/desktop with
// - org.freedesktop.portal.Settings version 2 (Read, ReadOne, ReadAll, the SettingChanged signal,
//   the version property) with the namespace org.freedesktop.appearance: color-scheme (uint32;
//   0 no preference, 1 prefer dark, 2 prefer light) and contrast (uint32 0). Any other key answers
//   org.freedesktop.portal.Error.NotFound; ReadAll returns the namespaces its patterns match (an
//   exact name, a prefix ending in "*", or every namespace for an empty list or an empty pattern).
// - io.github.MrG00gle.KytyPS5_GTK.TestPortal: SetColorScheme(u) stores the value and emits
//   SettingChanged, so a case can switch the desktop's color scheme while the launcher runs.
// The initial color scheme comes from --color-scheme <0|1|2>, else KYTY_UI_COLOR_SCHEME, else 0.
// Once the name is owned it prints "ui_fake_portal: ready ..." (with-display.sh waits for that
// line). SIGINT and SIGTERM end it with status 0 (so a coverage build writes its profile); losing
// or not getting the name ends it with status 1. It uses GIO only, no launcher code.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gio/gio.h>
#include <glib-unix.h>
#include <string>
#include <string_view>

namespace {

constexpr const char* kBusName           = "org.freedesktop.portal.Desktop";
constexpr const char* kObjectPath        = "/org/freedesktop/portal/desktop";
constexpr const char* kSettings          = "org.freedesktop.portal.Settings";
constexpr const char* kTestPortal        = "io.github.MrG00gle.KytyPS5_GTK.TestPortal";
constexpr const char* kAppearance        = "org.freedesktop.appearance";
constexpr const char* kNotFound          = "org.freedesktop.portal.Error.NotFound";
constexpr const char* kInvalidArgument   = "org.freedesktop.portal.Error.InvalidArgument";
constexpr guint32     kSettingsVersion   = 2;
constexpr guint32     kMaximumColorValue = 2;

constexpr const char* kIntrospection = R"(<node>
  <interface name="org.freedesktop.portal.Settings">
    <method name="ReadAll">
      <arg type="as" name="namespaces" direction="in"/>
      <arg type="a{sa{sv}}" name="value" direction="out"/>
    </method>
    <method name="Read">
      <arg type="s" name="namespace" direction="in"/>
      <arg type="s" name="key" direction="in"/>
      <arg type="v" name="value" direction="out"/>
    </method>
    <method name="ReadOne">
      <arg type="s" name="namespace" direction="in"/>
      <arg type="s" name="key" direction="in"/>
      <arg type="v" name="value" direction="out"/>
    </method>
    <signal name="SettingChanged">
      <arg type="s" name="namespace"/>
      <arg type="s" name="key"/>
      <arg type="v" name="value"/>
    </signal>
    <property name="version" type="u" access="read"/>
  </interface>
  <interface name="io.github.MrG00gle.KytyPS5_GTK.TestPortal">
    <method name="SetColorScheme">
      <arg type="u" name="color_scheme" direction="in"/>
    </method>
  </interface>
</node>)";

struct Portal {
	GMainLoop*       loop         = nullptr;
	GDBusNodeInfo*   node         = nullptr;
	GDBusConnection* connection   = nullptr;
	guint32          color_scheme = 0;
	guint32          contrast     = 0;
	int              status       = 0;
};

// The value of an appearance key, or nullptr for a key the portal does not have.
GVariant* AppearanceValue(const Portal& portal, std::string_view key) {
	if (key == "color-scheme") {
		return g_variant_new_uint32(portal.color_scheme);
	}
	if (key == "contrast") {
		return g_variant_new_uint32(portal.contrast);
	}
	return nullptr;
}

// A ReadAll pattern: an exact namespace, a prefix ending in "*", or "" for every namespace.
bool Matches(std::string_view pattern, std::string_view name) {
	if (pattern.empty()) {
		return true;
	}
	if (pattern.back() == '*') {
		return name.substr(0, pattern.size() - 1) == pattern.substr(0, pattern.size() - 1);
	}
	return pattern == name;
}

void ReadAll(const Portal& portal, GVariant* parameters, GDBusMethodInvocation* invocation) {
	GVariantIter* patterns = nullptr;
	g_variant_get(parameters, "(as)", &patterns);
	bool        matched = g_variant_iter_n_children(patterns) == 0;
	const char* pattern = nullptr;
	while (g_variant_iter_next(patterns, "&s", &pattern)) {
		matched = matched || Matches(pattern, kAppearance);
	}
	g_variant_iter_free(patterns);

	GVariantBuilder result;
	g_variant_builder_init(&result, G_VARIANT_TYPE("a{sa{sv}}"));
	if (matched) {
		GVariantBuilder values;
		g_variant_builder_init(&values, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&values, "{sv}", "color-scheme",
		                      AppearanceValue(portal, "color-scheme"));
		g_variant_builder_add(&values, "{sv}", "contrast", AppearanceValue(portal, "contrast"));
		g_variant_builder_add(&result, "{sa{sv}}", kAppearance, &values);
	}
	g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{sa{sv}})", &result));
}

// Read (version 1) wraps the value in a second variant; ReadOne (version 2) does not.
void Read(const Portal& portal, GVariant* parameters, GDBusMethodInvocation* invocation,
          bool wrapped) {
	const char* name_space = nullptr;
	const char* key        = nullptr;
	g_variant_get(parameters, "(&s&s)", &name_space, &key);
	GVariant* value =
	    std::strcmp(name_space, kAppearance) == 0 ? AppearanceValue(portal, key) : nullptr;
	if (value == nullptr) {
		g_dbus_method_invocation_return_dbus_error(invocation, kNotFound,
		                                           "Requested setting not found");
		return;
	}
	g_dbus_method_invocation_return_value(
	    invocation, g_variant_new("(v)", wrapped ? g_variant_new_variant(value) : value));
}

void SetColorScheme(Portal& portal, GVariant* parameters, GDBusMethodInvocation* invocation) {
	guint32 value = 0;
	g_variant_get(parameters, "(u)", &value);
	if (value > kMaximumColorValue) {
		g_dbus_method_invocation_return_dbus_error(invocation, kInvalidArgument,
		                                           "color-scheme must be 0, 1, or 2");
		return;
	}
	portal.color_scheme = value;
	GError* error       = nullptr;
	if (g_dbus_connection_emit_signal(
	        portal.connection, nullptr, kObjectPath, kSettings, "SettingChanged",
	        g_variant_new("(ssv)", kAppearance, "color-scheme", g_variant_new_uint32(value)),
	        &error) == FALSE) {
		std::fprintf(stderr, "ui_fake_portal: cannot emit SettingChanged: %s\n", error->message);
		g_error_free(error);
	}
	std::printf("ui_fake_portal: color-scheme %u\n", value);
	std::fflush(stdout);
	g_dbus_method_invocation_return_value(invocation, nullptr);
}

void OnMethodCall(GDBusConnection* /*connection*/, const char* /*sender*/,
                  const char* /*object_path*/, const char* interface_name, const char* method_name,
                  GVariant* parameters, GDBusMethodInvocation* invocation, gpointer data) {
	auto&                  portal    = *static_cast<Portal*>(data);
	const std::string_view interface = interface_name;
	const std::string_view method    = method_name;
	if (interface == kSettings && method == "ReadAll") {
		ReadAll(portal, parameters, invocation);
	} else if (interface == kSettings && (method == "Read" || method == "ReadOne")) {
		Read(portal, parameters, invocation, method == "Read");
	} else if (interface == kTestPortal && method == "SetColorScheme") {
		SetColorScheme(portal, parameters, invocation);
	} else {
		g_dbus_method_invocation_return_dbus_error(
		    invocation, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method");
	}
}

GVariant* OnGetProperty(GDBusConnection* /*connection*/, const char* /*sender*/,
                        const char* /*object_path*/, const char* /*interface_name*/,
                        const char* property_name, GError** error, gpointer /*data*/) {
	if (std::strcmp(property_name, "version") == 0) {
		return g_variant_new_uint32(kSettingsVersion);
	}
	g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property %s",
	            property_name);
	return nullptr;
}

const GDBusInterfaceVTable kVTable = {OnMethodCall, OnGetProperty, nullptr, {nullptr}};

void OnBusAcquired(GDBusConnection* connection, const char* /*name*/, gpointer data) {
	auto& portal      = *static_cast<Portal*>(data);
	portal.connection = connection;
	for (GDBusInterfaceInfo** info = portal.node->interfaces; *info != nullptr; info++) {
		GError* error = nullptr;
		if (g_dbus_connection_register_object(connection, kObjectPath, *info, &kVTable, &portal,
		                                      nullptr, &error) == 0) {
			std::fprintf(stderr, "ui_fake_portal: cannot export %s: %s\n", (*info)->name,
			             error->message);
			g_error_free(error);
			portal.status = 1;
			g_main_loop_quit(portal.loop);
			return;
		}
	}
}

void OnNameAcquired(GDBusConnection* /*connection*/, const char* name, gpointer data) {
	const auto& portal = *static_cast<Portal*>(data);
	std::printf("ui_fake_portal: ready (%s, color-scheme %u)\n", name, portal.color_scheme);
	std::fflush(stdout);
}

void OnNameLost(GDBusConnection* connection, const char* name, gpointer data) {
	auto& portal = *static_cast<Portal*>(data);
	std::fprintf(stderr, "ui_fake_portal: %s %s\n",
	             connection == nullptr ? "no session bus for" : "lost or could not own", name);
	portal.status = 1;
	g_main_loop_quit(portal.loop);
}

gboolean OnQuitSignal(gpointer data) {
	g_main_loop_quit(static_cast<Portal*>(data)->loop);
	return G_SOURCE_REMOVE;
}

// 0, 1, or 2; -1 for anything else.
int ParseColorScheme(const char* text) {
	if (text != nullptr && text[0] >= '0' && text[0] <= '2' && text[1] == '\0') {
		return text[0] - '0';
	}
	return -1;
}

} // namespace

int main(int argc, char** argv) {
	Portal      portal;
	const char* initial = std::getenv("KYTY_UI_COLOR_SCHEME");
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--color-scheme") == 0 && i + 1 < argc) {
			initial = argv[++i];
		} else {
			std::fprintf(stderr, "usage: ui_fake_portal [--color-scheme 0|1|2]\n");
			return 2;
		}
	}
	if (initial != nullptr && initial[0] != '\0') {
		const int value = ParseColorScheme(initial);
		if (value < 0) {
			std::fprintf(stderr, "ui_fake_portal: the color scheme must be 0, 1, or 2, not %s\n",
			             initial);
			return 2;
		}
		portal.color_scheme = static_cast<guint32>(value);
	}

	GError* error = nullptr;
	portal.node   = g_dbus_node_info_new_for_xml(kIntrospection, &error);
	if (portal.node == nullptr) {
		std::fprintf(stderr, "ui_fake_portal: bad introspection data: %s\n", error->message);
		g_error_free(error);
		return 1;
	}
	portal.loop = g_main_loop_new(nullptr, FALSE);
	g_unix_signal_add(SIGINT, OnQuitSignal, &portal);
	g_unix_signal_add(SIGTERM, OnQuitSignal, &portal);
	const guint owner =
	    g_bus_own_name(G_BUS_TYPE_SESSION, kBusName, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
	                   OnBusAcquired, OnNameAcquired, OnNameLost, &portal, nullptr);
	g_main_loop_run(portal.loop);
	g_bus_unown_name(owner);
	g_main_loop_unref(portal.loop);
	g_dbus_node_info_unref(portal.node);
	return portal.status;
}
