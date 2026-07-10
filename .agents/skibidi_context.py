#!/usr/bin/env python3
"""Small helper for agents using SkibidiQL as a context service.

It runs a SkibidiQL request file, retries common binary names, skips stale
binaries that do not understand ContextQL, and can print only the
`current_context` rows that should be passed to the LLM.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def candidate_binaries(root: Path) -> list[Path]:
    names = [
        root / "build" / "codex_skibidi_agent.exe",
        root / "build" / "codex_skibidi_agent",
        root / "build" / "codex_skibidi.exe",
        root / "build" / "codex_skibidi",
        root / "build" / "skibidi.exe",
        root / "build" / "skibidi",
        root / "build" / "Release" / "skibidi.exe",
        root / "build" / "Release" / "skibidi",
    ]
    return [path for path in names if path.exists()]


def is_stale_contextql_error(text: str) -> bool:
    return "manifest-context" in text and "Expected statement keyword" in text


def run_skibidi(binary: Path, db: Path, request_file: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), "--db", str(db), "--file", str(request_file)],
        cwd=repo_root(),
        text=True,
        capture_output=True,
        check=False,
    )


def extract_fields(output: str, field: str) -> list[str]:
    prefix = f"field={field} | value="
    values: list[str] = []
    for line in output.splitlines():
        if line.startswith(prefix):
            values.append(line[len(prefix):])
    return values


def is_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def remove_fresh_db(db: Path, root: Path) -> None:
    if not is_within(db, root):
        raise RuntimeError(f"--fresh refuses to delete DB outside repo: {db}")
    if db.exists():
        if db.is_dir():
            shutil.rmtree(db)
        else:
            db.unlink()


def print_prompt_pack(values: list[str]) -> None:
    context = "\n".join(values).strip()
    if not context:
        context = "(none retrieved)"
    print("SkibidiQL active context:")
    print(context)
    print()
    print("Rules:")
    print("- Treat this as the durable active memory for this turn.")
    print("- Ignore stale facts not present here.")
    print("- Do not reveal redacted values.")
    print("- If the context is insufficient, ask or proceed from the current user message.")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", default=".skibidi_agent_ctx", help="SkibidiQL DB directory")
    parser.add_argument("--file", required=True, help="Request .skql file to execute")
    parser.add_argument(
        "--context-only",
        action="store_true",
        help="Print only current_context rows for direct prompt insertion",
    )
    parser.add_argument(
        "--prompt",
        action="store_true",
        help="Wrap selected current_context rows in the recommended LLM prompt block",
    )
    parser.add_argument(
        "--fresh",
        action="store_true",
        help="Delete the DB path first. Only allowed for DB paths inside this repo.",
    )
    parser.add_argument(
        "--field",
        default="current_context",
        help="Field to extract when --context-only is set",
    )
    parser.add_argument(
        "--last",
        action="store_true",
        help="When extracting rows, use only the last matching field value",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = repo_root()
    request_file = (root / args.file).resolve() if not Path(args.file).is_absolute() else Path(args.file)
    db = (root / args.db).resolve() if not Path(args.db).is_absolute() else Path(args.db)

    if not request_file.exists():
        print(f"ERROR: request file does not exist: {request_file}", file=sys.stderr)
        return 2
    if args.fresh:
        try:
            remove_fresh_db(db, root)
        except RuntimeError as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 2

    binaries = candidate_binaries(root)
    if not binaries:
        print("ERROR: no SkibidiQL binary found under build/.", file=sys.stderr)
        print("Build first, then retry. Expected build/codex_skibidi.exe or build/skibidi.exe.", file=sys.stderr)
        return 2

    stale_errors: list[str] = []
    last: subprocess.CompletedProcess[str] | None = None
    for binary in binaries:
        completed = run_skibidi(binary, db, request_file)
        last = completed
        combined = completed.stdout + completed.stderr
        if completed.returncode == 0:
            if args.context_only or args.prompt:
                values = extract_fields(completed.stdout, args.field)
                if args.last and values:
                    values = [values[-1]]
                if args.prompt:
                    print_prompt_pack(values)
                elif values:
                    print("\n".join(values))
                return 0
            print(completed.stdout, end="")
            if completed.stderr:
                print(completed.stderr, end="", file=sys.stderr)
            return 0
        if is_stale_contextql_error(combined):
            stale_errors.append(f"{binary}: stale ContextQL parser")
            continue
        print(completed.stdout, end="")
        print(completed.stderr, end="", file=sys.stderr)
        return completed.returncode

    for error in stale_errors:
        print(f"SKIP: {error}", file=sys.stderr)
    if last is not None:
        print(last.stdout, end="")
        print(last.stderr, end="", file=sys.stderr)
    print("ERROR: all discovered SkibidiQL binaries were stale or failed.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
