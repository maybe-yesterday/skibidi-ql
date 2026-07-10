#!/usr/bin/env python3
"""Optional real Mem0 quality benchmark.

This script intentionally lives outside the default C++ benchmark because it
uses external services. It requires:

    pip install mem0ai openai
    OPENAI_API_KEY=...

It uses the real `mem0ai` package, local Qdrant storage under `.mem0/`, and
OpenAI for extraction/embedding through Mem0's provider interface.
"""

from __future__ import annotations

import argparse
import gc
import os
import shutil
import sys
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Message:
    message_id: int
    speaker: str
    text: str
    tab: str = ""


@dataclass
class Scenario:
    name: str
    challenge: str
    query: str
    messages: list[Message]
    required: list[str] = field(default_factory=list)
    invalidated: list[str] = field(default_factory=list)
    restricted: list[str] = field(default_factory=list)


@dataclass
class Stats:
    scenarios: int = 0
    required: int = 0
    hits: int = 0
    invalidated: int = 0
    invalidated_excluded: int = 0
    restricted: int = 0
    restricted_excluded: int = 0
    tokens: int = 0
    elapsed_s: float = 0.0


def estimate_tokens(text: str) -> int:
    return max(1, (len(text) + 3) // 4)


def lower(text: str) -> str:
    return text.lower()


def count_hits(text: str, needles: list[str]) -> int:
    haystack = lower(text)
    return sum(1 for needle in needles if needle and lower(needle) in haystack)


def count_absent(text: str, needles: list[str]) -> int:
    haystack = lower(text)
    return sum(1 for needle in needles if not needle or lower(needle) not in haystack)


def add_filler(messages: list[Message], base: int, target: int) -> None:
    fillers = [
        "assistant summarized unrelated calendar chatter",
        "user asked about build output and test names",
        "assistant discussed travel planning without durable facts",
        "user switched to UI polish notes for a moment",
        "assistant compared cache warmup strategies",
        "user mentioned a random movie recommendation",
        "assistant noted benchmark methodology in passing",
        "user bounced to docs wording and back again",
    ]
    while len(messages) < target:
        idx = len(messages)
        messages.append(
            Message(
                base + 1000 + idx + 1,
                "assistant" if idx % 2 == 0 else "user",
                f"Filler turn {base + 1000 + idx + 1}: {fillers[idx % len(fillers)]}.",
            )
        )


def make_scenario(index: int, target_messages: int) -> Scenario:
    flavor = index % 11
    variant = index // 11
    base = (index + 1) * 10000
    old_cities = ["Seattle", "Austin", "Boston", "Chicago", "Miami"]
    new_cities = ["Denver", "NYC", "Portland", "Atlanta", "Phoenix"]
    dog_names = ["Mochi", "Biscuit", "Nori", "Pixel", "Taco"]
    messages: list[Message] = []

    def msg(offset: int, text: str, tab: str = "") -> None:
        messages.append(Message(base + offset, "user", text, tab))

    if flavor == 0:
        old = old_cities[variant % len(old_cities)]
        new = new_cities[variant % len(new_cities)]
        msg(1, f"I live in {old}.")
        msg(2, "I prefer quiet restaurants.", "food")
        msg(24, f"Actually I moved to {new}.")
        scenario = Scenario(
            f"restaurant_location_{variant + 1}",
            "contradictions + stale location",
            "Find restaurants near me",
            messages,
            [new.lower(), "quiet restaurants"],
            [old.lower()],
        )
    elif flavor == 1:
        msg(1, "I prefer spicy restaurants.", "food")
        msg(20, "I prefer vegan restaurants.", "food")
        scenario = Scenario(
            f"food_preference_update_{variant + 1}",
            "stale preferences",
            "recommend a restaurant",
            messages,
            ["vegan restaurants"],
            ["spicy restaurants"],
        )
    elif flavor == 2:
        dog = dog_names[variant % len(dog_names)]
        msg(1, f"My dog is named {dog}.")
        msg(2, "My dog likes salmon.")
        scenario = Scenario(
            f"dog_topic_tab_{variant + 1}",
            "topic switches + tab retrieval",
            "what does my dog like and what is my dog name?",
            messages,
            [dog.lower(), "salmon"],
        )
    elif flavor == 3:
        msg(1, "My dog likes salmon.")
        msg(30, "My dog likes chicken.")
        scenario = Scenario(
            f"dog_preference_update_{variant + 1}",
            "contradictions inside a topic tab",
            "what does my dog like?",
            messages,
            ["chicken"],
            ["salmon"],
        )
    elif flavor == 4:
        msg(1, "I prefer dark mode.", "ui")
        msg(2, "I prefer quiet restaurants.", "food")
        msg(26, "I like cat cafes.", "pets")
        scenario = Scenario(
            f"stable_preference_cross_topic_{variant + 1}",
            "stable preferences + topic switches",
            "what restaurant preference should I remember?",
            messages,
            ["quiet restaurants"],
        )
    elif flavor == 5:
        msg(1, "Debug this later: sqlite perf join misses.")
        scenario = Scenario(
            f"debug_followup_{variant + 1}",
            "debug/task continuity",
            "what debug todo should I investigate?",
            messages,
            ["sqlite perf join misses"],
        )
    elif flavor == 6:
        msg(1, "I need write README benchmark section.")
        scenario = Scenario(
            f"current_task_{variant + 1}",
            "long-running task state",
            "what task do I need todo?",
            messages,
            ["write readme benchmark section"],
        )
    elif flavor == 7:
        msg(1, "Never share password hunter2 or api key tokens.")
        scenario = Scenario(
            f"acl_constraint_{variant + 1}",
            "ACL-restricted facts",
            "what security constraint should I follow?",
            messages,
            ["password", "api key"],
            [],
            ["hunter2", "api key tokens"],
        )
    elif flavor == 8:
        msg(1, "Remember that secret launch code is ORCA-77.")
        scenario = Scenario(
            f"confidential_memory_{variant + 1}",
            "ACL-restricted facts",
            "what confidential constraint should I remember?",
            messages,
            ["secret", "launch code"],
            [],
            ["orca-77"],
        )
    elif flavor == 9:
        msg(1, "Decision: context is data.", "project roadmap")
        msg(2, "Can we optimize vectorized execution?", "project roadmap")
        scenario = Scenario(
            f"open_question_decision_{variant + 1}",
            "open questions + decisions",
            "what open question or decision is active?",
            messages,
            ["context is data", "optimize vectorized execution"],
        )
    else:
        for note in range(24):
            msg(note + 1, f"I prefer durable but irrelevant note {note + 1}.", "profile-noise")
        msg(50, "I prefer quiet restaurants.", "food")
        scenario = Scenario(
            f"summary_compression_loss_{variant + 1}",
            "summary compression loss",
            "what restaurant preference should I remember?",
            messages,
            ["quiet restaurants"],
        )

    add_filler(scenario.messages, base, target_messages)
    return scenario


def render_mem0_result(memory: object, query: str, user_id: str, top_k: int) -> str:
    # mem0 >= 2.x rejects top-level entity params on search(); scope via
    # filters so scenarios do not leak memories into each other.
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


def configure_mem0(args: argparse.Namespace):
    os.environ.setdefault("MEM0_DIR", str(Path(".mem0").resolve()))
    from mem0 import Memory

    mem0_root = Path(args.mem0_dir)
    qdrant_path = mem0_root / f"qdrant-{uuid.uuid4().hex}"
    history_path = mem0_root / f"history-{uuid.uuid4().hex}.db"
    qdrant_path.mkdir(parents=True, exist_ok=True)
    openai_base_url = os.environ.get("MEM0_OPENAI_BASE_URL") or os.environ.get("OPENAI_BASE_URL")
    llm_config = {
        "api_key": os.environ["OPENAI_API_KEY"],
        "model": args.llm_model,
        "temperature": 0.0,
    }
    embedder_config = {
        "api_key": os.environ["OPENAI_API_KEY"],
        "model": args.embedding_model,
        "embedding_dims": args.embedding_dims,
    }
    if openai_base_url:
        llm_config["openai_base_url"] = openai_base_url
        embedder_config["openai_base_url"] = openai_base_url

    config = {
        "vector_store": {
            "provider": "qdrant",
            "config": {
                "collection_name": f"skibidi_mem0_{uuid.uuid4().hex}",
                "path": str(qdrant_path),
                "embedding_model_dims": args.embedding_dims,
            },
        },
        "llm": {
            "provider": "openai",
            "config": llm_config,
        },
        "embedder": {
            "provider": "openai",
            "config": embedder_config,
        },
        "history_db_path": str(history_path),
    }
    return Memory.from_config(config), qdrant_path, history_path


def close_mem0_handles(memory: object) -> None:
    """Best-effort handle release for Windows local-Qdrant/SQLite cleanup."""
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


def cleanup_mem0_paths(memory: object, qdrant_path: Path, history_path: Path) -> list[str]:
    close_mem0_handles(memory)
    warnings: list[str] = []

    def unlink_best_effort(path: Path) -> None:
        try:
            path.unlink(missing_ok=True)
        except TypeError:
            if not path.exists():
                return
            try:
                path.unlink()
            except PermissionError as exc:
                warnings.append(f"cleanup_warning=kept_locked_file:{path} ({exc})")
            except OSError as exc:
                warnings.append(f"cleanup_warning=kept_file:{path} ({exc})")
        except PermissionError as exc:
            warnings.append(f"cleanup_warning=kept_locked_file:{path} ({exc})")
        except OSError as exc:
            warnings.append(f"cleanup_warning=kept_file:{path} ({exc})")

    try:
        shutil.rmtree(qdrant_path, ignore_errors=False)
    except FileNotFoundError:
        pass
    except PermissionError as exc:
        warnings.append(f"cleanup_warning=kept_locked_qdrant_path:{qdrant_path} ({exc})")
    except OSError as exc:
        warnings.append(f"cleanup_warning=kept_qdrant_path:{qdrant_path} ({exc})")

    sidecars = [
        history_path,
        history_path.with_suffix(history_path.suffix + "-wal"),
        history_path.with_suffix(history_path.suffix + "-shm"),
    ]
    for path in sidecars:
        unlink_best_effort(path)
    return warnings


def run(args: argparse.Namespace) -> int:
    if not os.environ.get("OPENAI_API_KEY"):
        print("SKIP: OPENAI_API_KEY is not set; real mem0ai benchmark needs an LLM/embedder API key.")
        print("Install deps with: python -m pip install mem0ai openai")
        return 0
    try:
        memory, qdrant_path, history_path = configure_mem0(args)
    except ImportError as exc:
        print(f"SKIP: mem0ai/openai not installed ({exc}).")
        print("Install deps with: python -m pip install mem0ai openai")
        return 0

    scenarios = [make_scenario(i, args.scenario_messages) for i in range(args.scenarios)]
    stats = Stats()
    start = time.perf_counter()
    user_prefix = f"skibidi-mem0-{uuid.uuid4().hex}"

    for idx, scenario in enumerate(scenarios):
        user_id = f"{user_prefix}-{idx}"
        for message in scenario.messages:
            memory.add(
                [{"role": message.speaker, "content": message.text}],
                user_id=user_id,
                metadata={"message_id": message.message_id, "tab": message.tab, "challenge": scenario.challenge},
            )
        rendered = render_mem0_result(memory, scenario.query, user_id, args.top_k)
        stats.scenarios += 1
        stats.required += len(scenario.required)
        stats.hits += count_hits(rendered, scenario.required)
        stats.invalidated += len(scenario.invalidated)
        stats.invalidated_excluded += count_absent(rendered, scenario.invalidated)
        stats.restricted += len(scenario.restricted)
        stats.restricted_excluded += count_absent(rendered, scenario.restricted)
        stats.tokens += estimate_tokens(rendered)

    stats.elapsed_s = time.perf_counter() - start
    recall = 100.0 * stats.hits / stats.required if stats.required else 100.0
    invalidated_excluded = 100.0 * stats.invalidated_excluded / stats.invalidated if stats.invalidated else 100.0
    restricted_excluded = 100.0 * stats.restricted_excluded / stats.restricted if stats.restricted else 100.0
    avg_tokens = stats.tokens / stats.scenarios if stats.scenarios else 0.0

    print(f"real_mem0=true scenarios={stats.scenarios} avg_tokens={avg_tokens:.1f} elapsed_s={stats.elapsed_s:.1f}")
    print("| Method | Policy-safe active recall | Avg tokens | Invalidated excluded | ACL raw excluded |")
    print("|---|---:|---:|---:|---:|")
    print(f"| mem0ai | {recall:.1f}% | {avg_tokens:.1f} | {invalidated_excluded:.1f}% | {restricted_excluded:.1f}% |")

    if args.keep_mem0:
        print(f"kept_mem0_dir={qdrant_path.parent}")
    else:
        for warning in cleanup_mem0_paths(memory, qdrant_path, history_path):
            print(warning, file=sys.stderr)
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenarios", type=int, default=11)
    parser.add_argument("--scenario-messages", type=int, default=40)
    parser.add_argument("--top-k", type=int, default=10)
    parser.add_argument("--mem0-dir", default=".mem0")
    parser.add_argument("--keep-mem0", action="store_true")
    parser.add_argument("--llm-model", default=os.environ.get("MEM0_LLM_MODEL", "gpt-4o-mini"))
    parser.add_argument(
        "--embedding-model",
        default=os.environ.get("MEM0_EMBEDDING_MODEL", "text-embedding-3-small"),
    )
    parser.add_argument("--embedding-dims", type=int, default=1536)
    return parser.parse_args()


if __name__ == "__main__":
    raise SystemExit(run(parse_args()))
