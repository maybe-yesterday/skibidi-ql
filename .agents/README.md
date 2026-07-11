# SkibidiQL agent runtime guide

Read this if you are an AI agent using this repo.

SkibidiQL is the context database. Your model prompt should receive only the
active context SkibidiQL selects for the current user turn. Do not replay the
whole chat history and pray. Context is data; the prompt is just a view.

## The blessed runtime loop

For every non-trivial model call:

1. Append durable facts, decisions, corrections, tasks, and tool traces as
   SkibidiQL context rows.
2. Run active recall for the latest user message.
3. Send only the returned context pack plus the current user message to the
   LLM API.
4. After answering, write back any new durable state.

The helper does steps 2 and 3 for API callers:

```powershell
.\.venv\Scripts\python.exe -B .agents\skibidi_context.py `
  --fresh `
  --db build\agent_quickstart_db `
  --file .agents\examples\first-turn.skql `
  --query 'where am I and what should I not reveal?' `
  --format openai-messages
```

Output shape:

```json
[
  {"role": "system", "content": "You are a helpful coding agent."},
  {"role": "system", "content": "SkibidiQL active context:\n..."},
  {"role": "user", "content": "where am I and what should I not reveal?"}
]
```

Pass that JSON as the `messages` array to an LLM API. Raw rows, invalidation
receipts, and redaction counts stay in the controller/debug layer unless you
are explicitly debugging retrieval.

## Active recall as a tool call

If your agent has a tool interface, call SkibidiQL as `active_recall`:

```powershell
.\.venv\Scripts\python.exe -B .agents\skibidi_context.py `
  --fresh `
  --db build\agent_quickstart_db `
  --file .agents\examples\first-turn.skql `
  --query 'where am I and what should I not reveal?' `
  --format active-recall
```

Tool result shape:

```json
{
  "tool": "skibidiql.active_recall",
  "context_name": "agent_memory",
  "query": "where am I and what should I not reveal?",
  "context": "fact user_location=NYC @message_4 ...",
  "view_atoms": ["fact user_location=NYC @message_4"],
  "invalidated_receipts": ["user_location=Seattle @message_3 invalidated_by=message_4"],
  "token_cost": 42,
  "redacted_atoms": 1,
  "access_policy": "..."
}
```

Only `context` belongs in the normal LLM prompt. `invalidated_receipts` are
receipts, not facts. Treat them like provenance logs with a tiny “do not eat”
sticker.

## Seed/update files versus user-turn queries

`.skql` files under `.agents/examples/` are seed/update files. They create a
context and append memory rows. They intentionally do not contain a baked-in
`spill-context` query.

When you pass both `--file` and `--query`, the helper runs the seed/update file
and appends a fresh `spill-context` for the current user message. If `--context`
is omitted, it uses the first `manifest-context` in the file.

```powershell
.\.venv\Scripts\python.exe -B .agents\skibidi_context.py `
  --fresh `
  --db build\skibidi_project_agent_db `
  --file .agents\examples\skibidi-project-context.skql `
  --query 'polish agent integration using actual project facts' `
  --format openai-messages
```

For persistent memory, drop `--fresh` and reuse the same `--db` directory:

```powershell
.\.venv\Scripts\python.exe -B .agents\skibidi_context.py `
  --db .skibidi_agent_ctx `
  --query 'latest user task' `
  --format active-recall
```

Use `--format raw` only for debugging rows:

```powershell
.\.venv\Scripts\python.exe -B .agents\skibidi_context.py `
  --db .skibidi_agent_ctx `
  --file request.skql `
  --format raw
```

## Windows and Python commands

Preferred from repo root:

```powershell
.\.venv\Scripts\python.exe -B .agents\skibidi_context.py --help
```

Fallbacks:

```powershell
py -3 -B .agents\skibidi_context.py --help
python -B .agents\skibidi_context.py --help
```

If Python is unavailable, call the binary directly and parse
`field=current_context | value=...`:

```powershell
build\codex_skibidi_agent.exe --db build\scratch_ctx --file .agents\examples\first-turn.skql
build\codex_skibidi.exe --db build\scratch_ctx --file .agents\examples\first-turn.skql
build\skibidi.exe --db build\scratch_ctx --file .agents\examples\first-turn.skql
```

Prefer the newest ContextQL-capable binary. If a binary fails on
`manifest-context` with `Expected statement keyword`, it is stale; rebuild or
use `build\codex_skibidi_agent.exe`.

## Write memories that retrieve well

Initialize a context once:

```skql
manifest-context agent_memory;
```

Append durable events with stable message IDs:

```skql
yeet-memory agent_memory drip
    (1001, 'user', 'I prefer concise benchmark summaries.')
vibe-tab 'preferences';
```

Use explicit tabs when the topic matters:

```skql
yeet-memory agent_memory drip
    (1002, 'assistant', 'decision: README central message is context is data.')
vibe-tab 'project roadmap';
```

When the user corrects a fact, append the correction. Do not rewrite history:

```skql
yeet-memory agent_memory drip
    (1003, 'user', 'I live in Seattle.')
vibe-tab 'user facts';

yeet-memory agent_memory drip
    (1004, 'user', 'Actually I moved to NYC.')
vibe-tab 'user facts';
```

SkibidiQL keeps the NYC location active and emits Seattle as an invalidated
receipt.

Extractor-friendly memory shapes:

```text
I live in ...
I moved to ...
I prefer ...
remember that ...
always ...
never ...
do not ...
I need ...
todo: ...
debug this later: ...
decision: ...
we decided ...
```

Credential-shaped facts such as `password`, `secret`, `api key`, `access
token`, `bearer token`, `ssn`, `credit card`, `confidential`, and `private key`
are labeled and redacted in rendered context.

## Topic tabs

Tabs are explicit topic namespaces. They keep retrieval smaller and make
corrections local to the right topic.

```skql
show-tabs agent_memory;
alias-tab agent_memory 'perf' to 'debugging sqlite perf';
merge-tabs agent_memory 'sqlite' into 'debugging sqlite perf';
vibe-tab agent_memory message 1002 'project roadmap';
```

Good long-running-agent tabs:

- `project identity`
- `agent integration`
- `debugging sqlite perf`
- `benchmarks`
- `docs`
- `open questions`
- `user preferences`

Use broad tabs for routing and specific atom keys inside the memory text. That
combo makes multi-label retrieval less cursed.

## Debug retrieval

When context looks missing, stale, or too spicy, run an explanation request:

```skql
explain-context agent_memory
only-if 'why did the README benchmark context get selected?'
token-budget 800
receipts on;
```

Check:

- `ranked_atom`: what the optimizer considered
- `pruned_invalidated_atoms`: stale facts removed before prompting
- `redacted_atoms`: facts hidden by access policy
- `optimizer_saved_tokens`: avoided prompt bloat
- `context_indexes`: fields used for selection
- `provenance_model`: source tracking behavior

## Hard rules

- Use SkibidiQL before every non-trivial model call.
- Pass active `context`, not full conversation history, as durable memory.
- Never treat `invalidated_receipts` as active facts.
- Never reveal redacted raw values.
- Append corrections instead of mutating old facts.
- Use stable message IDs.
- Use tabs for topic switches.
- If the context pack is empty, say no relevant durable memory was retrieved.

Tiny vibe check: SkibidiQL stores the lore; `spill-context` hands the model the
canon. Do not make the model dig through ancient cursed scrolls.
