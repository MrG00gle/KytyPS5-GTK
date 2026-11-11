#pragma once

// RAII helpers of the GTK launcher (task 19.2; plan section 1, "GTK layer rules"): every GObject
// reference is owned by a GObjectPtr, every signal connection by a SignalScope, every GLib source
// by a SourceId, and every GError and g_malloc'ed string by a GErrorPtr or GCharPtr.

#include <glib-object.h>
#include <glib.h>
#include <string_view>
#include <utility>

namespace kyty::gtk {

// One strong reference to a GObject (T is the instance struct, for example GtkWindow or GFile).
// Copies take another reference; moves transfer it; the destructor drops it.
template <typename T>
class GObjectPtr {
public:
	GObjectPtr() = default;

	// Takes over the caller's reference (transfer full). A floating reference (a new widget) is
	// sunk, so it becomes this pointer's reference.
	static GObjectPtr Adopt(T* object) {
		GObjectPtr result;
		if (object != nullptr && g_object_is_floating(object)) {
			g_object_ref_sink(object);
		}
		result.m_object = object;
		return result;
	}

	// Adds a reference of its own (transfer none). A floating reference is sunk as well.
	static GObjectPtr Ref(T* object) {
		GObjectPtr result;
		if (object != nullptr) {
			g_object_ref_sink(object);
		}
		result.m_object = object;
		return result;
	}

	GObjectPtr(const GObjectPtr& other): m_object(other.m_object) {
		if (m_object != nullptr) {
			g_object_ref(m_object);
		}
	}
	GObjectPtr(GObjectPtr&& other) noexcept: m_object(std::exchange(other.m_object, nullptr)) {}
	GObjectPtr& operator=(const GObjectPtr& other) {
		if (this != &other) {
			GObjectPtr copy(other);
			std::swap(m_object, copy.m_object);
		}
		return *this;
	}
	GObjectPtr& operator=(GObjectPtr&& other) noexcept {
		if (this != &other) {
			reset();
			m_object = std::exchange(other.m_object, nullptr);
		}
		return *this;
	}
	~GObjectPtr() { reset(); }

	[[nodiscard]] T* get() const { return m_object; }
	T*               operator->() const { return m_object; }
	explicit         operator bool() const { return m_object != nullptr; }

	// Gives the reference to the caller (transfer full); this pointer is empty afterwards.
	[[nodiscard]] T* release() { return std::exchange(m_object, nullptr); }

	// Drops the reference.
	void reset() {
		if (T* object = std::exchange(m_object, nullptr); object != nullptr) {
			g_object_unref(object);
		}
	}

	friend bool operator==(const GObjectPtr& a, const GObjectPtr& b) {
		return a.m_object == b.m_object;
	}

private:
	T* m_object = nullptr;
};

// One signal connection. The destructor disconnects it unless the instance is gone already (a
// weak reference tracks it), so a scope may outlive the object it connected to.
class SignalScope {
public:
	SignalScope() = default;

	// g_signal_connect_data(instance, signal, callback, data, nullptr, flags).
	static SignalScope Connect(gpointer instance, const char* signal, GCallback callback,
	                           gpointer data, GConnectFlags flags = GConnectFlags(0)) {
		return Adopt(instance,
		             g_signal_connect_data(instance, signal, callback, data, nullptr, flags));
	}

	// Takes over the connection handler_id of instance.
	static SignalScope Adopt(gpointer instance, gulong handler_id) {
		SignalScope scope;
		if (instance != nullptr && handler_id != 0) {
			g_weak_ref_init(&scope.m_instance, instance);
			scope.m_handler_id = handler_id;
		}
		return scope;
	}

	SignalScope(const SignalScope&)            = delete;
	SignalScope& operator=(const SignalScope&) = delete;
	SignalScope(SignalScope&& other) noexcept { MoveFrom(other); }
	SignalScope& operator=(SignalScope&& other) noexcept {
		if (this != &other) {
			Disconnect();
			MoveFrom(other);
		}
		return *this;
	}
	~SignalScope() { Disconnect(); }

	[[nodiscard]] bool Connected() const { return m_handler_id != 0; }

	// Disconnects now; afterwards the scope is empty.
	void Disconnect() {
		if (m_handler_id == 0) {
			return;
		}
		if (gpointer instance = g_weak_ref_get(&m_instance); instance != nullptr) {
			g_signal_handler_disconnect(instance, m_handler_id);
			g_object_unref(instance);
		}
		g_weak_ref_clear(&m_instance);
		m_handler_id = 0;
	}

private:
	// A GWeakRef is registered by its address, so it is re-initialized instead of copied.
	void MoveFrom(SignalScope& other) {
		if (other.m_handler_id == 0) {
			return;
		}
		gpointer instance = g_weak_ref_get(&other.m_instance);
		g_weak_ref_init(&m_instance, instance);
		if (instance != nullptr) {
			g_object_unref(instance);
		}
		m_handler_id = std::exchange(other.m_handler_id, 0);
		g_weak_ref_clear(&other.m_instance);
	}

	GWeakRef m_instance {};
	gulong   m_handler_id = 0;
};

// One GLib source of the default main context (g_timeout_add, g_idle_add, g_unix_fd_add, ...). The
// scope keeps a reference to the GSource, so the destructor removes it safely even when it has
// already ended (its callback returned G_SOURCE_REMOVE).
class SourceId {
public:
	SourceId() = default;

	// Takes over the source with this id; 0 gives an empty scope.
	explicit SourceId(guint id) {
		if (id != 0) {
			m_source = g_main_context_find_source_by_id(nullptr, id);
			if (m_source != nullptr) {
				g_source_ref(m_source);
			}
		}
	}

	SourceId(const SourceId&)            = delete;
	SourceId& operator=(const SourceId&) = delete;
	SourceId(SourceId&& other) noexcept: m_source(std::exchange(other.m_source, nullptr)) {}
	SourceId& operator=(SourceId&& other) noexcept {
		if (this != &other) {
			Remove();
			m_source = std::exchange(other.m_source, nullptr);
		}
		return *this;
	}
	~SourceId() { Remove(); }

	// True while the source exists and has not ended or been removed.
	[[nodiscard]] bool Active() const {
		return m_source != nullptr && !g_source_is_destroyed(m_source);
	}

	// Removes the source (its callback no longer runs) and empties the scope.
	void Remove() {
		if (GSource* source = std::exchange(m_source, nullptr); source != nullptr) {
			g_source_destroy(source);
			g_source_unref(source);
		}
	}

private:
	GSource* m_source = nullptr;
};

// One GError, freed with g_error_free.
class GErrorPtr {
public:
	GErrorPtr() = default;
	explicit GErrorPtr(GError* error): m_error(error) {}
	GErrorPtr(const GErrorPtr&)            = delete;
	GErrorPtr& operator=(const GErrorPtr&) = delete;
	GErrorPtr(GErrorPtr&& other) noexcept: m_error(std::exchange(other.m_error, nullptr)) {}
	GErrorPtr& operator=(GErrorPtr&& other) noexcept {
		if (this != &other) {
			reset(std::exchange(other.m_error, nullptr));
		}
		return *this;
	}
	~GErrorPtr() { reset(); }

	[[nodiscard]] GError* get() const { return m_error; }
	GError*               operator->() const { return m_error; }
	explicit              operator bool() const { return m_error != nullptr; }

	// The message, or "" without an error.
	[[nodiscard]] std::string_view Message() const {
		if (m_error == nullptr) {
			return "";
		}
		const char* message = m_error->message; // core text: GError's message (library text)
		return message != nullptr ? message : "";
	}

	// For a GError** out parameter: frees the current error first.
	[[nodiscard]] GError** Out() {
		reset();
		return &m_error;
	}

	[[nodiscard]] GError* release() { return std::exchange(m_error, nullptr); }

	void reset(GError* error = nullptr) {
		if (GError* old = std::exchange(m_error, error); old != nullptr) {
			g_error_free(old);
		}
	}

private:
	GError* m_error = nullptr;
};

// One string allocated by GLib, freed with g_free.
class GCharPtr {
public:
	GCharPtr() = default;
	explicit GCharPtr(gchar* text): m_text(text) {}
	GCharPtr(const GCharPtr&)            = delete;
	GCharPtr& operator=(const GCharPtr&) = delete;
	GCharPtr(GCharPtr&& other) noexcept: m_text(std::exchange(other.m_text, nullptr)) {}
	GCharPtr& operator=(GCharPtr&& other) noexcept {
		if (this != &other) {
			reset(std::exchange(other.m_text, nullptr));
		}
		return *this;
	}
	~GCharPtr() { reset(); }

	[[nodiscard]] gchar* get() const { return m_text; }
	explicit             operator bool() const { return m_text != nullptr; }

	// The text, or "" for a null string.
	[[nodiscard]] std::string_view View() const { return m_text != nullptr ? m_text : ""; }

	// For a gchar** out parameter: frees the current string first.
	[[nodiscard]] gchar** Out() {
		reset();
		return &m_text;
	}

	[[nodiscard]] gchar* release() { return std::exchange(m_text, nullptr); }

	void reset(gchar* text = nullptr) {
		if (gchar* old = std::exchange(m_text, text); old != nullptr) {
			g_free(old);
		}
	}

private:
	gchar* m_text = nullptr;
};

} // namespace kyty::gtk
