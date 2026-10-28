#pragma once

// A small stand-in for C++23 std::expected (task 10.1). libstdc++ provides <expected> only when
// __cpp_concepts >= 202002L, which the Clang of the Ubuntu 24.04 CI job is not confirmed to report,
// so the launcher and the Updater use this type and stay on C++20. It mirrors the std::expected
// members that Launcher_Core uses: has_value(), explicit operator bool, value(), error(),
// operator*, operator->, value_or(), and ==.
//
// The project builds without exceptions, so misuse cannot throw bad_expected_access: value(),
// operator*, and operator-> on an error, and error() on a value, print a message and call
// std::abort().

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace kyty::core {

namespace detail {

[[noreturn]] inline void ExpectedMisuse(const char* what) {
	std::fprintf(stderr, "kyty::core::Expected: %s\n", what);
	std::abort();
}

} // namespace detail

// The error alternative of an Expected, as std::unexpected.
template <typename E>
class Unexpected {
public:
	static_assert(std::is_object_v<E> && !std::is_array_v<E> && !std::is_const_v<E> &&
	                  !std::is_volatile_v<E>,
	              "Unexpected<E> needs a non-const, non-volatile, non-array object type");

	constexpr explicit Unexpected(const E& error): m_error(error) {}
	constexpr explicit Unexpected(E&& error): m_error(std::move(error)) {}

	[[nodiscard]] constexpr const E& error() const& noexcept { return m_error; }
	[[nodiscard]] constexpr E&       error() & noexcept { return m_error; }
	[[nodiscard]] constexpr E&&      error() && noexcept { return std::move(m_error); }

	template <typename G>
	friend constexpr bool operator==(const Unexpected& a, const Unexpected<G>& b) {
		return a.error() == b.error();
	}

private:
	E m_error;
};

template <typename E>
Unexpected(E) -> Unexpected<E>;

// Unexpected<std::decay_t<E>>(error), for call sites that do not want to name the type.
template <typename E>
[[nodiscard]] constexpr Unexpected<std::decay_t<E>> MakeUnexpected(E&& error) {
	return Unexpected<std::decay_t<E>>(std::forward<E>(error));
}

namespace detail {

template <typename T>
struct IsUnexpected: std::false_type {};
template <typename E>
struct IsUnexpected<Unexpected<E>>: std::true_type {};

} // namespace detail

template <typename T, typename E>
class Expected;

namespace detail {

template <typename T>
struct IsExpected: std::false_type {};
template <typename T, typename E>
struct IsExpected<Expected<T, E>>: std::true_type {};

} // namespace detail

// Holds either a T value or an E error. A default-constructed Expected holds T().
template <typename T, typename E>
class Expected {
public:
	static_assert(std::is_object_v<T> && !std::is_array_v<T> && !detail::IsUnexpected<T>::value,
	              "Expected<T, E> needs an object type T that is not an Unexpected");

	using value_type      = T;
	using error_type      = E;
	using unexpected_type = Unexpected<E>;

	constexpr Expected()
	requires std::is_default_constructible_v<T>
	    : m_storage(std::in_place_index<0>) {}

	// A value, implicitly from anything convertible to T (as std::expected).
	template <typename U = T>
	requires(!std::is_same_v<std::remove_cvref_t<U>, Expected> &&
	         !std::is_same_v<std::remove_cvref_t<U>, std::in_place_t> &&
	         !detail::IsUnexpected<std::remove_cvref_t<U>>::value && std::is_constructible_v<T, U>)
	constexpr explicit(!std::is_convertible_v<U, T>) Expected(U&& value)
	    : m_storage(std::in_place_index<0>, std::forward<U>(value)) {}

	template <typename... Args>
	constexpr explicit Expected(std::in_place_t /*tag*/, Args&&... args)
	    : m_storage(std::in_place_index<0>, std::forward<Args>(args)...) {}

	template <typename G>
	requires std::is_constructible_v<E, const G&>
	constexpr explicit(!std::is_convertible_v<const G&, E>) Expected(const Unexpected<G>& error)
	    : m_storage(std::in_place_index<1>, error.error()) {}

	template <typename G>
	requires std::is_constructible_v<E, G>
	constexpr explicit(!std::is_convertible_v<G, E>) Expected(Unexpected<G>&& error)
	    : m_storage(std::in_place_index<1>, std::move(error).error()) {}

	[[nodiscard]] constexpr bool has_value() const noexcept { return m_storage.index() == 0; }
	constexpr explicit           operator bool() const noexcept { return has_value(); }

	[[nodiscard]] constexpr T& value() & {
		CheckValue("value() called on an Expected that holds an error");
		return *std::get_if<0>(&m_storage);
	}
	[[nodiscard]] constexpr const T& value() const& {
		CheckValue("value() called on an Expected that holds an error");
		return *std::get_if<0>(&m_storage);
	}
	[[nodiscard]] constexpr T&& value() && {
		CheckValue("value() called on an Expected that holds an error");
		return std::move(*std::get_if<0>(&m_storage));
	}

	[[nodiscard]] constexpr E& error() & {
		CheckError();
		return *std::get_if<1>(&m_storage);
	}
	[[nodiscard]] constexpr const E& error() const& {
		CheckError();
		return *std::get_if<1>(&m_storage);
	}
	[[nodiscard]] constexpr E&& error() && {
		CheckError();
		return std::move(*std::get_if<1>(&m_storage));
	}

	[[nodiscard]] constexpr T& operator*() & {
		CheckValue("operator* called on an Expected that holds an error");
		return *std::get_if<0>(&m_storage);
	}
	[[nodiscard]] constexpr const T& operator*() const& {
		CheckValue("operator* called on an Expected that holds an error");
		return *std::get_if<0>(&m_storage);
	}
	[[nodiscard]] constexpr T&& operator*() && {
		CheckValue("operator* called on an Expected that holds an error");
		return std::move(*std::get_if<0>(&m_storage));
	}

	[[nodiscard]] constexpr T* operator->() {
		CheckValue("operator-> called on an Expected that holds an error");
		return std::get_if<0>(&m_storage);
	}
	[[nodiscard]] constexpr const T* operator->() const {
		CheckValue("operator-> called on an Expected that holds an error");
		return std::get_if<0>(&m_storage);
	}

	template <typename U>
	[[nodiscard]] constexpr T value_or(U&& fallback) const& {
		if (has_value()) {
			return *std::get_if<0>(&m_storage);
		}
		return static_cast<T>(std::forward<U>(fallback));
	}
	template <typename U>
	[[nodiscard]] constexpr T value_or(U&& fallback) && {
		if (has_value()) {
			return std::move(*std::get_if<0>(&m_storage));
		}
		return static_cast<T>(std::forward<U>(fallback));
	}

	// Equal when both hold equal values or both hold equal errors.
	template <typename T2, typename E2>
	friend constexpr bool operator==(const Expected& a, const Expected<T2, E2>& b) {
		if (a.has_value() != b.has_value()) {
			return false;
		}
		return a.has_value() ? *a == *b : a.error() == b.error();
	}

	template <typename U>
	requires(!detail::IsExpected<U>::value && !detail::IsUnexpected<U>::value)
	friend constexpr bool operator==(const Expected& a, const U& value) {
		return a.has_value() && *a == value;
	}

	template <typename G>
	friend constexpr bool operator==(const Expected& a, const Unexpected<G>& error) {
		return !a.has_value() && a.error() == error.error();
	}

private:
	constexpr void CheckValue(const char* what) const {
		if (!has_value()) {
			detail::ExpectedMisuse(what);
		}
	}
	constexpr void CheckError() const {
		if (has_value()) {
			detail::ExpectedMisuse("error() called on an Expected that holds a value");
		}
	}

	std::variant<T, E> m_storage;
};

// Success without a value, or an E error. A default-constructed Expected<void, E> is a success.
template <typename E>
class Expected<void, E> {
public:
	using value_type      = void;
	using error_type      = E;
	using unexpected_type = Unexpected<E>;

	constexpr Expected() noexcept = default;
	constexpr explicit Expected(std::in_place_t /*tag*/) noexcept {}

	template <typename G>
	requires std::is_constructible_v<E, const G&>
	constexpr explicit(!std::is_convertible_v<const G&, E>) Expected(const Unexpected<G>& error)
	    : m_error(std::in_place, error.error()) {}

	template <typename G>
	requires std::is_constructible_v<E, G>
	constexpr explicit(!std::is_convertible_v<G, E>) Expected(Unexpected<G>&& error)
	    : m_error(std::in_place, std::move(error).error()) {}

	[[nodiscard]] constexpr bool has_value() const noexcept { return !m_error.has_value(); }
	constexpr explicit           operator bool() const noexcept { return has_value(); }

	constexpr void value() const {
		if (!has_value()) {
			detail::ExpectedMisuse("value() called on an Expected that holds an error");
		}
	}
	constexpr void operator*() const {
		if (!has_value()) {
			detail::ExpectedMisuse("operator* called on an Expected that holds an error");
		}
	}

	[[nodiscard]] constexpr E& error() & {
		CheckError();
		return *m_error;
	}
	[[nodiscard]] constexpr const E& error() const& {
		CheckError();
		return *m_error;
	}
	[[nodiscard]] constexpr E&& error() && {
		CheckError();
		return std::move(*m_error);
	}

	template <typename E2>
	friend constexpr bool operator==(const Expected& a, const Expected<void, E2>& b) {
		if (a.has_value() != b.has_value()) {
			return false;
		}
		return a.has_value() || a.error() == b.error();
	}

	template <typename G>
	friend constexpr bool operator==(const Expected& a, const Unexpected<G>& error) {
		return !a.has_value() && a.error() == error.error();
	}

private:
	constexpr void CheckError() const {
		if (has_value()) {
			detail::ExpectedMisuse("error() called on an Expected that holds a value");
		}
	}

	std::optional<E> m_error;
};

} // namespace kyty::core
