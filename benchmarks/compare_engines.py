#!/usr/bin/env python3
"""Run matched native/SQLite workloads and print a Markdown summary."""

import argparse
import json
import os
import statistics
import subprocess


DEFAULT_ITERATIONS = {
    "point": 10_000,
    "scan": 100,
    "count_miss": 100,
    "aggregate": 100,
    "join": 10,
    "join_miss": 10,
    "context_schema": 1_000,
    "context_spill": 100,
    "context_spill_acl": 100,
    "context_objects": 10,
}


def run(binary, engine, workload, rows, iterations, buffer_pages):
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
        "--buffer-pages",
        str(buffer_pages),
    ]
    completed = subprocess.run(
        command, check=True, capture_output=True, text=True
    )
    return json.loads(completed.stdout.strip())


def median_result(results):
    middle = dict(results[0])
    for key in ("elapsed_ms", "ops_per_sec", "peak_rss_bytes",
                "engine_memory_bytes", "buffer_memory_bytes"):
        if key not in middle:
            continue
        middle[key] = statistics.median(result[key] for result in results)
    for key in ("context_cache_hits", "context_cache_misses",
                "context_atoms_scored", "context_atoms_rendered",
                "raw_point_queries", "raw_point_hits",
                "value_count_queries", "value_count_rows_answered",
                "dense_group_aggregate_queries",
                "dense_group_aggregate_rows"):
        if key in middle:
            middle[key] = statistics.median(
                result.get(key, 0) for result in results
            )
    return middle


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--rows", type=int, default=10_000)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--buffer-pages", type=int, default=1024)
    parser.add_argument(
        "--workloads",
        nargs="+",
        choices=DEFAULT_ITERATIONS,
        default=list(DEFAULT_ITERATIONS),
    )
    arguments = parser.parse_args()
    if not os.path.exists(arguments.binary):
        raise SystemExit(
            f"benchmark binary not found: {arguments.binary}\n"
            "Build it first, for example:\n"
            "  cmake -S . -B build-sqlite-wsl "
            "-DCMAKE_BUILD_TYPE=Release -DSKIBIDI_WITH_SQLITE=ON\n"
            "  cmake --build build-sqlite-wsl "
            "--target skibidi_engine_comparison -j\n"
        )

    print("| Workload | Native | SQLite | Native / SQLite | "
          "Native peak RSS | SQLite peak RSS | Native est mem | "
          "Native buffer mem | Native raw point | Native value count | "
          "Native dense agg | Native ctx cache hits |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for workload in arguments.workloads:
        iterations = DEFAULT_ITERATIONS[workload]
        measurements = {}
        for engine in ("native", "sqlite"):
            measurements[engine] = median_result([
                run(arguments.binary, engine, workload, arguments.rows,
                    iterations, arguments.buffer_pages)
                for _ in range(arguments.repeats)
            ])
        native = measurements["native"]
        sqlite = measurements["sqlite"]
        ratio = native["elapsed_ms"] / sqlite["elapsed_ms"]
        print(
            f"| {workload} | {native['elapsed_ms']:.1f} ms | "
            f"{sqlite['elapsed_ms']:.1f} ms | {ratio:.2f}x | "
            f"{native['peak_rss_bytes'] / 1048576:.1f} MiB | "
            f"{sqlite['peak_rss_bytes'] / 1048576:.1f} MiB | "
            f"{native['engine_memory_bytes'] / 1048576:.2f} MiB | "
            f"{native.get('buffer_memory_bytes', 0) / 1048576:.2f} MiB | "
            f"{native.get('raw_point_queries', 0):.0f} | "
            f"{native.get('value_count_queries', 0):.0f} | "
            f"{native.get('dense_group_aggregate_queries', 0):.0f} | "
            f"{native.get('context_cache_hits', 0):.0f} |"
        )


if __name__ == "__main__":
    main()
