# ui_harness_selftest (task 22.6; Requirements 16.6, 17.11; phase 7 plan D38): the UI test harness
# fails a case on a deliberate critical or warning and prints the message, passes a declared
# expected warning, fails when a declared warning never arrives, passes a clean run, fails a case
# that reads an accessible property of no supported type (phase 8 plan D17), and tolerates GTK
# 4.24's frame-timings warning only as its exact text from Gdk at warning level on GTK 4.24 (the
# frame-timings-* kinds inject the GTK version; docs/testing.md), on every
# backend inside tests/ui/with-display.sh ("launcher_ui_tests --selftest <kind>"); the case
# harness_locale passes with the caller's LC_MESSAGES, LC_NUMERIC, and LC_TIME set (p7-u2-1); and
# the wrapper exits 77 when the display server executable it is told to use does not exist. Run as
#   cmake -DBASH=<bash> -DWRAPPER=<tests/ui/with-display.sh> -DPORTAL=<ui_fake_portal>
#         -DEXE=<launcher_ui_tests> -DWORK_DIR=<scratch directory> -P UiHarnessSelfTest.cmake
# The exit status is 0 when every check passed, 1 when one failed, and 77 (the ui label's skip code)
# only when the checks that ran passed but this host lacks dbus-daemon, weston, or Xvfb for a
# backend (with-display.sh's own 77). Exiting 77 needs CMake 3.29 or newer; an older CMake fails
# instead, which CI treats the same way (Requirement 16.6). WORK_DIR is removed when the test ends.
cmake_minimum_required(VERSION 3.22)

foreach(kyty_variable IN ITEMS BASH WRAPPER PORTAL EXE WORK_DIR)
	if(NOT DEFINED ${kyty_variable} OR "${${kyty_variable}}" STREQUAL "")
		message(FATAL_ERROR "UiHarnessSelfTest: ${kyty_variable} is not set")
	endif()
endforeach()
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
# Only the server overrides that this script sets below apply.
foreach(kyty_variable IN ITEMS KYTY_UI_DBUS_DAEMON KYTY_UI_WESTON KYTY_UI_XVFB)
	unset(ENV{${kyty_variable}})
endforeach()

set(kyty_failures "")
set(kyty_missing_backends "")

# Runs "launcher_ui_tests <arguments>" (--selftest <kind>, or --case <name>) inside with-display.sh
# for backend and sets kyty_result and kyty_output (stdout and stderr, merged in order) in the
# caller.
function(kyty_selftest_run backend)
	execute_process(
		COMMAND "${BASH}" "${WRAPPER}" --portal "${PORTAL}" ${backend}
		        -- "${EXE}" ${ARGN}
		WORKING_DIRECTORY "${WORK_DIR}"
		INPUT_FILE /dev/null
		RESULT_VARIABLE kyty_run_result
		OUTPUT_VARIABLE kyty_run_output
		ERROR_VARIABLE kyty_run_output
		TIMEOUT 60)
	list(JOIN ARGN " " kyty_run_arguments)
	message(STATUS "UiHarnessSelfTest: ${backend} ${kyty_run_arguments}: "
		"exit '${kyty_run_result}'\n${kyty_run_output}")
	set(kyty_result "${kyty_run_result}" PARENT_SCOPE)
	set(kyty_output "${kyty_run_output}" PARENT_SCOPE)
endfunction()

# Appends a failure unless output contains text.
function(kyty_expect_text what output text)
	string(FIND "${output}" "${text}" kyty_position)
	if(kyty_position LESS 0)
		string(APPEND kyty_failures "\n  ${what}: the output lacks \"${text}\"")
		set(kyty_failures "${kyty_failures}" PARENT_SCOPE)
	endif()
endfunction()

function(kyty_expect_no_text what output text)
	string(FIND "${output}" "${text}" kyty_position)
	if(kyty_position GREATER_EQUAL 0)
		string(APPEND kyty_failures "\n  ${what}: the output contains \"${text}\"")
		set(kyty_failures "${kyty_failures}" PARENT_SCOPE)
	endif()
endfunction()

function(kyty_expect_exit what result expected)
	if(NOT result STREQUAL expected)
		string(APPEND kyty_failures "\n  ${what}: exit '${result}', expected ${expected}")
		set(kyty_failures "${kyty_failures}" PARENT_SCOPE)
	endif()
endfunction()

# Each kind's exit status and the line the harness prints for it (after "selftest-<kind>: ").
set(kyty_declared "ui_harness_selftest: a declared warning")
set(kyty_exit_critical 1)
set(kyty_text_critical "FAILED: CRITICAL from Gtk: gtk_widget_set_visible: assertion")
set(kyty_exit_warning 1)
string(CONCAT kyty_text_warning "FAILED: WARNING from kyty-launcher: "
	"ui_harness_selftest: a deliberate warning")
set(kyty_exit_expected-warning 0)
set(kyty_text_expected-warning "expected WARNING from kyty-launcher: ${kyty_declared} (ExpectLog)")
set(kyty_exit_missing-expected 1)
string(CONCAT kyty_text_missing-expected "FAILED: the expected WARNING from kyty-launcher "
	"\"${kyty_declared}...\" arrived 0 time(s), not once")
set(kyty_exit_clean 0)
set(kyty_text_clean "PASS")
set(kyty_exit_accessible-untyped 1)
string(CONCAT kyty_text_accessible-untyped "FAILED: AccessibleProperty: the accessible property 31 "
	"has no boolean, integer, number, string, or token value")
# GTK 4.24's frame-timings warning (docs/testing.md): tolerated, with a note, only as the exact
# text from Gdk at warning level while GTK 4.24.x runs; the kinds inject the GTK version.
set(kyty_frame "gdk_frame_timings_submitted() called on submitted frame.")
set(kyty_exit_frame-timings-gtk-4-24 0)
string(CONCAT kyty_text_frame-timings-gtk-4-24 "NOTE: tolerated on GTK 4.24 (a GTK 4.24 bug; "
	"docs/testing.md): WARNING from Gdk: ${kyty_frame}")
set(kyty_exit_frame-timings-gtk-4-22 1)
set(kyty_text_frame-timings-gtk-4-22 "FAILED: WARNING from Gdk: ${kyty_frame}")
set(kyty_exit_frame-timings-near-miss 1)
string(CONCAT kyty_text_frame-timings-near-miss "FAILED: WARNING from Gdk: "
	"gdk_frame_timings_submitted() called on discarded frame.")
set(kyty_exit_frame-timings-critical 1)
set(kyty_text_frame-timings-critical "FAILED: CRITICAL from Gdk: ${kyty_frame}")
set(kyty_exit_frame-timings-other-domain 1)
set(kyty_text_frame-timings-other-domain "FAILED: WARNING from Gtk: ${kyty_frame}")
set(kyty_kinds critical warning expected-warning missing-expected clean accessible-untyped
	frame-timings-gtk-4-24 frame-timings-gtk-4-22 frame-timings-near-miss frame-timings-critical
	frame-timings-other-domain)
list(LENGTH kyty_kinds kyty_kind_count)

foreach(kyty_backend IN ITEMS wayland x11)
	foreach(kyty_kind IN LISTS kyty_kinds)
		set(kyty_expected_exit ${kyty_exit_${kyty_kind}})
		set(kyty_expected_text "selftest-${kyty_kind}: ${kyty_text_${kyty_kind}}")
		kyty_selftest_run(${kyty_backend} --selftest ${kyty_kind})
		if(kyty_result STREQUAL "77")
			# with-display.sh found no server executable: the remaining kinds cannot run either.
			list(APPEND kyty_missing_backends ${kyty_backend})
			break()
		endif()
		set(kyty_what "${kyty_backend} ${kyty_kind}")
		kyty_expect_exit("${kyty_what}" "${kyty_result}" ${kyty_expected_exit})
		kyty_expect_text("${kyty_what}" "${kyty_output}" "${kyty_expected_text}")
		kyty_expect_text("${kyty_what}" "${kyty_output}"
			", GDK_BACKEND ${kyty_backend}, GTK_A11Y test)")
		if(kyty_expected_exit STREQUAL "0")
			kyty_expect_no_text("${kyty_what}" "${kyty_output}" "FAILED")
			kyty_expect_text("${kyty_what}" "${kyty_output}" "selftest-${kyty_kind}: PASS")
		else()
			kyty_expect_text("${kyty_what}" "${kyty_output}"
				"selftest-${kyty_kind}: FAIL (1 failure(s))")
		endif()
		if(kyty_kind MATCHES "^frame-timings-" AND NOT kyty_kind STREQUAL "frame-timings-gtk-4-24")
			kyty_expect_no_text("${kyty_what}" "${kyty_output}" "NOTE: tolerated")
		endif()
	endforeach()
endforeach()

# The caller's locale categories never reach the session (with-display.sh unsets every LC_*
# variable; phase 8 plan D17, p7-u2-1): the case harness_locale passes with LC_MESSAGES,
# LC_NUMERIC, and LC_TIME set here, on the first backend that this host has.
set(kyty_locale_backend "")
foreach(kyty_backend IN ITEMS wayland x11)
	if(NOT kyty_backend IN_LIST kyty_missing_backends)
		set(kyty_locale_backend ${kyty_backend})
		break()
	endif()
endforeach()
if(kyty_locale_backend)
	set(kyty_locale_variables LC_MESSAGES LC_NUMERIC LC_TIME)
	foreach(kyty_variable IN LISTS kyty_locale_variables)
		set(ENV{${kyty_variable}} "de_DE.UTF-8")
	endforeach()
	kyty_selftest_run(${kyty_locale_backend} --case harness_locale)
	foreach(kyty_variable IN LISTS kyty_locale_variables)
		unset(ENV{${kyty_variable}})
	endforeach()
	set(kyty_what "${kyty_locale_backend} harness_locale with LC_MESSAGES, LC_NUMERIC, LC_TIME set")
	kyty_expect_exit("${kyty_what}" "${kyty_result}" 0)
	kyty_expect_text("${kyty_what}" "${kyty_output}"
		"harness_locale: LANG C.UTF-8, leaked variables: none")
	kyty_expect_text("${kyty_what}" "${kyty_output}" "harness_locale: PASS")
	kyty_expect_no_text("${kyty_what}" "${kyty_output}" "FAILED")
endif()

# A display server executable that does not exist: with-display.sh exits 77 before anything runs.
# A backend whose real runs already exited 77 is left out: with-display.sh would stop at the
# executable that this host lacks (dbus-daemon, which it checks first, or the server) instead.
foreach(kyty_server IN ITEMS "wayland|KYTY_UI_WESTON|weston" "x11|KYTY_UI_XVFB|Xvfb")
	string(REPLACE "|" ";" kyty_fields "${kyty_server}")
	list(GET kyty_fields 0 kyty_backend)
	list(GET kyty_fields 1 kyty_variable)
	list(GET kyty_fields 2 kyty_name)
	if(kyty_backend IN_LIST kyty_missing_backends)
		continue()
	endif()
	set(kyty_absent "${WORK_DIR}/no-such-${kyty_name}")
	set(ENV{${kyty_variable}} "${kyty_absent}")
	kyty_selftest_run(${kyty_backend} --selftest clean)
	unset(ENV{${kyty_variable}})
	set(kyty_what "${kyty_backend} with ${kyty_variable}=${kyty_absent}")
	kyty_expect_exit("${kyty_what}" "${kyty_result}" 77)
	kyty_expect_text("${kyty_what}" "${kyty_output}"
		"with-display.sh: ${kyty_name} not found (${kyty_absent}); skipping with exit 77")
	kyty_expect_no_text("${kyty_what}" "${kyty_output}" "selftest-clean: start")
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
if(kyty_failures)
	message(FATAL_ERROR "UiHarnessSelfTest: FAILED:${kyty_failures}")
endif()
if(kyty_missing_backends)
	list(JOIN kyty_missing_backends ", " kyty_missing_text)
	string(CONCAT kyty_skip_text "UiHarnessSelfTest: no display server for ${kyty_missing_text} "
		"on this host (with-display.sh exited 77); the other checks passed")
	if(CMAKE_VERSION VERSION_LESS 3.29)
		message(FATAL_ERROR "${kyty_skip_text}; CMake ${CMAKE_VERSION} cannot exit 77, so this "
			"is a failure")
	endif()
	message(STATUS "${kyty_skip_text}; skipping with exit 77")
	cmake_language(EXIT 77)
endif()
message(STATUS "UiHarnessSelfTest: PASS (${kyty_kind_count} kinds on wayland and x11, "
	"harness_locale with the caller's LC_* set; missing servers give 77)")
