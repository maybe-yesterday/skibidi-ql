#!/usr/bin/env python3
"""Compare normal SQL with cached and uncached SkibidiQL."""

from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time


def find_binary(root: Path) -> Path:
    names = [
        root / "build" / "skibidi_benchmark",
        root / "build" / "skibidi_benchmark.exe",
        root / "build" / "Release" / "skibidi_benchmark.exe",
        root / "build" / "benchmarks" / "skibidi_benchmark",
        root / "build" / "benchmarks" / "Release" / "skibidi_benchmark.exe",
    ]
    for candidate in names:
        if candidate.exists():
            return candidate
    raise FileNotFoundError(
        "Could not find skibidi_benchmark; build the benchmark target first."
    )


def windows_rss_reader(pid: int):
    kernel32 = ctypes.windll.kernel32
    psapi = ctypes.windll.psapi
    process_query = 0x0400
    process_vm_read = 0x0010
    handle = kernel32.OpenProcess(process_query | process_vm_read, False, pid)
    if not handle:
        return lambda: 0, lambda: None

    class Counters(ctypes.Structure):
        _fields_ = [
            ("cb", ctypes.c_ulong),
            ("PageFaultCount", ctypes.c_ulong),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
        ]

    def read() -> int:
        counters = Counters()
        counters.cb = ctypes.sizeof(counters)
        ok = psapi.GetProcessMemoryInfo(
            handle, ctypes.byref(counters), counters.cb
        )
        return int(counters.WorkingSetSize) if ok else 0

    return read, lambda: kernel32.CloseHandle(handle)


def linux_rss(pid: int) -> int:
    try:
        status = Path(f"/proc/{pid}/status").read_text(encoding="utf-8")
    except OSError:
        return 0
    for line in status.splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1]) * 1024
    return 0


def run_once(binary: Path, mode: str, args) -> dict:
    command = [
        str(binary),
        "--mode",
        mode,
        "--workload",
        args.workload,
        "--iterations",
        str(args.iterations),
        "--rows",
        str(args.rows),
    ]
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    if sys.platform == "win32":
        read_rss, close_reader = windows_rss_reader(process.pid)
    elif sys.platform.startswith("linux"):
        read_rss, close_reader = lambda: linux_rss(process.pid), lambda: None
    else:
        read_rss, close_reader = lambda: 0, lambda: None

    peak_rss = 0
    while process.poll() is None:
        peak_rss = max(peak_rss, read_rss())
        time.sleep(0.002)
    peak_rss = max(peak_rss, read_rss())
    close_reader()

    stdout, stderr = process.communicate()
    if process.returncode != 0:
        raise RuntimeError(stderr.strip() or stdout.strip())

    result = json.loads(stdout.strip().splitlines()[-1])
    if not result.get("release_build", False):
        print(
            "warning: benchmark binary is not a Release build; "
            "compiler-cache timings will be misleading",
            file=sys.stderr,
        )
    result["process_peak_rss_bytes"] = peak_rss
    return result


def median_result(results: list[dict]) -> dict:
    representative = dict(results[0])
    numeric = [
        "elapsed_ms",
        "compile_ms",
        "sqlite_prepare_ms",
        "sqlite_execute_ms",
        "ops_per_sec",
        "sqlite_peak_bytes",
        "sqlite_current_bytes",
        "cache_hits",
        "cache_misses",
        "cache_bytes",
        "statement_cache_hits",
        "statement_cache_misses",
        "statement_cache_bytes",
        "process_peak_rss_bytes",
    ]
    for key in numeric:
        representative[key] = statistics.median(item[key] for item in results)
    return representative


def format_bytes(value: float) -> str:
    return f"{value / (1024 * 1024):.2f} MiB"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--workload", choices=["point", "count", "mixed"],
                        default="point")
    parser.add_argument("--iterations", type=int, default=5000)
    parser.add_argument("--rows", type=int, default=10000)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    binary = args.binary or find_binary(root)
    modes = [
        "sql",
        "sql-prepared",
        "skibidi-uncached",
        "skibidi-cached",
        "skibidi-prepared",
    ]
    runs_by_mode = {mode: [] for mode in modes}
    for repeat in range(args.repeats):
        offset = repeat % len(modes)
        order = modes[offset:] + modes[:offset]
        for mode in order:
            runs_by_mode[mode].append(run_once(binary, mode, args))
    summaries = [median_result(runs_by_mode[mode]) for mode in modes]
    expected_checksum = summaries[0]["checksum"]
    for result in summaries[1:]:
        if result["checksum"] != expected_checksum:
            raise RuntimeError(
                f"result mismatch: {result['mode']} checksum "
                f"{result['checksum']} != SQL checksum {expected_checksum}"
            )

    if args.json:
        print(json.dumps(summaries, indent=2))
        return 0

    baseline = summaries[0]["elapsed_ms"]
    print(
        "| Mode | Median time | Compile | Prepare | Execute | vs SQL | "
        "Ops/sec | Peak RSS | Compile hits | Statement hits |"
    )
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for result in summaries:
        ratio = result["elapsed_ms"] / baseline
        print(
            f"| {result['mode']} | {result['elapsed_ms']:.3f} ms | "
            f"{result['compile_ms']:.3f} ms | "
            f"{result['sqlite_prepare_ms']:.3f} ms | "
            f"{result['sqlite_execute_ms']:.3f} ms | "
            f"{ratio:.2f}x | {result['ops_per_sec']:.0f} | "
            f"{format_bytes(result['process_peak_rss_bytes'])} | "
            f"{result['cache_hits']:.0f} | "
            f"{result['statement_cache_hits']:.0f} |"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
