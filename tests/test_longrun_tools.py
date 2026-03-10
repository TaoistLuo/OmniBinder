#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
SUMMARY = REPO_ROOT / "tools" / "longrun_summary.py"
VERDICT = REPO_ROOT / "tools" / "longrun_verdict.py"


def run_python(script: pathlib.Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(script), *args],
        cwd=str(REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def write_log(contents: str) -> pathlib.Path:
    temp_dir = pathlib.Path(tempfile.mkdtemp(prefix="omnibinder_longrun_"))
    log_path = temp_dir / "weekly.log"
    log_path.write_text(contents, encoding="utf-8")
    return log_path


def assert_contains(text: str, needle: str) -> None:
    assert needle in text, "expected to find %r in output\nstdout:\n%s" % (needle, text)


def test_summary_reports_cycles_and_keywords() -> None:
    log_path = write_log(
        "START long-run cycle 1\n"
        "100% tests passed, 0 tests failed out of 20\n"
        "All soak rounds passed.\n"
        "All disconnect recovery rounds passed.\n"
        "sm_reconnect_begin\n"
        "sm_reconnect_success\n"
        "END long-run cycle 1\n"
    )

    result = run_python(SUMMARY, str(log_path), "--tail", "3")
    assert result.returncode == 0, result.stderr
    assert_contains(result.stdout, "Complete cycles (min): 1")
    assert_contains(result.stdout, "Potential incomplete cycles: 0")
    assert_contains(result.stdout, "sm_reconnect_begin: 1")
    assert_contains(result.stdout, "sm_reconnect_success: 1")
    assert_contains(result.stdout, "[Last 3 lines]")


def test_summary_missing_file_fails() -> None:
    missing = REPO_ROOT / "build" / "longrun_logs" / "does-not-exist.log"
    result = run_python(SUMMARY, str(missing))
    assert result.returncode == 1
    assert_contains(result.stderr, "Log file not found")


def test_verdict_pass_for_clean_log() -> None:
    log_path = write_log(
        "START long-run cycle 1\n"
        "100% tests passed, 0 tests failed out of 20\n"
        "END long-run cycle 1\n"
        "sm_reconnect_begin\n"
        "sm_reconnect_success\n"
    )

    result = run_python(VERDICT, str(log_path), "--min-cycles", "1")
    assert result.returncode == 0, result.stderr
    assert_contains(result.stdout, "VERDICT: PASS")
    assert_contains(result.stdout, "Complete cycles: 1")


def test_verdict_warning_for_mismatched_markers() -> None:
    log_path = write_log(
        "START long-run cycle 1\n"
        "100% tests passed, 0 tests failed out of 20\n"
    )

    result = run_python(VERDICT, str(log_path), "--min-cycles", "1")
    assert result.returncode == 0, result.stderr
    assert_contains(result.stdout, "VERDICT: WARNING")
    assert_contains(result.stdout, "cycle markers mismatched")


def test_verdict_fail_for_failure_markers() -> None:
    log_path = write_log(
        "START long-run cycle 1\n"
        "tests failed out of 20\n"
        "FAIL: reconnect loop\n"
        "END long-run cycle 1\n"
    )

    result = run_python(VERDICT, str(log_path), "--min-cycles", "1")
    assert result.returncode == 1
    assert_contains(result.stdout, "VERDICT: FAIL")
    assert_contains(result.stdout, "failure markers present in long-run log")


def test_verdict_fail_when_reconnect_never_succeeds() -> None:
    log_path = write_log(
        "START long-run cycle 1\n"
        "100% tests passed, 0 tests failed out of 20\n"
        "sm_reconnect_begin\n"
        "END long-run cycle 1\n"
    )

    result = run_python(VERDICT, str(log_path), "--min-cycles", "1")
    assert result.returncode == 1
    assert_contains(result.stdout, "VERDICT: FAIL")
    assert_contains(result.stdout, "reconnect attempts observed without success markers")


def main() -> int:
    tests = [
        test_summary_reports_cycles_and_keywords,
        test_summary_missing_file_fails,
        test_verdict_pass_for_clean_log,
        test_verdict_warning_for_mismatched_markers,
        test_verdict_fail_for_failure_markers,
        test_verdict_fail_when_reconnect_never_succeeds,
    ]

    for test in tests:
        test()

    print("All long-run tool tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
