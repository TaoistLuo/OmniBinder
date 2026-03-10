#!/usr/bin/env python3
import argparse
import pathlib
import re
import sys


KEYWORDS = [
    "sm_connection_lost",
    "sm_reconnect_begin",
    "sm_reconnect_success",
    "rpc_timeout",
    "data_connection_lost",
    "FAIL:",
    "FAILED",
    "Subprocess aborted",
    "Timeout",
]


def count_occurrences(lines, token):
    return sum(1 for line in lines if token in line)


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize OmniBinder long-run weekly log")
    parser.add_argument("logfile", nargs="?", default="build/longrun_logs/weekly.log",
                        help="Path to weekly long-run log")
    parser.add_argument("--tail", type=int, default=40,
                        help="How many trailing log lines to print")
    args = parser.parse_args()

    log_path = pathlib.Path(args.logfile)
    if not log_path.exists():
        print(f"Log file not found: {log_path}", file=sys.stderr)
        return 1

    text = log_path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()

    start_count = count_occurrences(lines, "START long-run cycle")
    end_count = count_occurrences(lines, "END long-run cycle")
    soak_count = count_occurrences(lines, "All soak rounds passed.")
    recovery_count = count_occurrences(lines, "All disconnect recovery rounds passed.")

    ctest_full_pass = len(re.findall(r"100% tests passed, 0 tests failed out of", text))
    explicit_failures = count_occurrences(lines, "FAIL:")
    ctest_failed = len(re.findall(r"tests failed out of", text)) - ctest_full_pass

    print("=== OmniBinder Long-run Summary ===")
    print(f"Log file: {log_path}")
    print(f"Total lines: {len(lines)}")
    print()

    print("[Cycle Summary]")
    print(f"START markers: {start_count}")
    print(f"END markers:   {end_count}")
    print(f"Complete cycles (min): {min(start_count, end_count)}")
    print(f"Potential incomplete cycles: {max(start_count - end_count, 0)}")
    print()

    print("[Runner Completion]")
    print(f"Soak runner success markers:       {soak_count}")
    print(f"Disconnect recovery success markers: {recovery_count}")
    print(f"CTest full-pass blocks:            {ctest_full_pass}")
    print()

    print("[Failure Signals]")
    print(f"Explicit FAIL lines: {explicit_failures}")
    print(f"CTest failure blocks: {max(ctest_failed, 0)}")
    print()

    print("[Keyword Counts]")
    for keyword in KEYWORDS:
        print(f"{keyword}: {count_occurrences(lines, keyword)}")
    print()

    print(f"[Last {args.tail} lines]")
    for line in lines[-args.tail:]:
        print(line)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
