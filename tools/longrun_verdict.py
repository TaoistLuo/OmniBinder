#!/usr/bin/env python3
import argparse
import pathlib
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


def count(lines, token):
    return sum(1 for line in lines if token in line)


def main() -> int:
    parser = argparse.ArgumentParser(description="Emit PASS/WARNING/FAIL verdict for OmniBinder weekly long-run log")
    parser.add_argument("logfile", nargs="?", default="build/longrun_logs/weekly.log",
                        help="Path to weekly long-run log")
    parser.add_argument("--min-cycles", type=int, default=1,
                        help="Minimum complete cycles required for PASS/WARNING evaluation")
    args = parser.parse_args()

    path = pathlib.Path(args.logfile)
    if not path.exists():
        print(f"FAIL: log file not found: {path}")
        return 2

    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()

    starts = count(lines, "START long-run cycle")
    ends = count(lines, "END long-run cycle")
    cycles = min(starts, ends)

    explicit_fail = count(lines, "FAIL:")
    failed_blocks = count(lines, "tests failed out of") - count(lines, "100% tests passed, 0 tests failed out of")
    timeout_hits = count(lines, "Timeout")
    aborted_hits = count(lines, "Subprocess aborted")

    reconnect_begin = count(lines, "sm_reconnect_begin")
    reconnect_success = count(lines, "sm_reconnect_success")
    rpc_timeout = count(lines, "rpc_timeout")
    data_lost = count(lines, "data_connection_lost")

    verdict = "PASS"
    reasons = []

    if cycles < args.min_cycles:
        verdict = "WARNING"
        reasons.append(f"complete cycles {cycles} below required minimum {args.min_cycles}")

    if starts != ends:
        verdict = "WARNING" if verdict == "PASS" else verdict
        reasons.append(f"cycle markers mismatched (start={starts}, end={ends})")

    if explicit_fail > 0 or failed_blocks > 0 or timeout_hits > 0 or aborted_hits > 0:
        verdict = "FAIL"
        reasons.append("failure markers present in long-run log")

    if reconnect_begin > 0 and reconnect_success == 0:
        verdict = "FAIL"
        reasons.append("reconnect attempts observed without success markers")

    if cycles > 0 and rpc_timeout > cycles * 20:
        if verdict != "FAIL":
            verdict = "WARNING"
        reasons.append(f"rpc_timeout count is high relative to cycle count ({rpc_timeout} over {cycles} cycles)")

    if cycles > 0 and data_lost > cycles * 10:
        if verdict != "FAIL":
            verdict = "WARNING"
        reasons.append(f"data_connection_lost count is high relative to cycle count ({data_lost} over {cycles} cycles)")

    print(f"VERDICT: {verdict}")
    print(f"Log file: {path}")
    print(f"Complete cycles: {cycles}")
    print(f"Cycle markers: start={starts}, end={ends}")
    print("Keyword counts:")
    for keyword in KEYWORDS:
        print(f"  {keyword}: {count(lines, keyword)}")

    if reasons:
        print("Reasons:")
        for reason in reasons:
            print(f"  - {reason}")
    else:
        print("Reasons:")
        print("  - no failure markers detected and cycle accounting looks healthy")

    return 1 if verdict == "FAIL" else 0


if __name__ == "__main__":
    raise SystemExit(main())
