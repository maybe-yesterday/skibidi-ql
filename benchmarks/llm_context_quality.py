#!/usr/bin/env python3
"""Real-LLM context-quality dogfood benchmark.

This benchmark asks an actual OpenAI-compatible chat model to answer from
different context strategies:

- `skibidiql`: active recall from the SkibidiQL context DB
- `lexical_rag`: dependency-free BM25-ish retrieval over message chunks
- `mem0ai`: optional real Mem0 retrieval, when `mem0ai` is installed
- `full_history`: every prior message stuffed into the prompt
- `recency_window`: only the last N prior messages

It is intentionally tiny by default so a live run stays cheap. The default
methods use only the Python standard library and skip cleanly when
`OPENAI_API_KEY` is not set. `mem0ai` is opt-in because it performs its own LLM
and embedding calls while building memory.
"""

from __future__ import annotations

import argparse
import gc
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


DEFAULT_MODEL = os.environ.get("SKIBIDI_LLM_MODEL", "gpt-4o-mini")
DEFAULT_BASE_URL = os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1")
DEFAULT_ENV_FILE = ".env.local"
DEFAULT_METHODS = "skibidiql,lexical_rag,full_history,recency_window"
SUPPORTED_METHODS = {"skibidiql", "lexical_rag", "mem0ai", "full_history", "recency_window"}
EVAL_SYSTEM_PROMPT = (
    "You are evaluating an agent-memory context system. Answer the user using "
    "only the provided context/history. Return JSON only with keys: answer, "
    "used_facts, safety_notes. Keep the answer short. Do not reveal raw secrets."
)


@dataclass(frozen=True)
class Message:
    message_id: int
    speaker: str
    text: str
    tab: str = ""


@dataclass(frozen=True)
class Scenario:
    name: str
    challenge: str
    query: str
    messages: list[Message]
    required: list[str] = field(default_factory=list)
    invalidated: list[str] = field(default_factory=list)
    restricted: list[str] = field(default_factory=list)
    aliases: dict[str, list[str]] = field(default_factory=dict)


@dataclass
class MethodResult:
    scenario: str
    challenge: str
    method: str
    ok: bool
    recall_hits: int
    recall_total: int
    invalidated_excluded: int
    invalidated_total: int
    restricted_excluded: int
    restricted_total: int
    input_tokens: int
    output_tokens: int
    elapsed_s: float
    answer: str
    context_preview: str = ""
    error: str = ""


@dataclass
class Mem0Retriever:
    memory: Any
    qdrant_path: Path
    history_path: Path
    user_prefix: str


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def read_env_value(path: Path, name: str) -> str | None:
    if not path.exists():
        return None
    prefix = name + "="
    try:
        for line in path.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#") or not stripped.startswith(prefix):
                continue
            value = stripped[len(prefix):].strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
                value = value[1:-1]
            return value or None
    except OSError:
        return None
    return None


def load_api_key(root: Path, env_file: str | None) -> str | None:
    if os.environ.get("OPENAI_API_KEY"):
        return os.environ["OPENAI_API_KEY"]
    if not env_file:
        return None
    path = Path(env_file)
    candidate = path if path.is_absolute() else root / path
    return read_env_value(candidate, "OPENAI_API_KEY")


def estimate_tokens(text: str) -> int:
    return max(1, (len(text) + 3) // 4)


def lower(text: str) -> str:
    return text.lower()


GLOBAL_ALIASES: dict[str, list[str]] = {
    "nyc": ["NYC", "New York City", "New York", "the Big Apple"],
    "new york city": ["NYC", "New York City", "New York", "the Big Apple"],
    "quiet": ["quiet", "calm", "low-noise", "low noise", "not loud", "chill"],
    "vegan": ["vegan", "plant-based", "plant based", "meat-free", "meat free", "no meat"],
    "salmon": ["salmon", "fish"],
    "redacted": ["redacted", "withheld", "hidden", "not reveal", "do not reveal", "don't reveal"],
}


def normalize_for_match(text: str) -> str:
    normalized = re.sub(r"[^a-z0-9]+", " ", text.lower())
    return " ".join(normalized.split())


def phrase_present(text: str, phrase: str) -> bool:
    normalized_text = f" {normalize_for_match(text)} "
    normalized_phrase = normalize_for_match(phrase)
    return bool(normalized_phrase) and f" {normalized_phrase} " in normalized_text


def aliases_for(needle: str, scenario: Scenario | None = None) -> list[str]:
    aliases = [needle]
    key = normalize_for_match(needle)
    if key in GLOBAL_ALIASES:
        aliases.extend(GLOBAL_ALIASES[key])
    if scenario and needle in scenario.aliases:
        aliases.extend(scenario.aliases[needle])
    if scenario and key in scenario.aliases:
        aliases.extend(scenario.aliases[key])
    deduped: list[str] = []
    seen: set[str] = set()
    for alias in aliases:
        alias_key = normalize_for_match(alias)
        if alias_key and alias_key not in seen:
            deduped.append(alias)
            seen.add(alias_key)
    return deduped


def semantic_match(text: str, needle: str, scenario: Scenario | None = None) -> bool:
    return any(phrase_present(text, alias) for alias in aliases_for(needle, scenario))


def count_hits(text: str, needles: list[str], scenario: Scenario | None = None) -> int:
    return sum(1 for needle in needles if needle and semantic_match(text, needle, scenario))


def count_absent(text: str, needles: list[str], scenario: Scenario | None = None) -> int:
    return sum(1 for needle in needles if not needle or not semantic_match(text, needle, scenario))


STOPWORDS = {
    "a",
    "an",
    "and",
    "are",
    "as",
    "at",
    "be",
    "do",
    "does",
    "for",
    "from",
    "i",
    "is",
    "it",
    "me",
    "my",
    "of",
    "or",
    "should",
    "the",
    "to",
    "using",
    "what",
    "where",
    "with",
}


def normalize_token(token: str) -> str:
    token = token.lower()
    for suffix in ("ing", "ed", "es", "s"):
        if len(token) > len(suffix) + 3 and token.endswith(suffix):
            return token[: -len(suffix)]
    return token


def tokenize(text: str) -> list[str]:
    tokens = [normalize_token(token) for token in re.findall(r"[A-Za-z0-9_]+", text)]
    return [token for token in tokens if token and token not in STOPWORDS]


def lexical_score(query: str, message: Message) -> float:
    query_tokens = set(tokenize(query))
    message_tokens = set(tokenize(message.text + " " + message.tab))
    if not query_tokens or not message_tokens:
        return 0.0
    overlap = query_tokens & message_tokens
    score = float(len(overlap))
    query_text = lower(query)
    message_text = lower(message.text)
    for token in overlap:
        if token in message_text:
            score += 0.25
    if message.tab and any(token in lower(message.tab) for token in query_tokens):
        score += 0.75
    if "restaurant" in query_tokens and {"prefer", "preference"} & message_tokens:
        score += 0.5
    if "dog" in query_tokens and "dog" in message_tokens:
        score += 0.5
    if "security" in query_tokens and {"never", "password", "secret", "key", "token"} & message_tokens:
        score += 0.5
    if any(phrase in query_text for phrase in ("where", "location")) and {"live", "moved", "location"} & message_tokens:
        score += 0.5
    return score


def skql_string(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def seed_skql(scenario: Scenario, context_name: str) -> str:
    lines = [f"manifest-context {context_name};", ""]
    for message in scenario.messages:
        lines.extend(
            [
                f"yeet-memory {context_name} drip",
                f"    ({message.message_id}, {skql_string(message.speaker)}, {skql_string(message.text)})",
            ]
        )
        if message.tab:
            lines.append(f"vibe-tab {skql_string(message.tab)};")
        else:
            lines.append("vibe-tab auto;")
        lines.append("")
    return "\n".join(lines)


def filler_messages(start_id: int, count: int) -> list[Message]:
    fillers = [
        "Assistant discussed unrelated calendar cleanup.",
        "User switched to CSS polish for a moment.",
        "Assistant mentioned benchmark warmups.",
        "User asked about weekend food ideas.",
        "Assistant summarized a build warning.",
        "User talked about a random movie.",
    ]
    return [
        Message(
            start_id + idx,
            "assistant" if idx % 2 == 0 else "user",
            f"Filler turn {start_id + idx}: {fillers[idx % len(fillers)]}",
            "filler",
        )
        for idx in range(count)
    ]


def scenarios() -> list[Scenario]:
    return [
        Scenario(
            name="location_correction",
            challenge="contradiction + stale fact",
            query="Where should I assume the user is, and what restaurant preference matters?",
            messages=[
                Message(1, "user", "I live in Seattle.", "user facts"),
                *filler_messages(100, 10),
                Message(2, "user", "I prefer quiet restaurants.", "food"),
                *filler_messages(200, 10),
                Message(3, "user", "Actually I moved to NYC.", "user facts"),
            ],
            required=["NYC", "quiet"],
            invalidated=["Seattle"],
        ),
        Scenario(
            name="stable_preference_buried",
            challenge="stable preference + topic switches",
            query="What restaurant preference should I remember?",
            messages=[
                Message(11, "user", "I prefer vegan restaurants.", "food"),
                *filler_messages(300, 28),
                Message(12, "assistant", "Decision: README central message is context is data.", "project docs"),
            ],
            required=["vegan"],
        ),
        Scenario(
            name="dog_topic_switch",
            challenge="topic switch + multi-fact recall",
            query="What is my dog named and what food does my dog like?",
            messages=[
                Message(21, "user", "My dog is named Nori.", "pets"),
                *filler_messages(500, 14),
                Message(22, "user", "My dog likes salmon.", "pets"),
                *filler_messages(600, 8),
            ],
            required=["Nori", "salmon"],
        ),
        Scenario(
            name="redacted_secret",
            challenge="ACL-restricted fact",
            query="What security constraint should I follow? Do not reveal raw secrets.",
            messages=[
                Message(31, "user", "Never share password hunter2 or api key tokens.", "constraints"),
                *filler_messages(700, 8),
            ],
            required=["redacted"],
            restricted=["hunter2"],
        ),
        Scenario(
            name="sloppy_location_synonym",
            challenge="synonyms + badly phrased prompt",
            query="where's the user posted up rn, and what's the dinner vibe?",
            messages=[
                Message(41, "user", "I used to be based in Seattle.", "user facts"),
                *filler_messages(800, 12),
                Message(42, "user", "Tiny update: I am in New York City now.", "user facts"),
                *filler_messages(850, 8),
                Message(43, "user", "Dinner-wise I like calm, low-noise spots.", "food"),
            ],
            required=["NYC", "quiet"],
            invalidated=["Seattle"],
            aliases={
                "quiet": ["calm", "low-noise", "low noise"],
                "NYC": ["New York City", "New York"],
            },
        ),
        Scenario(
            name="plant_based_sloppy_prompt",
            challenge="synonyms + paraphrased preference",
            query="food recs should lean which way? user phrased it kinda messy",
            messages=[
                Message(51, "user", "Not tryna do meat-heavy meals lately.", "food"),
                *filler_messages(900, 18),
                Message(52, "user", "I prefer plant-based spots; meat-heavy meals are not it.", "food"),
            ],
            required=["vegan"],
            aliases={"vegan": ["plant-based", "plant based", "meat-free", "no meat"]},
        ),
        Scenario(
            name="pupper_sloppy_query",
            challenge="badly phrased prompt + topic switch",
            query="pupper noms?? name too if we got it",
            messages=[
                Message(61, "user", "My dog is named Nori.", "pets"),
                *filler_messages(950, 10),
                Message(62, "user", "My dog likes salmon.", "pets"),
                *filler_messages(980, 10),
            ],
            required=["Nori", "salmon"],
            aliases={
                "Nori": ["Nori"],
                "salmon": ["salmon", "fish"],
            },
        ),
    ]


def method_names(text: str) -> list[str]:
    return [part.strip() for part in text.split(",") if part.strip()]


def messages_to_text(messages: list[dict[str, str]]) -> str:
    return "\n".join(f"{item['role']}: {item['content']}" for item in messages)


def compact(text: str, limit: int) -> str:
    one_line = " ".join(text.split())
    if len(one_line) <= limit:
        return one_line
    return one_line[: max(0, limit - 3)].rstrip() + "..."


def context_preview(messages: list[dict[str, str]], limit: int = 220) -> str:
    if len(messages) >= 2:
        return compact(messages[1]["content"], limit)
    return compact(messages_to_text(messages), limit)


def baseline_messages(scenario: Scenario, method: str, recency_messages: int) -> list[dict[str, str]]:
    if method == "full_history":
        selected = scenario.messages
        label = "Full conversation history"
    elif method == "recency_window":
        selected = scenario.messages[-recency_messages:]
        label = f"Last {recency_messages} messages"
    else:
        raise ValueError(f"Unknown baseline method: {method}")

    history = "\n".join(
        f"message_{message.message_id} {message.speaker}: {message.text}" for message in selected
    )
    return [
        {"role": "system", "content": EVAL_SYSTEM_PROMPT},
        {"role": "system", "content": f"{label}:\n{history or '(none)'}"},
        {"role": "user", "content": scenario.query},
    ]


def lexical_rag_messages(scenario: Scenario, top_k: int) -> list[dict[str, str]]:
    ranked = sorted(
        ((lexical_score(scenario.query, message), message) for message in scenario.messages),
        key=lambda item: (item[0], item[1].message_id),
        reverse=True,
    )
    chunks = [
        f"score={score:.2f} message_{message.message_id} {message.speaker}"
        f"{' tab=' + message.tab if message.tab else ''}: {message.text}"
        for score, message in ranked[:top_k]
    ]
    return [
        {"role": "system", "content": EVAL_SYSTEM_PROMPT},
        {"role": "system", "content": "Lexical RAG retrieved message chunks:\n" + ("\n".join(chunks) or "(none)")},
        {"role": "user", "content": scenario.query},
    ]


def run_skibidi_messages(
    scenario: Scenario,
    *,
    root: Path,
    db_root: Path,
    token_budget: int,
) -> list[dict[str, str]]:
    context_name = f"llm_{scenario.name}"
    request_dir = root / "build" / "llm_context_quality"
    request_dir.mkdir(parents=True, exist_ok=True)
    request_file = tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        suffix=".skql",
        prefix=f"{scenario.name}_",
        dir=request_dir,
        delete=False,
    )
    request_path = Path(request_file.name)
    try:
        with request_file:
            request_file.write(seed_skql(scenario, context_name))
        db = db_root / scenario.name
        if db.exists():
            if db.is_dir():
                shutil.rmtree(db)
            else:
                db.unlink()
        cmd = [
            sys.executable,
            "-B",
            str(root / ".agents" / "skibidi_context.py"),
            "--fresh",
            "--db",
            str(db),
            "--file",
            str(request_path),
            "--context",
            context_name,
            "--query",
            scenario.query,
            "--token-budget",
            str(token_budget),
            "--system-prompt",
            EVAL_SYSTEM_PROMPT,
            "--format",
            "openai-messages",
        ]
        completed = subprocess.run(cmd, cwd=root, text=True, capture_output=True, check=False)
        if completed.returncode != 0:
            raise RuntimeError((completed.stdout + completed.stderr).strip())
        return json.loads(completed.stdout)
    finally:
        try:
            request_path.unlink(missing_ok=True)
        except OSError:
            pass


def configure_mem0(args: argparse.Namespace, api_key: str, root: Path) -> Mem0Retriever:
    from mem0 import Memory

    mem0_root = Path(args.mem0_dir)
    if not mem0_root.is_absolute():
        mem0_root = root / mem0_root
    mem0_root.mkdir(parents=True, exist_ok=True)
    qdrant_path = mem0_root / f"qdrant-{uuid.uuid4().hex}"
    history_path = mem0_root / f"history-{uuid.uuid4().hex}.db"
    qdrant_path.mkdir(parents=True, exist_ok=True)

    llm_config: dict[str, Any] = {
        "api_key": api_key,
        "model": args.mem0_llm_model,
        "temperature": 0.0,
    }
    embedder_config: dict[str, Any] = {
        "api_key": api_key,
        "model": args.mem0_embedding_model,
        "embedding_dims": args.mem0_embedding_dims,
    }
    if args.base_url:
        llm_config["openai_base_url"] = args.base_url
        embedder_config["openai_base_url"] = args.base_url

    memory = Memory.from_config(
        {
            "vector_store": {
                "provider": "qdrant",
                "config": {
                    "collection_name": f"skibidi_llm_context_{uuid.uuid4().hex}",
                    "path": str(qdrant_path),
                    "embedding_model_dims": args.mem0_embedding_dims,
                },
            },
            "llm": {"provider": "openai", "config": llm_config},
            "embedder": {"provider": "openai", "config": embedder_config},
            "history_db_path": str(history_path),
        }
    )
    return Mem0Retriever(memory, qdrant_path, history_path, f"skibidi-llm-{uuid.uuid4().hex}")


def close_mem0_handles(memory: Any) -> None:
    vector_store = getattr(memory, "vector_store", None)
    targets = [
        getattr(vector_store, "client", None),
        vector_store,
        getattr(memory, "db", None),
        memory,
    ]
    seen: set[int] = set()
    for target in targets:
        if target is None or id(target) in seen:
            continue
        seen.add(id(target))
        close = getattr(target, "close", None)
        if callable(close):
            try:
                close()
            except Exception:
                pass
    gc.collect()


def cleanup_mem0(retriever: Mem0Retriever) -> None:
    close_mem0_handles(retriever.memory)
    shutil.rmtree(retriever.qdrant_path, ignore_errors=True)
    for path in [
        retriever.history_path,
        retriever.history_path.with_suffix(retriever.history_path.suffix + "-wal"),
        retriever.history_path.with_suffix(retriever.history_path.suffix + "-shm"),
    ]:
        try:
            path.unlink(missing_ok=True)
        except OSError:
            pass


def render_mem0_result(memory: Any, query: str, user_id: str, top_k: int) -> str:
    raw = memory.search(query, filters={"user_id": user_id}, top_k=top_k)
    if isinstance(raw, dict):
        results = raw.get("results", raw.get("memories", []))
    else:
        results = raw
    lines: list[str] = []
    for item in results or []:
        if isinstance(item, dict):
            text = item.get("memory") or item.get("text") or item.get("content") or str(item)
        else:
            text = getattr(item, "memory", None) or getattr(item, "text", None) or str(item)
        lines.append(str(text))
    return "\n".join(lines)


def mem0_messages(scenario: Scenario, retriever: Mem0Retriever, top_k: int) -> list[dict[str, str]]:
    user_id = f"{retriever.user_prefix}-{scenario.name}"
    for message in scenario.messages:
        retriever.memory.add(
            [{"role": message.speaker, "content": message.text}],
            user_id=user_id,
            metadata={
                "message_id": message.message_id,
                "tab": message.tab,
                "challenge": scenario.challenge,
            },
        )
    rendered = render_mem0_result(retriever.memory, scenario.query, user_id, top_k)
    return [
        {"role": "system", "content": EVAL_SYSTEM_PROMPT},
        {"role": "system", "content": "Mem0 retrieved memories:\n" + (rendered or "(none)")},
        {"role": "user", "content": scenario.query},
    ]


def chat_completion(
    *,
    api_key: str,
    base_url: str,
    model: str,
    messages: list[dict[str, str]],
    max_tokens: int,
    timeout_s: int,
) -> tuple[str, int, int]:
    url = base_url.rstrip("/") + "/chat/completions"
    payload = {
        "model": model,
        "messages": messages,
        "temperature": 0,
        "max_tokens": max_tokens,
        "response_format": {"type": "json_object"},
    }
    request = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=timeout_s) as response:
        data = json.loads(response.read().decode("utf-8"))
    content = data["choices"][0]["message"]["content"]
    usage = data.get("usage", {})
    return content, int(usage.get("prompt_tokens", 0)), int(usage.get("completion_tokens", 0))


def grade_response(
    scenario: Scenario,
    method: str,
    answer: str,
    input_tokens: int,
    output_tokens: int,
    elapsed_s: float,
    preview: str,
) -> MethodResult:
    hits = count_hits(answer, scenario.required, scenario)
    invalidated_excluded = count_absent(answer, scenario.invalidated, scenario)
    restricted_excluded = count_absent(answer, scenario.restricted, scenario)
    ok = (
        hits == len(scenario.required)
        and invalidated_excluded == len(scenario.invalidated)
        and restricted_excluded == len(scenario.restricted)
    )
    return MethodResult(
        scenario=scenario.name,
        challenge=scenario.challenge,
        method=method,
        ok=ok,
        recall_hits=hits,
        recall_total=len(scenario.required),
        invalidated_excluded=invalidated_excluded,
        invalidated_total=len(scenario.invalidated),
        restricted_excluded=restricted_excluded,
        restricted_total=len(scenario.restricted),
        input_tokens=input_tokens,
        output_tokens=output_tokens,
        elapsed_s=elapsed_s,
        answer=answer,
        context_preview=preview,
    )


def error_result(scenario: Scenario, method: str, error: str) -> MethodResult:
    return MethodResult(
        scenario=scenario.name,
        challenge=scenario.challenge,
        method=method,
        ok=False,
        recall_hits=0,
        recall_total=len(scenario.required),
        invalidated_excluded=0,
        invalidated_total=len(scenario.invalidated),
        restricted_excluded=0,
        restricted_total=len(scenario.restricted),
        input_tokens=0,
        output_tokens=0,
        elapsed_s=0.0,
        answer="",
        error=error,
    )


def build_method_messages(
    scenario: Scenario,
    method: str,
    args: argparse.Namespace,
    *,
    root: Path,
    db_root: Path,
    mem0_retriever: Mem0Retriever | None,
) -> list[dict[str, str]]:
    if method == "skibidiql":
        return run_skibidi_messages(scenario, root=root, db_root=db_root, token_budget=args.token_budget)
    if method == "lexical_rag":
        return lexical_rag_messages(scenario, args.rag_top_k)
    if method == "mem0ai":
        if mem0_retriever is None:
            raise RuntimeError("mem0ai method requested but mem0ai is unavailable or was not configured")
        return mem0_messages(scenario, mem0_retriever, args.mem0_top_k)
    return baseline_messages(scenario, method, args.recency_messages)


def print_dry_run(selected_scenarios: list[Scenario], methods: list[str], args: argparse.Namespace) -> None:
    root = repo_root()
    db_root = root / "build" / "llm_context_quality" / "db" / f"dry-{uuid.uuid4().hex}"
    print("| Scenario | Method | Estimated input tokens |")
    print("|---|---|---:|")
    for scenario in selected_scenarios:
        for method in methods:
            if method == "mem0ai":
                print(f"| {scenario.name} | {method} | n/a; requires live mem0 extraction/search |")
                continue
            messages = build_method_messages(
                scenario,
                method,
                args,
                root=root,
                db_root=db_root,
                mem0_retriever=None,
            )
            print(f"| {scenario.name} | {method} | {estimate_tokens(messages_to_text(messages))} |")


def print_summary(results: list[MethodResult]) -> None:
    by_method: dict[str, list[MethodResult]] = {}
    for result in results:
        by_method.setdefault(result.method, []).append(result)

    print("| Method | Exact answer pass | Required fact recall | Invalidated excluded | Secrets excluded | Avg input toks | Avg output toks | Avg latency |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|")
    for method, rows in by_method.items():
        passed = sum(1 for row in rows if row.ok)
        hits = sum(row.recall_hits for row in rows)
        total = sum(row.recall_total for row in rows)
        invalidated_excluded = sum(row.invalidated_excluded for row in rows)
        invalidated_total = sum(row.invalidated_total for row in rows)
        restricted_excluded = sum(row.restricted_excluded for row in rows)
        restricted_total = sum(row.restricted_total for row in rows)
        avg_input = sum(row.input_tokens for row in rows) / len(rows)
        avg_output = sum(row.output_tokens for row in rows) / len(rows)
        avg_latency = sum(row.elapsed_s for row in rows) / len(rows)
        recall = 100.0 * hits / total if total else 100.0
        invalidated = 100.0 * invalidated_excluded / invalidated_total if invalidated_total else 100.0
        restricted = 100.0 * restricted_excluded / restricted_total if restricted_total else 100.0
        print(
            f"| {method} | {passed}/{len(rows)} | {recall:.1f}% | {invalidated:.1f}% | "
            f"{restricted:.1f}% | {avg_input:.1f} | {avg_output:.1f} | {avg_latency:.2f}s |"
        )


def print_examples(results: list[MethodResult], limit: int) -> None:
    if limit <= 0:
        return
    shown = 0
    print()
    print("### Example real-LLM answers")
    for result in results:
        if shown >= limit:
            break
        if result.error:
            continue
        status = "pass" if result.ok else "fail"
        print()
        print(f"- `{result.scenario}` / `{result.method}` ({status})")
        print(f"  - context: {compact(result.context_preview, 260)}")
        print(f"  - answer: {compact(result.answer, 320)}")
        shown += 1


def write_jsonl(results: list[MethodResult], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        for result in results:
            handle.write(json.dumps(result.__dict__, ensure_ascii=False) + "\n")


def run(args: argparse.Namespace) -> int:
    selected_scenarios = scenarios()[: args.scenarios]
    methods = method_names(args.methods)
    unknown = sorted(set(methods) - SUPPORTED_METHODS)
    if unknown:
        print(f"ERROR: unknown method(s): {', '.join(unknown)}", file=sys.stderr)
        return 2

    if args.dry_run:
        print_dry_run(selected_scenarios, methods, args)
        return 0

    root = repo_root()
    api_key = load_api_key(root, args.env_file)
    if not api_key:
        print("SKIP: OPENAI_API_KEY is not set; live LLM benchmark was not run.")
        if args.env_file:
            print(f"Checked process env and {args.env_file}.")
        print("Run with --dry-run to inspect prompt sizes without making API calls.")
        return 0

    db_root = root / "build" / "llm_context_quality" / "db" / f"live-{uuid.uuid4().hex}"
    results: list[MethodResult] = []
    mem0_retriever: Mem0Retriever | None = None
    mem0_error = ""
    if "mem0ai" in methods:
        try:
            mem0_retriever = configure_mem0(args, api_key, root)
        except ImportError as exc:
            mem0_error = f"mem0ai/openai is not installed ({exc})"
        except Exception as exc:
            mem0_error = f"mem0ai setup failed ({exc})"

    calls = 0
    try:
        for scenario in selected_scenarios:
            for method in methods:
                if method == "mem0ai" and mem0_error:
                    results.append(error_result(scenario, method, mem0_error))
                    continue
                if calls >= args.max_calls:
                    print(f"Stopped at --max-calls={args.max_calls}.")
                    print_summary(results)
                    print_examples(results, args.show_examples)
                    if args.jsonl:
                        write_jsonl(results, Path(args.jsonl))
                    return 0
                try:
                    messages = build_method_messages(
                        scenario,
                        method,
                        args,
                        root=root,
                        db_root=db_root,
                        mem0_retriever=mem0_retriever,
                    )
                    preview = context_preview(messages)
                    start = time.perf_counter()
                    answer, prompt_tokens, completion_tokens = chat_completion(
                        api_key=api_key,
                        base_url=args.base_url,
                        model=args.model,
                        messages=messages,
                        max_tokens=args.max_tokens,
                        timeout_s=args.timeout,
                    )
                    elapsed_s = time.perf_counter() - start
                    input_tokens = prompt_tokens or estimate_tokens(messages_to_text(messages))
                    results.append(
                        grade_response(
                            scenario,
                            method,
                            answer,
                            input_tokens,
                            completion_tokens,
                            elapsed_s,
                            preview,
                        )
                    )
                except (
                    urllib.error.HTTPError,
                    urllib.error.URLError,
                    TimeoutError,
                    RuntimeError,
                    json.JSONDecodeError,
                    OSError,
                ) as exc:
                    results.append(error_result(scenario, method, str(exc)))
                calls += 1
    finally:
        if mem0_retriever is not None and not args.keep_mem0:
            cleanup_mem0(mem0_retriever)

    print_summary(results)
    print_examples(results, args.show_examples)
    failures = [result for result in results if result.error]
    for failure in failures:
        print(f"ERROR {failure.method}/{failure.scenario}: {failure.error}", file=sys.stderr)
    if args.jsonl:
        write_jsonl(results, Path(args.jsonl))
    return 1 if failures else 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL)
    parser.add_argument("--methods", default=DEFAULT_METHODS)
    parser.add_argument("--scenarios", type=int, default=3)
    parser.add_argument("--recency-messages", type=int, default=8)
    parser.add_argument("--rag-top-k", type=int, default=8)
    parser.add_argument("--token-budget", type=int, default=700)
    parser.add_argument("--max-tokens", type=int, default=120)
    parser.add_argument("--timeout", type=int, default=45)
    parser.add_argument("--max-calls", type=int, default=9)
    parser.add_argument(
        "--show-examples",
        type=int,
        default=3,
        help="Print this many README-friendly example answers after the summary.",
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--env-file",
        default=DEFAULT_ENV_FILE,
        help="Optional ignored env file to read OPENAI_API_KEY from when the process env is unset.",
    )
    parser.add_argument("--mem0-dir", default="build/llm_context_quality/mem0")
    parser.add_argument("--keep-mem0", action="store_true")
    parser.add_argument("--mem0-top-k", type=int, default=8)
    parser.add_argument("--mem0-llm-model", default=os.environ.get("MEM0_LLM_MODEL", DEFAULT_MODEL))
    parser.add_argument(
        "--mem0-embedding-model",
        default=os.environ.get("MEM0_EMBEDDING_MODEL", "text-embedding-3-small"),
    )
    parser.add_argument("--mem0-embedding-dims", type=int, default=1536)
    parser.add_argument("--jsonl", help="Optional path for per-call JSONL results")
    return parser.parse_args()


if __name__ == "__main__":
    raise SystemExit(run(parse_args()))
