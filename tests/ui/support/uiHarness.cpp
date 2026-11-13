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

struct WatchedLog {
	std::string              domain;
	std::string              prefix;
	std::vector<std::string> messages;
};

// The one message that does not fail a case (docs/testing.md; a deviation from Requirement 17.11
// that the repository owner approved on 2026-11-13): GTK 4.24's X11 backend logs it when a popup
// or dialog paints in the same frame as its parent in a session without a window manager. Only
// this exact text, only from the Gdk domain at warning level, and only while GTK 4.24.x runs; it
// is printed as a note. Every other message still fails the case.
constexpr const char* kToleratedDomain = "Gdk";
constexpr const char* kToleratedMessage =
    "gdk_frame_timings_submitted() called on submitted frame.";
constexpr guint kToleratedMinor = 24;

// The harness state. The log writer may run on any thread (GTask workers log too), so everything
// it touches is under the mutex. Never destroyed: GLib may log while the process exits.
struct HarnessState {
	std::mutex                     mutex;
	std::map<std::string, CaseFn>  cases;
	std::map<std::string, guint32> initial_color_schemes;
	std::string                    current  = kProgram;
	int                            failures = 0;
	std::vector<ExpectedLog>       expected;
	std::vector<WatchedLog>        watched;
	std::filesystem::path          capture_directory;
	std::filesystem::path          screenshot_output;
	// The GTK minor version that the tolerated message is checked against; the harness self-test
	// sets it, -1 means gtk_get_minor_version().
	int gtk_minor_for_testing = -1;
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

// Whether a warning, critical, or error is the tolerated message (kToleratedMessage). The caller
// holds the state's mutex.
bool Tolerated(const HarnessState& state, GLogLevelFlags severity, const std::string& domain,
               const std::string& message, guint* minor) {
	*minor = state.gtk_minor_for_testing >= 0 ? static_cast<guint>(state.gtk_minor_for_testing)
	                                          : gtk_get_minor_version();
	return severity == G_LOG_LEVEL_WARNING && domain == kToleratedDomain &&
	       message == kToleratedMessage && gtk_get_major_version() == 4 &&
	       *minor == kToleratedMinor;
}

// D11: warnings, criticals, and errors of every domain fail the case unless they were expected.
// The domain and the message of a lower-level message, recorded when a watch matches (WatchLog).
void RecordWatched(const GLogField* fields, gsize count) {
	std::string domain;
	std::string message;
	for (gsize i = 0; i < count; i++) {
		const std::string_view key = fields[i].key;
		if (key == "GLIB_DOMAIN") {
			domain = FieldText(fields[i]);
		} else if (key == "MESSAGE") {
			message = FieldText(fields[i]);
		}
	}
	auto&                             state = State();
	const std::lock_guard<std::mutex> lock(state.mutex);
	for (auto& watch: state.watched) {
		if (watch.domain == domain && message.compare(0, watch.prefix.size(), watch.prefix) == 0) {
			watch.messages.push_back(message);
		}
	}
}

GLogWriterOutput Writer(GLogLevelFlags level, const GLogField* fields, gsize count,
                        gpointer user_data) {
	const auto severity = static_cast<GLogLevelFlags>(level & G_LOG_LEVEL_MASK);
	if ((severity & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING)) == 0) {
		RecordWatched(fields, count);
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
	if (guint minor = 0; Tolerated(state, severity, domain, message, &minor)) {
		std::fprintf(stderr,
		             "%s: NOTE: tolerated on GTK 4.%u (a GTK 4.24 bug; docs/testing.md): WARNING "
		             "from %s: %s\n",
		             state.current.c_str(), minor, domain.c_str(), message.c_str());
		return G_LOG_WRITER_HANDLED;
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
	             "       %s --case <name> [--capture-dir <directory>] [--screenshot-out <file>]\n"
	             "       %s --selftest critical|warning|expected-warning|missing-expected|clean|"
	             "accessible-untyped|frame-timings-gtk-4-24|frame-timings-gtk-4-22|"
	             "frame-timings-near-miss|frame-timings-critical|frame-timings-other-domain\n",
	             kProgram, kProgram, kProgram);
	return 1;
}

// The harness self-test (task 22.6; Requirement 17.11; phase 7 plan D38): each kind runs like a
// case, after gtk_init, and ends as tests/cmake/UiHarnessSelfTest.cmake expects. The kinds are not
// cases: --list does not name them, and no test of their own runs them.
constexpr const char* kSelfTestDomain   = "kyty-launcher";
constexpr const char* kSelfTestExpected = "ui_harness_selftest: a declared warning";

// A real critical from GTK: its precondition check on a widget argument (exit 1).
void SelfTestCritical() {
	gtk_widget_set_visible(nullptr, TRUE);
}

// A warning from the launcher's domain, which no case declared (exit 1).
void SelfTestWarning() {
	g_log(kSelfTestDomain, G_LOG_LEVEL_WARNING, "ui_harness_selftest: a deliberate warning");
}

// A declared warning that arrives once, as the 8.4 offline warning does (exit 0).
void SelfTestExpectedWarning() {
	ExpectLog(kSelfTestDomain, G_LOG_LEVEL_WARNING, kSelfTestExpected);
	g_log(kSelfTestDomain, G_LOG_LEVEL_WARNING, "%s (ExpectLog)", kSelfTestExpected);
}

// A declared warning that never arrives (exit 1).
void SelfTestMissingExpected() {
	ExpectLog(kSelfTestDomain, G_LOG_LEVEL_WARNING, kSelfTestExpected);
}

// Nothing to report (exit 0).
void SelfTestClean() {}

// AccessibleProperty (uiDriver.h) on a property without a boolean, integer, number, string, or
// token value (31: no GtkAccessibleProperty up to GTK 4.22): it fails the case instead of passing
// GTK's test API a value of the wrong type (exit 1; phase 8 plan D17, p7-u2-2).
void SelfTestAccessibleUntyped() {
	GtkWidget* label = gtk_label_new("selftest");
	g_object_ref_sink(label);
	AccessibleProperty(label, static_cast<GtkAccessibleProperty>(31));
	g_object_unref(label);
}

// Logs message with the GTK minor version that the tolerated message is checked against set to
// minor (the version is injected: the self-test runs on any GTK).
void LogWithGtkMinor(int minor, const char* domain, GLogLevelFlags level, const char* message) {
	{
		auto&                             state = State();
		const std::lock_guard<std::mutex> lock(state.mutex);
		state.gtk_minor_for_testing = minor;
	}
	g_log(domain, level, "%s", message);
}

// The tolerated message on GTK 4.24: a note, not a failure (exit 0).
void SelfTestFrameTimingsGtk424() {
	LogWithGtkMinor(24, kToleratedDomain, G_LOG_LEVEL_WARNING, kToleratedMessage);
}

// The same message on GTK 4.22: the exception is off (exit 1).
void SelfTestFrameTimingsGtk422() {
	LogWithGtkMinor(22, kToleratedDomain, G_LOG_LEVEL_WARNING, kToleratedMessage);
}

// A near miss on GTK 4.24, another frame state in the text (exit 1).
void SelfTestFrameTimingsNearMiss() {
	LogWithGtkMinor(24, kToleratedDomain, G_LOG_LEVEL_WARNING,
	                "gdk_frame_timings_submitted() called on discarded frame.");
}

// The exact text as a critical on GTK 4.24 (exit 1).
void SelfTestFrameTimingsCritical() {
	LogWithGtkMinor(24, kToleratedDomain, G_LOG_LEVEL_CRITICAL, kToleratedMessage);
}

// The exact text from another domain on GTK 4.24 (exit 1).
void SelfTestFrameTimingsOtherDomain() {
	LogWithGtkMinor(24, "Gtk", G_LOG_LEVEL_WARNING, kToleratedMessage);
}

struct SelfTest {
	std::string_view kind;
	CaseFn           run;
};

constexpr SelfTest kSelfTests[] = {
    {"critical", &SelfTestCritical},
    {"warning", &SelfTestWarning},
    {"expected-warning", &SelfTestExpectedWarning},
    {"missing-expected", &SelfTestMissingExpected},
    {"clean", &SelfTestClean},
    {"accessible-untyped", &SelfTestAccessibleUntyped},
    {"frame-timings-gtk-4-24", &SelfTestFrameTimingsGtk424},
    {"frame-timings-gtk-4-22", &SelfTestFrameTimingsGtk422},
    {"frame-timings-near-miss", &SelfTestFrameTimingsNearMiss},
    {"frame-timings-critical", &SelfTestFrameTimingsCritical},
    {"frame-timings-other-domain", &SelfTestFrameTimingsOtherDomain},
};

CaseFn FindSelfTest(std::string_view kind) {
	for (const auto& test: kSelfTests) {
		if (test.kind == kind) {
			return test.run;
		}
	}
	return nullptr;
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

void WatchLog(std::string_view domain, std::string_view prefix) {
	auto&                             state = State();
	const std::lock_guard<std::mutex> lock(state.mutex);
	state.watched.push_back({std::string(domain), std::string(prefix), {}});
}

std::vector<std::string> WatchedLogs(std::string_view prefix) {
	auto&                             state = State();
	const std::lock_guard<std::mutex> lock(state.mutex);
	for (const auto& watch: state.watched) {
		if (watch.prefix == prefix) {
			return watch.messages;
		}
	}
	return {};
}

const std::string& CaseName() {
	return State().current;
}

const std::filesystem::path& CaptureDirectory() {
	return State().capture_directory;
}

const std::filesystem::path& ScreenshotOutput() {
	return State().screenshot_output;
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
	std::string selftest;
	std::string capture_directory;
	std::string screenshot_output;
	for (int i = 1; i < argc; i++) {
		const std::string_view argument = argv[i];
		if (argument == "--list") {
			list = true;
		} else if (argument == "--case" && i + 1 < argc) {
			name = argv[++i];
		} else if (argument == "--selftest" && i + 1 < argc) {
			selftest = argv[++i];
		} else if (argument == "--capture-dir" && i + 1 < argc) {
			capture_directory = argv[++i];
		} else if (argument == "--screenshot-out" && i + 1 < argc) {
			screenshot_output = argv[++i];
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
	if (name.empty() == selftest.empty()) {
		return KytyUi::Usage();
	}
	KytyUi::CaseFn run = nullptr;
	if (!selftest.empty()) {
		run = KytyUi::FindSelfTest(selftest);
		if (run == nullptr) {
			std::fprintf(stderr, "%s: unknown self-test %s\n", KytyUi::kProgram, selftest.c_str());
			return KytyUi::Usage();
		}
		name = "selftest-" + selftest;
	} else {
		const auto found = state.cases.find(name);
		if (found == state.cases.end()) {
			std::fprintf(stderr, "%s: unknown case %s (--list names them)\n", KytyUi::kProgram,
			             name.c_str());
			return 1;
		}
		run = found->second;
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
	if (!screenshot_output.empty()) {
		std::error_code ec;
		state.screenshot_output = std::filesystem::absolute(screenshot_output, ec);
		if (ec) {
			std::fprintf(stderr, "%s: cannot resolve %s: %s\n", KytyUi::kProgram,
			             screenshot_output.c_str(), ec.message().c_str());
			return 1;
		}
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

	run();
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
