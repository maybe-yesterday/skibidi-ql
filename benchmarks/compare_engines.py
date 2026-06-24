#!/usr/bin/env python3
"""Run matched native/SQLite workloads and print a Markdown summary."""

import argparse
import json
import statistics
import subprocess


DEFAULT_ITERATIONS = {
    "point": 10_000,
    "scan": 100,
    "aggregate": 100,
    "join": 10,
}


def run(binary, engine, workload, rows, iterations):
    command = [
        binary,
        "--engine",
        engine,
        "--workload",
        workload,
        "--rows",
        str(rows),
        "--iterations",
        str(iterations),
    ]
    completed = subprocess.run(
        command, check=True, capture_output=True, text=True
    )
    return json.loads(completed.stdout.strip())


def median_result(results):
    middle = dict(results[0])
    for key in ("elapsed_ms", "ops_per_sec", "peak_rss_bytes",
                "engine_memory_bytes"):
        middle[key] = statistics.median(result[key] for result in results)
    return middle


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--rows", type=int, default=10_000)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument(
        "--workloads",
        nargs="+",
        choices=DEFAULT_ITERATIONS,
        default=list(DEFAULT_ITERATIONS),
    )
    arguments = parser.parse_args()

    print("| Workload | Native | SQLite | Native / SQLite | "
          "Native peak RSS | SQLite peak RSS |")
    print("|---|---:|---:|---:|---:|---:|")
    for workload in arguments.workloads:
        iterations = DEFAULT_ITERATIONS[workload]
        measurements = {}
        for engine in ("native", "sqlite"):
            measurements[engine] = median_result([
                run(arguments.binary, engine, workload, arguments.rows,
                    iterations)
                for _ in range(arguments.repeats)
            ])
        native = measurements["native"]
        sqlite = measurements["sqlite"]
        ratio = native["elapsed_ms"] / sqlite["elapsed_ms"]
        print(
            f"| {workload} | {native['elapsed_ms']:.1f} ms | "
            f"{sqlite['elapsed_ms']:.1f} ms | {ratio:.2f}x | "
            f"{native['peak_rss_bytes'] / 1048576:.1f} MiB | "
            f"{sqlite['peak_rss_bytes'] / 1048576:.1f} MiB |"
        )


if __name__ == "__main__":
    main()
