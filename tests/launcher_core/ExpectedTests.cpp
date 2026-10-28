// Example tests of kyty::core::Expected (src/launcher/core/expected.h), the in-repo stand-in for
// std::expected (task 10.1).

#include "launcher/core/expected.h"
#include "support/noDisplay.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace {

using kyty::core::Expected;
using kyty::core::MakeUnexpected;
using kyty::core::Unexpected;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "ExpectedTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

// Runs fn in a child process (stderr discarded) and reports whether it ended with SIGABRT.
template <typename Fn>
bool Aborts(Fn fn) {
	std::fflush(nullptr);
	const pid_t pid = fork();
	Check(pid >= 0, "fork");
	if (pid == 0) {
		const int null_fd = open("/dev/null", O_WRONLY);
		if (null_fd >= 0) {
			dup2(null_fd, STDERR_FILENO);
		}
		fn();
		_exit(0);
	}
	int status = 0;
	Check(waitpid(pid, &status, 0) == pid, "waitpid");
	return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

struct NoDefault {
	explicit NoDefault(int v): value(v) {}
	int  value;
	bool operator==(const NoDefault& other) const { return value == other.value; }
};

// Constant evaluation works as for std::expected.
static_assert(Expected<int, int>(3).value_or(0) == 3);
static_assert(!Expected<int, int>(Unexpected<int>(1)).has_value());
static_assert(Expected<int, int>(Unexpected<int>(1)).value_or(5) == 5);
static_assert(Expected<void, int>().has_value());

void TestValue() {
	Expected<int, std::string> value = 5;
	Check(value.has_value() && static_cast<bool>(value), "a value is has_value()");
	Check(value.value() == 5 && *value == 5, "value() and operator*");
	Check(value.value_or(7) == 5, "value_or() of a value");
	value.value() = 6;
	Check(*value == 6, "value() is a reference");
	*value = 8;
	Check(value.value() == 8, "operator* is a reference");

	const Expected<int, std::string> defaulted;
	Check(defaulted.has_value() && *defaulted == 0, "a default-constructed Expected holds T()");

	Expected<std::string, int> text = "abc";
	Check(text.has_value() && *text == "abc", "implicit conversion from a convertible value");
	Check(text->size() == 3, "operator->");
	const auto& const_text = text;
	Check(const_text->size() == 3 && const_text.value() == "abc" && *const_text == "abc",
	      "const accessors");

	const Expected<NoDefault, int> in_place(std::in_place, 4);
	Check(in_place.value().value == 4, "in_place construction");
	const Expected<NoDefault, int> explicit_value {NoDefault(2)};
	Check(explicit_value->value == 2, "construction from T");
}

void TestError() {
	Expected<int, std::string> error = MakeUnexpected(std::string("bad"));
	Check(!error.has_value() && !static_cast<bool>(error), "an error is not has_value()");
	Check(error.error() == "bad", "error()");
	Check(error.value_or(7) == 7, "value_or() of an error");
	error.error() += "!";
	Check(error.error() == "bad!", "error() is a reference");

	const Expected<int, std::string> from_unexpected = Unexpected<std::string>("lvalue");
	Check(from_unexpected.error() == "lvalue", "construction from an Unexpected");
	const Unexpected<std::string>    lvalue("copy");
	const Expected<int, std::string> copied = lvalue;
	Check(copied.error() == "copy" && lvalue.error() == "copy",
	      "construction from an lvalue Unexpected");

	// An error type convertible from the Unexpected's type.
	const Expected<int, std::string> converted = MakeUnexpected("text");
	Check(converted.error() == "text", "construction from a convertible Unexpected");

	// The same type for value and error.
	const Expected<std::string, std::string> same_value = std::string("a");
	const Expected<std::string, std::string> same_error = MakeUnexpected(std::string("a"));
	Check(same_value.has_value() && !same_error.has_value(), "T and E of the same type");
	Check(!(same_value == same_error), "a value never equals an error");

	Unexpected<std::string> unexpected("moved");
	Check(unexpected.error() == "moved", "Unexpected::error()");
	unexpected.error() += "!";
	const std::string taken = std::move(unexpected).error();
	Check(taken == "moved!", "Unexpected::error() &&");
}

void TestVoid() {
	Expected<void, std::string> ok;
	Check(ok.has_value() && static_cast<bool>(ok), "a default-constructed Expected<void> succeeds");
	ok.value();
	*ok;
	const Expected<void, std::string> in_place {std::in_place};
	Check(in_place.has_value(), "in_place Expected<void>");

	Expected<void, std::string> bad = MakeUnexpected(std::string("x"));
	Check(!bad.has_value() && bad.error() == "x", "an Expected<void> error");
	bad.error() += "y";
	Check(std::as_const(bad).error() == "xy", "Expected<void>::error() is a reference");
	const std::string moved = std::move(bad).error();
	Check(moved == "xy", "Expected<void>::error() &&");

	const Unexpected<std::string>     lvalue("l");
	const Expected<void, std::string> copied = lvalue;
	Check(copied.error() == "l", "Expected<void> from an lvalue Unexpected");

	const Expected<void, std::string> other_ok;
	const Expected<void, std::string> other_bad = MakeUnexpected(std::string("z"));
	Check(ok == other_ok, "two successes are equal");
	Check(!(ok == other_bad) && !(other_bad == ok), "a success never equals an error");
	Check(other_bad == Unexpected<std::string>("z"), "== with an Unexpected");
	Check(!(other_bad == Unexpected<std::string>("w")), "!= with another Unexpected");
	Check(!(ok == Unexpected<std::string>("z")), "a success is not an Unexpected");
	const Expected<void, std::string> same_bad = MakeUnexpected(std::string("z"));
	Check(other_bad == same_bad, "equal errors are equal");
}

void TestMoves() {
	Expected<std::unique_ptr<int>, std::string> pointer = std::make_unique<int>(4);
	auto                                        moved   = std::move(pointer);
	Check(moved.has_value() && **moved == 4, "move construction keeps the value");
	std::unique_ptr<int> taken = std::move(moved).value();
	Check(taken && *taken == 4, "value() && moves the value out");

	Expected<std::unique_ptr<int>, std::string> star      = std::make_unique<int>(5);
	std::unique_ptr<int>                        from_star = *std::move(star);
	Check(from_star && *from_star == 5, "operator* && moves the value out");

	Expected<std::string, int> text      = std::string(100, 'x');
	const std::string          text_copy = std::move(text).value_or("fallback");
	Check(text_copy == std::string(100, 'x'), "value_or() && of a value");
	Expected<std::string, int> failed = MakeUnexpected(1);
	Check(std::move(failed).value_or("fallback") == "fallback", "value_or() && of an error");

	Expected<int, std::string> error   = MakeUnexpected(std::string(50, 'e'));
	const std::string          message = std::move(error).error();
	Check(message == std::string(50, 'e'), "error() && moves the error out");

	Expected<int, std::string> assigned = 1;
	assigned                            = MakeUnexpected(std::string("now an error"));
	Check(!assigned.has_value() && assigned.error() == "now an error", "assignment of an error");
	assigned = 9;
	Check(assigned.has_value() && *assigned == 9, "assignment of a value");
	Expected<int, std::string> copy = assigned;
	Check(copy == assigned, "copy construction");
}

void TestEquality() {
	const Expected<int, std::string> three = 3;
	const Expected<int, std::string> also_three(3);
	const Expected<int, std::string> four  = 4;
	const Expected<int, std::string> error = MakeUnexpected(std::string("e"));
	const Expected<int, std::string> other = MakeUnexpected(std::string("f"));
	Check(three == also_three && !(three == four), "values compare by value");
	Check(!(three == error) && !(error == three), "a value never equals an error");
	Check(error == Expected<int, std::string>(MakeUnexpected(std::string("e"))) &&
	          !(error == other),
	      "errors compare by error");
	Check(three == 3 && 3 == three && !(three == 4) && !(error == 3), "== with a value");
	Check(error == Unexpected<std::string>("e") && !(three == Unexpected<std::string>("e")),
	      "== with an Unexpected");
	Check(three == Expected<long, std::string>(3L), "== across value types");
	Check(three != four && error != other, "!= is the negation of ==");
	Check(Unexpected<int>(1) == Unexpected<long>(1L) && !(Unexpected<int>(1) == Unexpected<int>(2)),
	      "Unexpected ==");
}

void TestMisuseAborts() {
	Check(Aborts([] {
		      const Expected<int, std::string> error = MakeUnexpected(std::string("e"));
		      std::printf("%d\n", error.value());
	      }),
	      "value() on an error aborts");
	Check(Aborts([] {
		      Expected<int, std::string> error = MakeUnexpected(std::string("e"));
		      std::printf("%d\n", *error);
	      }),
	      "operator* on an error aborts");
	Check(Aborts([] {
		      Expected<std::string, int> error = MakeUnexpected(1);
		      std::printf("%zu\n", error->size());
	      }),
	      "operator-> on an error aborts");
	Check(Aborts([] {
		      const Expected<int, std::string> value = 1;
		      std::printf("%s\n", value.error().c_str());
	      }),
	      "error() on a value aborts");
	Check(Aborts([] {
		      const Expected<void, std::string> error = MakeUnexpected(std::string("e"));
		      error.value();
	      }),
	      "Expected<void>::value() on an error aborts");
	Check(Aborts([] {
		      const Expected<void, std::string> error = MakeUnexpected(std::string("e"));
		      *error;
	      }),
	      "Expected<void>::operator* on an error aborts");
	Check(Aborts([] {
		      const Expected<void, std::string> ok;
		      std::printf("%s\n", ok.error().c_str());
	      }),
	      "Expected<void>::error() on a success aborts");
	Check(!Aborts([] {
		const Expected<int, std::string> value = 1;
		std::printf("%d\n", value.value());
	}),
	      "value() on a value does not abort");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("ExpectedTests");
	TestValue();
	TestError();
	TestVoid();
	TestMoves();
	TestEquality();
	TestMisuseAborts();
	std::printf("ExpectedTests: all tests passed\n");
	return 0;
}
