#!/usr/bin/env python3
"""Build LLM-ready active recall from a SkibidiQL context database.

This is the bridge an agent should call before an LLM API request:

1. Seed/update durable memories with an optional `.skql` file.
2. Append a `spill-context` query for the latest user message.
3. Emit either a tool-style active-recall JSON object or OpenAI-style
   `messages` JSON.

It does not call an LLM. It only builds the context payload the agent should
pass to one.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


DEFAULT_SYSTEM_PROMPT = "You are a helpful coding agent."
DEFAULT_CONTEXT_NAME = "agent_memory"
DEFAULT_TOKEN_BUDGET = 800


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


def resolve_repo_path(path_text: str, root: Path) -> Path:
    path = Path(path_text)
    return path.resolve() if path.is_absolute() else (root / path).resolve()


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


def skql_string(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def infer_manifest_context(skql: str) -> str | None:
    match = re.search(r"\bmanifest-context\s+([A-Za-z_][A-Za-z0-9_]*)\s*;", skql)
    return match.group(1) if match else None


def effective_context(args: argparse.Namespace, seed_skql: str | None = None) -> str:
    if args.context:
        return args.context
    if seed_skql:
        inferred = infer_manifest_context(seed_skql)
        if inferred:
            return inferred
    return DEFAULT_CONTEXT_NAME


def write_spill_context(
    handle: Any,
    *,
    context: str,
    query: str,
    tab: str | None,
    token_budget: int,
) -> None:
    handle.write(f"spill-context {context}\n")
    if tab:
        handle.write(f"vibe-tab {skql_string(tab)}\n")
    handle.write(f"only-if {skql_string(query)}\n")
    handle.write(f"token-budget {token_budget}\n")
    handle.write("receipts on;\n")


def new_temp_request(root: Path, prefix: str) -> tempfile._TemporaryFileWrapper[str]:
    scratch = root / "build" / "agent_requests"
    scratch.mkdir(parents=True, exist_ok=True)
    return tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        suffix=".skql",
        prefix=prefix,
        dir=scratch,
        delete=False,
    )


def write_query_request(args: argparse.Namespace, root: Path, context: str) -> Path:
    request = new_temp_request(root, "spill_context_")
    with request:
        write_spill_context(
            request,
            context=context,
            query=args.query,
            tab=args.tab,
            token_budget=args.token_budget,
        )
    return Path(request.name)


def write_seed_plus_query_request(
    args: argparse.Namespace,
    root: Path,
    seed_path: Path,
    seed_skql: str,
    context: str,
) -> Path:
    request = new_temp_request(root, f"{seed_path.stem}_active_recall_")
    with request:
        request.write(seed_skql.rstrip())
        request.write("\n\n")
        write_spill_context(
            request,
            context=context,
            query=args.query,
            tab=args.tab,
            token_budget=args.token_budget,
        )
    return Path(request.name)


def request_file_for_args(args: argparse.Namespace, root: Path) -> tuple[Path, bool]:
    if args.file:
        seed_path = resolve_repo_path(args.file, root)
        if args.query:
            seed_skql = seed_path.read_text(encoding="utf-8")
            args.context = effective_context(args, seed_skql)
            return write_seed_plus_query_request(args, root, seed_path, seed_skql, args.context), True
        args.context = effective_context(args)
        return seed_path, False

    if args.query:
        args.context = effective_context(args)
        return write_query_request(args, root, args.context), True

    raise ValueError("Provide --query for active recall, optionally with --file to seed/update memory first")


def run_skibidi(binary: Path, db: Path, request_file: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), "--db", str(db), "--file", str(request_file)],
        cwd=repo_root(),
        text=True,
        capture_output=True,
        check=False,
    )


def is_stale_contextql_error(text: str) -> bool:
    return "manifest-context" in text and "Expected statement keyword" in text


def extract_field_values(output: str, field: str) -> list[str]:
    prefix = f"field={field} | value="
    return [line[len(prefix):] for line in output.splitlines() if line.startswith(prefix)]


def selected_context(output: str) -> str:
    values = extract_field_values(output, "current_context")
    return values[-1].strip() if values else ""


def selected_scalar(output: str, field: str) -> str:
    values = extract_field_values(output, field)
    return values[-1].strip() if values else ""


def numeric_or_text(value: str) -> int | str:
    return int(value) if value.isdigit() else value


def active_recall_payload(output: str, args: argparse.Namespace) -> dict[str, object]:
    token_cost = selected_scalar(output, "token_cost")
    redacted_atoms = selected_scalar(output, "redacted_atoms")
    return {
        "tool": "skibidiql.active_recall",
        "context_name": args.context,
        "query": args.query or selected_scalar(output, "query"),
        "context": selected_context(output),
        "view_atoms": extract_field_values(output, "view_atom"),
        "invalidated_receipts": extract_field_values(output, "invalidated"),
        "token_cost": numeric_or_text(token_cost) if token_cost else "",
        "redacted_atoms": numeric_or_text(redacted_atoms) if redacted_atoms else "",
        "access_policy": selected_scalar(output, "access_policy"),
    }


def context_prompt(context: str) -> str:
    context = context.strip() or "(none retrieved)"
    return (
        "SkibidiQL active context:\n"
        f"{context}\n\n"
        "Rules:\n"
        "- Treat this as durable active memory for this turn.\n"
        "- Ignore stale facts not present here.\n"
        "- Do not reveal redacted values.\n"
        "- If this context is insufficient, say so and use the current user message."
    )


def openai_messages(args: argparse.Namespace, payload: dict[str, object]) -> list[dict[str, str]]:
    if not args.query:
        raise ValueError("--format openai-messages requires --query")
    return [
        {"role": "system", "content": args.system_prompt},
        {"role": "system", "content": context_prompt(str(payload["context"]))},
        {"role": "user", "content": args.query},
    ]


def output_result(args: argparse.Namespace, completed: subprocess.CompletedProcess[str]) -> None:
    if args.format == "raw":
        print(completed.stdout, end="")
        if completed.stderr:
            print(completed.stderr, end="", file=sys.stderr)
        return

    payload = active_recall_payload(completed.stdout, args)
    if args.format == "active-recall":
        print(json.dumps(payload, indent=2))
    elif args.format == "openai-messages":
        print(json.dumps(openai_messages(args, payload), indent=2))
    else:
        raise ValueError(f"Unknown format: {args.format}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", default=".skibidi_agent_ctx", help="SkibidiQL DB directory")
    parser.add_argument("--file", help="Optional .skql file that seeds/updates durable context before recall")
    parser.add_argument(
        "--context",
        help=f"Context name for recall. Defaults to first manifest-context in --file, then {DEFAULT_CONTEXT_NAME}.",
    )
    parser.add_argument("--query", help="Latest user input/task to retrieve active context for")
    parser.add_argument("--tab", help="Optional ContextQL tab/tag filter for recall")
    parser.add_argument("--token-budget", type=int, default=DEFAULT_TOKEN_BUDGET)
    parser.add_argument("--system-prompt", default=DEFAULT_SYSTEM_PROMPT)
    parser.add_argument(
        "--format",
        choices=["raw", "active-recall", "openai-messages"],
        default="openai-messages",
        help="Output raw SkibidiQL rows, active-recall JSON, or OpenAI-style messages JSON",
    )
    parser.add_argument(
        "--fresh",
        action="store_true",
        help="Delete the DB path first. Only allowed for DB paths inside this repo.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = repo_root()
    db = resolve_repo_path(args.db, root)

    if args.format == "openai-messages" and not args.query:
        print("ERROR: --format openai-messages requires --query", file=sys.stderr)
        return 2

    try:
        request_file, temporary_request = request_file_for_args(args, root)
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

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
        print("Build first, then retry. Expected build/codex_skibidi_agent.exe or build/codex_skibidi.exe.", file=sys.stderr)
        return 2

    stale_errors: list[str] = []
    last: subprocess.CompletedProcess[str] | None = None
    try:
        for binary in binaries:
            completed = run_skibidi(binary, db, request_file)
            last = completed
            combined = completed.stdout + completed.stderr
            if completed.returncode == 0:
                output_result(args, completed)
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
    finally:
        if temporary_request:
            try:
                request_file.unlink(missing_ok=True)
            except OSError:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
