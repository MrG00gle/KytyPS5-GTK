#include "ui/support/uiHarness.h"

#include "ui/support/uiDriver.h"

#include <adwaita.h>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gtk/gtk.h>
#include <map>
#include <mutex>
#include <system_error>
#include <vector>

// From the source that glib-compile-resources --manual-register generates (src/launcher/gtk).
extern "C" void kyty_launcher_register_resource(void);

namespace KytyUi {
namespace {

constexpr const char* kProgram = "launcher_ui_tests";

struct ExpectedLog {
	std::string    domain;
	GLogLevelFlags level = G_LOG_LEVEL_WARNING;
	std::string    prefix;
	int            seen = 0;
};

// The harness state. The log writer may run on any thread (GTask workers log too), so everything
// it touches is under the mutex. Never destroyed: GLib may log while the process exits.
struct HarnessState {
	std::mutex                     mutex;
	std::map<std::string, CaseFn>  cases;
	std::map<std::string, guint32> initial_color_schemes;
	std::string                    current  = kProgram;
	int                            failures = 0;
	std::vector<ExpectedLog>       expected;
	std::filesystem::path          capture_directory;
};

HarnessState& State() {
	static auto* state = new HarnessState;
	return *state;
}

const char* LevelName(GLogLevelFlags level) {
	if ((level & G_LOG_LEVEL_ERROR) != 0) {
		return "ERROR";
	}
	if ((level & G_LOG_LEVEL_CRITICAL) != 0) {
		return "CRITICAL";
	}
	return "WARNING";
}

std::string FieldText(const GLogField& field) {
	if (field.value == nullptr) {
		return "";
	}
	const auto* text = static_cast<const char*>(field.value);
	return field.length < 0 ? std::string(text)
	                        : std::string(text, static_cast<std::size_t>(field.length));
}

// D11: warnings, criticals, and errors of every domain fail the case unless they were expected.
GLogWriterOutput Writer(GLogLevelFlags level, const GLogField* fields, gsize count,
                        gpointer user_data) {
	const auto severity = static_cast<GLogLevelFlags>(level & G_LOG_LEVEL_MASK);
	if ((severity & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING)) == 0) {
		return g_log_writer_default(level, fields, count, user_data);
	}
	std::string domain;
	std::string message;
	std::string file;
	std::string line;
	std::string function;
	for (gsize i = 0; i < count; i++) {
		const std::string_view key = fields[i].key;
		if (key == "GLIB_DOMAIN") {
			domain = FieldText(fields[i]);
		} else if (key == "MESSAGE") {
			message = FieldText(fields[i]);
		} else if (key == "CODE_FILE") {
			file = FieldText(fields[i]);
		} else if (key == "CODE_LINE") {
			line = FieldText(fields[i]);
		} else if (key == "CODE_FUNC") {
			function = FieldText(fields[i]);
		}
	}
	auto&                             state = State();
	const std::lock_guard<std::mutex> lock(state.mutex);
	for (auto& expected: state.expected) {
		if (expected.domain == domain && (severity & expected.level) != 0 &&
		    message.compare(0, expected.prefix.size(), expected.prefix) == 0) {
			expected.seen++;
			std::fprintf(stderr, "%s: expected %s from %s: %s\n", state.current.c_str(),
			             LevelName(severity), domain.c_str(), message.c_str());
			return G_LOG_WRITER_HANDLED;
		}
	}
	state.failures++;
	std::string where;
	if (!file.empty()) {
		where = " (" + file + (line.empty() ? "" : ":" + line) +
		        (function.empty() ? "" : " " + function) + ")";
	}
	std::fprintf(stderr, "%s: FAILED: %s from %s: %s%s\n", state.current.c_str(),
	             LevelName(severity), domain.empty() ? "(no domain)" : domain.c_str(),
	             message.c_str(), where.c_str());
	return G_LOG_WRITER_HANDLED;
}

int Usage() {
	std::fprintf(stderr,
	             "usage: %s --list\n"
	             "       %s --case <name> [--capture-dir <directory>]\n",
	             kProgram, kProgram);
	return 1;
}

} // namespace

CaseRegistrar::CaseRegistrar(const char* name, CaseFn run) {
	auto& state = State();
	if (!state.cases.emplace(name, run).second) {
		std::fprintf(stderr, "%s: the case %s is defined twice\n", kProgram, name);
		std::abort();
	}
}

InitialColorScheme::InitialColorScheme(const char* case_name, guint32 value) {
	State().initial_color_schemes[case_name] = value;
}

void Fail(std::string_view what) {
	auto&                             state = State();
	const std::lock_guard<std::mutex> lock(state.mutex);
	state.failures++;
	std::fprintf(stderr, "%s: FAILED: %.*s\n", state.current.c_str(), static_cast<int>(what.size()),
	             what.data());
}

bool Check(bool value, std::string_view what) {
	if (!value) {
		Fail(what);
	}
	return value;
}

int Failures() {
	auto&                             state = State();
	const std::lock_guard<std::mutex> lock(state.mutex);
	return state.failures;
}

void ExpectLog(std::string_view domain, GLogLevelFlags level, std::string_view prefix) {
	auto&                             state = State();
	const std::lock_guard<std::mutex> lock(state.mutex);
	state.expected.push_back({std::string(domain), level, std::string(prefix), 0});
}

const std::string& CaseName() {
	return State().current;
}

const std::filesystem::path& CaptureDirectory() {
	return State().capture_directory;
}

std::vector<std::string> CaseNames() {
	std::vector<std::string> names;
	for (const auto& [name, run]: State().cases) {
		names.push_back(name);
	}
	return names;
}

} // namespace KytyUi

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	// First: every message from here on goes through the harness's writer (D11).
	g_log_set_writer_func(&KytyUi::Writer, nullptr, nullptr);

	auto&       state = KytyUi::State();
	bool        list  = false;
	std::string name;
	std::string capture_directory;
	for (int i = 1; i < argc; i++) {
		const std::string_view argument = argv[i];
		if (argument == "--list") {
			list = true;
		} else if (argument == "--case" && i + 1 < argc) {
			name = argv[++i];
		} else if (argument == "--capture-dir" && i + 1 < argc) {
			capture_directory = argv[++i];
		} else {
			return KytyUi::Usage();
		}
	}
	if (list) {
		for (const auto& [case_name, run]: state.cases) {
			std::printf("%s\n", case_name.c_str());
		}
		return 0;
	}
	if (name.empty()) {
		return KytyUi::Usage();
	}
	const auto found = state.cases.find(name);
	if (found == state.cases.end()) {
		std::fprintf(stderr, "%s: unknown case %s (--list names them)\n", KytyUi::kProgram,
		             name.c_str());
		return 1;
	}
	if (capture_directory.empty()) {
		const char* from_environment = std::getenv("KYTY_UI_CAPTURE_DIR");
		capture_directory            = from_environment != nullptr ? from_environment : "";
	}
	if (!capture_directory.empty()) {
		std::error_code ec;
		std::filesystem::create_directories(capture_directory, ec);
		if (ec) {
			std::fprintf(stderr, "%s: cannot create %s: %s\n", KytyUi::kProgram,
			             capture_directory.c_str(), ec.message().c_str());
			return 1;
		}
		state.capture_directory = std::filesystem::absolute(capture_directory, ec);
	}
	{
		const std::lock_guard<std::mutex> lock(state.mutex);
		state.current = name;
	}

	// The desktop's preference before GTK and libadwaita read it (InitialColorScheme).
	if (const auto scheme = state.initial_color_schemes.find(name);
	    scheme != state.initial_color_schemes.end()) {
		std::printf("%s: the portal's color-scheme is %u before gtk_init\n", name.c_str(),
		            scheme->second);
		KytyUi::SetPortalColorScheme(scheme->second);
	}
	std::setlocale(LC_ALL, "");
	if (gtk_init_check() == FALSE) {
		std::fprintf(stderr,
		             "%s: %s: cannot open a display (run it through tests/ui/with-display.sh)\n",
		             KytyUi::kProgram, name.c_str());
		return 1;
	}
	adw_init();
	kyty_launcher_register_resource();
	const char* backend = g_getenv("GDK_BACKEND");
	const char* a11y    = g_getenv("GTK_A11Y");
	std::printf("%s: start (display %s, GDK_BACKEND %s, GTK_A11Y %s)\n", name.c_str(),
	            gdk_display_get_name(gdk_display_get_default()),
	            backend != nullptr ? backend : "(unset)", a11y != nullptr ? a11y : "(unset)");

	found->second();
	// Idles of windows the case destroyed run now, so their messages still count.
	KytyUi::Pump(100);

	std::vector<KytyUi::ExpectedLog> expected;
	{
		const std::lock_guard<std::mutex> lock(state.mutex);
		expected = state.expected;
	}
	for (const auto& log: expected) {
		KytyUi::Check(log.seen == 1, "the expected " + std::string(KytyUi::LevelName(log.level)) +
		                                 " from " + log.domain + " \"" + log.prefix +
		                                 "...\" arrived " + std::to_string(log.seen) +
		                                 " time(s), not once");
	}
	const int failures = KytyUi::Failures();
	if (failures > 0) {
		std::printf("%s: FAIL (%d failure(s))\n", name.c_str(), failures);
		return 1;
	}
	std::printf("%s: PASS\n", name.c_str());
	return 0;
}
