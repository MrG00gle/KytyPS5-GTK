# argv_echo_selftest and fake_emulator_selftest (task 15.6, Requirement 17.4): run a process test
# helper (tests/helpers/) with execute_process and check its exact output bytes (file(READ ... HEX)),
# its exit codes, and its signal deaths.
#   CASE=argv_echo: arguments with a space, an empty string, quotes, a backslash, a newline, shell
#   metacharacters, and non-ASCII UTF-8 come back NUL-terminated, byte for byte.
#   CASE=fake_emulator: every option of tests/helpers/FakeEmulator.cpp.
#
#   cmake -DCASE=argv_echo|fake_emulator -DEXE=<helper> -DWORK_DIR=<scratch directory>
#         -P tests/cmake/ProcessHelperSelfTest.cmake
#
# WORK_DIR is recreated at the start and removed when every check passes.
cmake_minimum_required(VERSION 3.19)

if(NOT CASE MATCHES "^(argv_echo|fake_emulator)$")
	message(FATAL_ERROR "ProcessHelperSelfTest: -DCASE=argv_echo|fake_emulator is required")
endif()
if(NOT EXE OR NOT EXISTS "${EXE}")
	message(FATAL_ERROR "ProcessHelperSelfTest: -DEXE=<helper executable> is required")
endif()
if(NOT WORK_DIR)
	message(FATAL_ERROR "ProcessHelperSelfTest: -DWORK_DIR=<scratch directory> is required")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(kyty_failures "")
set(kyty_run_count 0)

# Runs EXE with the arguments that follow the prefix (stdin /dev/null, cwd WORK_DIR) and sets
# <prefix>_result, <prefix>_out (stdout as lower-case hex), <prefix>_err (stderr as hex), and
# <prefix>_out_file. Each argument is passed as one bracket argument through cmake_language(EVAL),
# because expanding a list would drop an empty argument and split one at its semicolons.
function(kyty_run prefix)
	math(EXPR kyty_count "${kyty_run_count} + 1")
	set(kyty_run_count ${kyty_count} PARENT_SCOPE)
	set(kyty_out_file "${WORK_DIR}/run${kyty_count}.out")
	set(kyty_err_file "${WORK_DIR}/run${kyty_count}.err")
	set(kyty_code "execute_process(COMMAND [==[${EXE}]==]")
	if(ARGC GREATER 1)
		math(EXPR kyty_last "${ARGC} - 1")
		foreach(kyty_index RANGE 1 ${kyty_last})
			string(APPEND kyty_code " [==[${ARGV${kyty_index}}]==]")
		endforeach()
	endif()
	string(APPEND kyty_code " WORKING_DIRECTORY [==[${WORK_DIR}]==] INPUT_FILE /dev/null"
		" OUTPUT_FILE [==[${kyty_out_file}]==] ERROR_FILE [==[${kyty_err_file}]==]"
		" RESULT_VARIABLE kyty_result TIMEOUT 30)")
	cmake_language(EVAL CODE "${kyty_code}")
	file(READ "${kyty_out_file}" kyty_out HEX)
	file(READ "${kyty_err_file}" kyty_err HEX)
	message(STATUS "ProcessHelperSelfTest: ${prefix}: exit '${kyty_result}', stdout '${kyty_out}', "
		"stderr '${kyty_err}'")
	set(${prefix}_result "${kyty_result}" PARENT_SCOPE)
	set(${prefix}_out "${kyty_out}" PARENT_SCOPE)
	set(${prefix}_err "${kyty_err}" PARENT_SCOPE)
	set(${prefix}_out_file "${kyty_out_file}" PARENT_SCOPE)
endfunction()

# Sets <out> to the hex of every argument after <out>, each followed by 00 (a NUL byte).
function(kyty_nul_terminated_hex out)
	set(kyty_hex "")
	if(ARGC GREATER 1)
		math(EXPR kyty_last "${ARGC} - 1")
		foreach(kyty_index RANGE 1 ${kyty_last})
			string(HEX "${ARGV${kyty_index}}" kyty_argument_hex)
			string(APPEND kyty_hex "${kyty_argument_hex}00")
		endforeach()
	endif()
	set(${out} "${kyty_hex}" PARENT_SCOPE)
endfunction()

function(kyty_text_hex out text)
	string(HEX "${text}" kyty_hex)
	set(${out} "${kyty_hex}" PARENT_SCOPE)
endfunction()

function(kyty_expect what actual expected)
	if(NOT actual STREQUAL expected)
		list(APPEND kyty_failures "${what}: got '${actual}', expected '${expected}'")
		set(kyty_failures "${kyty_failures}" PARENT_SCOPE)
	endif()
endfunction()

# A death by a signal: execute_process reports a text instead of an exit code.
function(kyty_expect_signal_death what actual)
	if(actual MATCHES "^[0-9]+$" OR actual STREQUAL "")
		list(APPEND kyty_failures "${what}: got exit '${actual}', expected a death by a signal")
		set(kyty_failures "${kyty_failures}" PARENT_SCOPE)
	endif()
endfunction()

if(CASE STREQUAL "argv_echo")
	kyty_run(none)
	kyty_expect("no arguments: exit" "${none_result}" "0")
	kyty_expect("no arguments: stdout" "${none_out}" "")

	kyty_run(empty "")
	kyty_expect("one empty argument: exit" "${empty_result}" "0")
	kyty_expect("one empty argument: stdout" "${empty_out}" "00")

	# The same arguments in both calls (the last one is a tab and UTF-8 "café 日本").
	kyty_run(mixed "with space" "" "'single' \"double\"" "back\\slash" "new\nline"
		"$ ; & | < > * ? ( ) [ ] { } ! # ~ `" "\tcafé 日本")
	kyty_nul_terminated_hex(mixed_expected "with space" "" "'single' \"double\"" "back\\slash"
		"new\nline" "$ ; & | < > * ? ( ) [ ] { } ! # ~ `" "\tcafé 日本")
	kyty_expect("mixed arguments: exit" "${mixed_result}" "0")
	kyty_expect("mixed arguments: stdout" "${mixed_out}" "${mixed_expected}")
	kyty_expect("mixed arguments: stderr" "${mixed_err}" "")
	# Spot checks that do not rely on string(HEX): "e", NUL, and the empty argument's NUL; "k\s";
	# the newline; the semicolon; U+00E9 and U+65E5.
	foreach(kyty_bytes IN ITEMS "650000" "6b5c73" "770a6c" "203b20" "c3a9" "e697a5")
		string(FIND "${mixed_out}" "${kyty_bytes}" kyty_position)
		if(kyty_position EQUAL -1)
			list(APPEND kyty_failures "mixed arguments: stdout lacks the bytes ${kyty_bytes}")
		endif()
	endforeach()
else()
	# Lines, raw bytes, and an unterminated last line, on both streams.
	kyty_run(lines --stdout hello --stderr "to stderr" --stdout-hex 80ff0a --stderr-hex fe
		--stdout-hex 6C617374 --stdout-hex "")
	kyty_text_hex(hello_hex "hello\n")
	kyty_text_hex(to_stderr_hex "to stderr\n")
	kyty_expect("lines: exit" "${lines_result}" "0")
	kyty_expect("lines: stdout" "${lines_out}" "${hello_hex}80ff0a6c617374")
	kyty_expect("lines: stderr" "${lines_err}" "${to_stderr_hex}fe")

	# Exit codes; --exit ends the processing.
	kyty_run(exit_code --stdout before --exit 7 --stdout after)
	kyty_text_hex(before_hex "before\n")
	kyty_expect("--exit 7: exit" "${exit_code_result}" "7")
	kyty_expect("--exit 7: stdout" "${exit_code_out}" "${before_hex}")
	kyty_run(exit_max --exit 255)
	kyty_expect("--exit 255: exit" "${exit_max_result}" "255")

	# Signals: a default SIGTERM ends the process; --ignore-term keeps it alive.
	kyty_run(raise_term --raise 15 --stdout unreachable)
	kyty_expect_signal_death("--raise 15" "${raise_term_result}")
	kyty_expect("--raise 15: stdout" "${raise_term_out}" "")
	# SIGUSR1 ends a process without a core dump.
	kyty_run(raise_usr1 --raise 10)
	kyty_expect_signal_death("--raise 10" "${raise_usr1_result}")
	kyty_run(ignored_term --ignore-term --raise 15 --exit 5)
	kyty_expect("--ignore-term --raise 15: exit" "${ignored_term_result}" "5")

	# --record-signals: caught signals are appended; a caught SIGTERM still ends the process unless
	# --ignore-term is given, before or after --record-signals.
	kyty_run(record --record-signals "${WORK_DIR}/record1.txt" --raise 10 --raise 1 --raise 2 --exit 3)
	kyty_expect("--record-signals: exit" "${record_result}" "3")
	file(READ "${WORK_DIR}/record1.txt" kyty_record1)
	kyty_expect("--record-signals: file" "${kyty_record1}" "10\n1\n2\n")
	kyty_run(record_term --record-signals "${WORK_DIR}/record2.txt" --raise 15 --exit 4)
	kyty_expect_signal_death("--record-signals --raise 15" "${record_term_result}")
	file(READ "${WORK_DIR}/record2.txt" kyty_record2)
	kyty_expect("--record-signals --raise 15: file" "${kyty_record2}" "15\n")
	kyty_run(record_ignored --ignore-term --record-signals "${WORK_DIR}/record3.txt" --raise 15 --exit 4)
	kyty_expect("--ignore-term --record-signals --raise 15: exit" "${record_ignored_result}" "4")
	file(READ "${WORK_DIR}/record3.txt" kyty_record3)
	kyty_expect("--ignore-term --record-signals --raise 15: file" "${kyty_record3}" "15\n")
	kyty_run(record_ignored_after --record-signals "${WORK_DIR}/record4.txt" --ignore-term --raise 15
		--exit 6)
	kyty_expect("--record-signals --ignore-term --raise 15: exit" "${record_ignored_after_result}" "6")
	file(READ "${WORK_DIR}/record4.txt" kyty_record4)
	kyty_expect("--record-signals --ignore-term --raise 15: file" "${kyty_record4}" "15\n")

	# --wait returns for a signal caught before it, once per signal.
	kyty_run(wait --record-signals "${WORK_DIR}/record5.txt" --raise 10 --wait --stdout woke --exit 8)
	kyty_text_hex(woke_hex "woke\n")
	kyty_expect("--wait after a caught signal: exit" "${wait_result}" "8")
	kyty_expect("--wait after a caught signal: stdout" "${wait_out}" "${woke_hex}")

	kyty_run(sleep --sleep-ms 50 --stdout slept)
	kyty_text_hex(slept_hex "slept\n")
	kyty_expect("--sleep-ms 50: exit" "${sleep_result}" "0")
	kyty_expect("--sleep-ms 50: stdout" "${sleep_out}" "${slept_hex}")

	# Introspection options for the spawn tests.
	kyty_run(cwd --print-cwd)
	file(REAL_PATH "${WORK_DIR}" kyty_real_work_dir)
	kyty_text_hex(cwd_hex "${kyty_real_work_dir}\n")
	kyty_expect("--print-cwd: stdout" "${cwd_out}" "${cwd_hex}")
	kyty_run(stdio --describe-stdio)
	kyty_text_hex(stdio_hex "stdin dev-null rdonly\nstdout blocking\nstderr blocking\n")
	kyty_expect("--describe-stdio: stdout" "${stdio_out}" "${stdio_hex}")
	kyty_run(fds --list-fds)
	file(READ "${fds_out_file}" kyty_fds_text)
	if(NOT kyty_fds_text MATCHES "^0 1 2( [0-9]+)*\n$")
		list(APPEND kyty_failures "--list-fds: got '${kyty_fds_text}', expected '0 1 2 ...'")
	endif()
	kyty_run(signal_state --print-signal-state)
	file(READ "${signal_state_out_file}" kyty_signal_text)
	if(NOT kyty_signal_text MATCHES
	   "^SIGPIPE (default|ignored|handler)\nSIGTERM (default|ignored|handler)\nblocked (none|[0-9 ]+)\n$")
		list(APPEND kyty_failures "--print-signal-state: got '${kyty_signal_text}'")
	endif()

	# Usage errors exit 64 and name the problem on stderr.
	kyty_run(unknown --bogus)
	kyty_expect("--bogus: exit" "${unknown_result}" "64")
	if(unknown_err STREQUAL "")
		list(APPEND kyty_failures "--bogus: no message on stderr")
	endif()
	set(kyty_usage_index 0)
	foreach(kyty_bad_arguments IN ITEMS "--exit" "--exit|256" "--exit|-1" "--stdout-hex|0g"
			"--stdout-hex|abc" "--raise|0" "--raise|65" "--sleep-ms|x" "--stdout"
			"--record-signals|${WORK_DIR}/missing/record.txt")
		string(REPLACE "|" ";" kyty_bad_list "${kyty_bad_arguments}")
		math(EXPR kyty_usage_index "${kyty_usage_index} + 1")
		kyty_run(usage${kyty_usage_index} ${kyty_bad_list})
		kyty_expect("${kyty_bad_arguments}: exit" "${usage${kyty_usage_index}_result}" "64")
	endforeach()
endif()

if(kyty_failures)
	list(JOIN kyty_failures "\n  " kyty_failures_text)
	message(FATAL_ERROR "ProcessHelperSelfTest (${CASE}) failed:\n  ${kyty_failures_text}")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
message(STATUS "ProcessHelperSelfTest (${CASE}): ${kyty_run_count} runs, every check passed")
