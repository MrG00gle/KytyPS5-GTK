// Example tests of the GTK launcher's RAII helpers (src/launcher/gtk/gobjectPtr.h, task 19.2):
// GObjectPtr reference counts for adopt, ref, copy, move, reset, and release (watched through weak
// pointers and ref_count), floating references sunk, SignalScope disconnects (also after the
// instance is gone), SourceId removes pending and finished sources, and GErrorPtr and GCharPtr free
// what they hold. No display and no gtk_init: only GLib and GObject.

#include "launcher/gtk/gobjectPtr.h"
#include "support/noDisplay.h"

#include <cstdio>
#include <cstdlib>
#include <gio/gio.h>
#include <glib-object.h>
#include <malloc.h>
#include <string>
#include <utility>

// A floating object type: new instances start with a floating reference, as new widgets do.
G_DECLARE_FINAL_TYPE(KytyTestFloating, kyty_test_floating, KYTY_TEST, FLOATING, GInitiallyUnowned)
struct _KytyTestFloating {
	GInitiallyUnowned parent_instance;
};
G_DEFINE_FINAL_TYPE(KytyTestFloating, kyty_test_floating, G_TYPE_INITIALLY_UNOWNED)
static void kyty_test_floating_class_init(KytyTestFloatingClass* /*klass*/) {}
static void kyty_test_floating_init(KytyTestFloating* /*self*/) {}

namespace {

using kyty::gtk::GCharPtr;
using kyty::gtk::GErrorPtr;
using kyty::gtk::GObjectPtr;
using kyty::gtk::SignalScope;
using kyty::gtk::SourceId;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "GobjectPtrTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

guint RefCount(gpointer object) {
	return g_atomic_int_get(&G_OBJECT(object)->ref_count);
}

// A new action whose finalization clears *weak.
GSimpleAction* NewWatchedAction(gpointer* weak) {
	GSimpleAction* action = g_simple_action_new("test", nullptr);
	*weak                 = action;
	g_object_add_weak_pointer(G_OBJECT(action), weak);
	return action;
}

void TestAdoptAndReset() {
	gpointer weak = nullptr;
	{
		auto action = GObjectPtr<GSimpleAction>::Adopt(NewWatchedAction(&weak));
		Check(action && action.get() == weak, "Adopt holds the object");
		Check(RefCount(action.get()) == 1, "Adopt takes over the reference without adding one");
		action.reset();
		Check(!action && weak == nullptr, "reset drops the only reference");
	}
	{
		auto action = GObjectPtr<GSimpleAction>::Adopt(NewWatchedAction(&weak));
		static_cast<void>(action);
	}
	Check(weak == nullptr, "the destructor drops the reference");
}

void TestRefCopyAndMove() {
	gpointer       weak   = nullptr;
	GSimpleAction* raw    = NewWatchedAction(&weak);
	auto           shared = GObjectPtr<GSimpleAction>::Ref(raw);
	Check(RefCount(raw) == 2, "Ref adds a reference");
	g_object_unref(raw);
	Check(RefCount(raw) == 1 && weak != nullptr, "the pointer's reference keeps the object");

	{
		auto copy = shared;
		Check(copy == shared && RefCount(raw) == 2, "a copy adds a reference");
		GObjectPtr<GSimpleAction> assigned;
		assigned = copy;
		Check(RefCount(raw) == 3, "copy assignment adds a reference");
		const auto& alias = assigned;
		assigned          = alias;
		Check(RefCount(raw) == 3, "self-assignment keeps the count");
	}
	Check(RefCount(raw) == 1, "the copies drop their references");

	auto moved = std::move(shared);
	Check(!shared && moved.get() == raw && RefCount(raw) == 1, "a move transfers the reference");
	GObjectPtr<GSimpleAction> target;
	target = std::move(moved);
	Check(!moved && target.get() == raw && RefCount(raw) == 1, "move assignment transfers it");

	GSimpleAction* released = target.release();
	Check(!target && released == raw && RefCount(raw) == 1, "release gives the reference away");
	target = GObjectPtr<GSimpleAction>::Adopt(released);
	target = GObjectPtr<GSimpleAction>();
	Check(weak == nullptr, "assigning an empty pointer drops the reference");
}

void TestFloating() {
	gpointer weak = nullptr;
	auto*    floating =
	    static_cast<KytyTestFloating*>(g_object_new(kyty_test_floating_get_type(), nullptr));
	weak = floating;
	g_object_add_weak_pointer(G_OBJECT(floating), &weak);
	Check(g_object_is_floating(floating), "a new initially unowned object is floating");
	{
		auto owned = GObjectPtr<KytyTestFloating>::Adopt(floating);
		Check(!g_object_is_floating(floating) && RefCount(floating) == 1,
		      "Adopt sinks the floating reference instead of adding one");
	}
	Check(weak == nullptr, "the sunk reference is dropped");

	auto* other =
	    static_cast<KytyTestFloating*>(g_object_new(kyty_test_floating_get_type(), nullptr));
	weak = other;
	g_object_add_weak_pointer(G_OBJECT(other), &weak);
	{
		auto owned = GObjectPtr<KytyTestFloating>::Ref(other);
		Check(!g_object_is_floating(other) && RefCount(other) == 1,
		      "Ref sinks a floating reference");
	}
	Check(weak == nullptr, "Ref's sunk reference is dropped");
}

void OnActivate(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	++*static_cast<int*>(data);
}

void TestSignalScope() {
	int  count  = 0;
	auto action = GObjectPtr<GSimpleAction>::Adopt(g_simple_action_new("test", nullptr));
	{
		auto scope = SignalScope::Connect(action.get(), "activate", G_CALLBACK(OnActivate), &count);
		Check(scope.Connected(), "Connect connects");
		g_action_activate(G_ACTION(action.get()), nullptr);
		Check(count == 1, "the handler runs while the scope lives");

		SignalScope moved = std::move(scope);
		Check(!scope.Connected() && moved.Connected(), "a move transfers the connection");
		g_action_activate(G_ACTION(action.get()), nullptr);
		Check(count == 2, "the moved connection still runs");
	}
	g_action_activate(G_ACTION(action.get()), nullptr);
	Check(count == 2, "the destructor disconnects");

	auto scope = SignalScope::Connect(action.get(), "activate", G_CALLBACK(OnActivate), &count);
	scope.Disconnect();
	g_action_activate(G_ACTION(action.get()), nullptr);
	Check(count == 2 && !scope.Connected(), "Disconnect disconnects");

	// The instance goes first: the scope must not touch it afterwards.
	auto* raw  = g_simple_action_new("gone", nullptr);
	auto  late = SignalScope::Connect(raw, "activate", G_CALLBACK(OnActivate), &count);
	g_object_unref(raw);
	late.Disconnect();
	Check(!late.Connected(), "a scope whose instance is gone empties itself");
	{
		auto* other = g_simple_action_new("gone-too", nullptr);
		auto  kept  = SignalScope::Connect(other, "activate", G_CALLBACK(OnActivate), &count);
		g_object_unref(other);
	}
	Check(SignalScope::Adopt(nullptr, 0).Connected() == false,
	      "an empty connection is not connected");
}

gboolean SetFlag(gpointer data) {
	*static_cast<bool*>(data) = true;
	return G_SOURCE_REMOVE;
}

void Iterate() {
	for (int i = 0; i < 10; i++) {
		g_main_context_iteration(nullptr, FALSE);
	}
}

void TestSourceId() {
	bool ran = false;
	{
		SourceId source(g_timeout_add(0, SetFlag, &ran));
		Check(source.Active(), "a pending source is active");
	}
	Iterate();
	Check(!ran, "the destructor removes a pending source");

	SourceId idle(g_idle_add(SetFlag, &ran));
	idle.Remove();
	Check(!idle.Active(), "Remove empties the scope");
	Iterate();
	Check(!ran, "Remove removes a pending source");

	SourceId finished(g_idle_add(SetFlag, &ran));
	Iterate();
	Check(ran && !finished.Active(), "a source that ended is no longer active");
	finished.Remove(); // must not complain about an unknown source id

	bool     moved_ran = false;
	SourceId first(g_timeout_add(0, SetFlag, &moved_ran));
	SourceId second = std::move(first);
	Check(!first.Active() && second.Active(), "a move transfers the source");
	second = SourceId();
	Iterate();
	Check(!moved_ran, "assigning an empty scope removes the source");
	Check(!SourceId(0).Active(), "id 0 gives an empty scope");
}

// Bytes in use by malloc (main arena and mmapped chunks).
std::size_t BytesInUse() {
	const struct mallinfo2 info = mallinfo2();
	return info.uordblks + info.hblkhd;
}

constexpr gsize kLarge = gsize {4} << 20u;

void TestGCharPtr() {
	const std::size_t before = BytesInUse();
	{
		GCharPtr text(g_strnfill(kLarge, 'x'));
		Check(text && text.View().size() == kLarge, "GCharPtr holds the string");
		Check(BytesInUse() >= before + kLarge, "the string is allocated");
	}
	Check(BytesInUse() < before + kLarge, "the destructor frees the string");

	GCharPtr out;
	*out.Out() = g_strdup("first");
	Check(out.View() == "first", "Out() fills the pointer");
	*out.Out()                = g_strnfill(kLarge, 'y');
	const std::size_t holding = BytesInUse();
	out.reset();
	Check(BytesInUse() + kLarge <= holding && !out && out.View().empty(), "reset frees the string");

	GCharPtr owner(g_strdup("moved"));
	GCharPtr target = std::move(owner);
	Check(!owner && target.View() == "moved", "a move transfers the string");
	gchar* released = target.release();
	Check(!target && std::string(released) == "moved", "release gives the string away");
	g_free(released);
}

void TestGErrorPtr() {
	const std::string message(kLarge, 'e');
	const std::size_t baseline = BytesInUse();
	{
		GErrorPtr error(g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, message.c_str()));
		Check(error && error->code == G_IO_ERROR_FAILED && error.Message().size() == kLarge,
		      "GErrorPtr holds the error");
		Check(BytesInUse() >= baseline + kLarge, "the error message is allocated");
	}
	Check(BytesInUse() < baseline + kLarge, "the destructor frees the error");

	GErrorPtr out;
	gchar*    contents = nullptr;
	Check(g_file_get_contents("/nonexistent-kyty-test/file", &contents, nullptr, out.Out()) ==
	              FALSE &&
	          out,
	      "Out() receives an error");
	Check(contents == nullptr && g_error_matches(out.get(), G_FILE_ERROR, G_FILE_ERROR_NOENT),
	      "the error is kept");
	*out.Out()                = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, message.c_str());
	const std::size_t holding = BytesInUse();
	out.reset();
	Check(BytesInUse() + kLarge <= holding && !out && out.Message().empty(),
	      "reset frees the error");

	GErrorPtr owner(g_error_new_literal(G_IO_ERROR, G_IO_ERROR_EXISTS, "exists"));
	GErrorPtr target;
	target = std::move(owner);
	Check(!owner && target.Message() == "exists", "move assignment transfers the error");
	GError* released = target.release();
	Check(!target && released->code == G_IO_ERROR_EXISTS, "release gives the error away");
	g_error_free(released);
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("GobjectPtrTests");
	// A GLib critical or warning (for example an unknown source id or a bad disconnect) fails the
	// test.
	g_log_set_always_fatal(GLogLevelFlags(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING));

	TestAdoptAndReset();
	TestRefCopyAndMove();
	TestFloating();
	TestSignalScope();
	TestSourceId();
	TestGCharPtr();
	TestGErrorPtr();
	std::printf("GobjectPtrTests: all tests passed\n");
	return 0;
}
