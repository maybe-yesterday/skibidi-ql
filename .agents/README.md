# SkibidiQL agent integration guide

Read this if you are an AI agent using this repository.

SkibidiQL is the context database. The model prompt is only the compiled
context pack that SkibidiQL returns.

Do not paste the whole conversation into the model. Store useful context as
rows, query the active rows, then pass only the `current_context` result to the
LLM. Context is data; the prompt is just a view.

## Fast path

From the repo root, run the demo request against a fresh DB:

```powershell
python .agents\skibidi_context.py --fresh --db build\agent_quickstart_db --file .agents\examples\first-turn.skql --prompt
```

Expected shape:

```text
constraint user_constraint=[redacted:CONFIDENTIAL_CUSTOMER_DATA] @message_5 ...
decision decision=SkibidiQL central message is context is data @message_2 ...
fact user_location=NYC @message_4 ...
preference user_preference=concise benchmark summaries @message_1 ...
```

That output is the model-facing memory pack wrapped in the recommended prompt
section. Put it into the LLM prompt. Keep raw rows, receipts, and invalidated
facts in the controller/debug layer.

To dogfood against this actual project instead of the tiny example:

```powershell
python .agents\skibidi_context.py --fresh --db build\skibidi_project_agent_db --file .agents\examples\skibidi-project-context.skql --prompt
```

## The contract

For each agent turn:

1. Write durable context events into SkibidiQL.
2. Query SkibidiQL for task-relevant active context.
3. Put only the rendered context pack into the LLM prompt.
4. After the answer, write back durable new facts, decisions, corrections,
   unresolved tasks, and debug follow-ups.

The LLM never needs raw tables. It needs a small, policy-safe pack like:

```text
Active context from SkibidiQL:
- preference user_preference=concise benchmark summaries @message_1001
- decision decision=README central message is context is data @message_1009
- constraint user_constraint=never expose api key tokens @message_1010

Do not treat invalidated receipt rows as active facts.
```

## Run SkibidiQL as the context tool

Use one DB directory per workspace, agent, or conversation family:

```powershell
python .agents\skibidi_context.py --db .skibidi_agent_ctx --file agent_context.skql
```

For direct binary integration:

```bash
build/skibidi --db .skibidi_agent_ctx --file agent_context.skql
```

Windows binaries may be named one of:

```powershell
build\codex_skibidi.exe --db .skibidi_agent_ctx --file agent_context.skql
build\skibidi.exe --db .skibidi_agent_ctx --file agent_context.skql
build\Release\skibidi.exe --db .skibidi_agent_ctx --file agent_context.skql
```

Prefer the newest ContextQL-capable binary. If a binary fails on
`manifest-context` with `Expected statement keyword`, it is stale; use
`build\codex_skibidi.exe` or rebuild before continuing.

If you are generating one-off requests, write a small `.skql` file with the
commands below, run the binary, parse the table rows, and discard the request
file if it was temporary.

The helper supports:

```powershell
python .agents\skibidi_context.py --db .skibidi_agent_ctx --file request.skql --prompt
python .agents\skibidi_context.py --db .skibidi_agent_ctx --file request.skql --prompt --last
python .agents\skibidi_context.py --db .skibidi_agent_ctx --file request.skql --context-only
python .agents\skibidi_context.py --db .skibidi_agent_ctx --file request.skql --context-only --field view_atom
python .agents\skibidi_context.py --fresh --db build\scratch_ctx --file request.skql --prompt
```

`--prompt` wraps `current_context` in the recommended LLM prompt block.
`--context-only` prints only selected field values. `--last` uses only the last
matching row, which is handy when a request file contains multiple
`spill-context` calls. `--fresh` deletes the DB path first, but only for DB
paths inside this repository; use it for demos and repeatable dogfood runs, not
persistent memory.

## Initialize a context once

```skql
manifest-context agent_memory;
show-context-schemas;
```

Use a stable context name. For example:

- `agent_memory` for one long-running local agent
- `convo_123` for one conversation
- `workspace_wireless` for one project workspace

## Append memory events

Use monotonically increasing message IDs. Duplicate IDs are rejected.

```skql
yeet-memory agent_memory drip
    (1001, 'user', 'I prefer concise benchmark summaries.')
vibe-tab auto;
```

Use `vibe-tab auto` when unsure. Use an explicit tab when the topic is known:

```skql
yeet-memory agent_memory drip
    (1002, 'assistant', 'decision: README central message is context is data.')
vibe-tab 'project roadmap';
```

The auto-tab classifier is intentionally lightweight. For important agent
writebacks, prefer explicit tabs; words like `benchmark`, `perf`, or
`dogfood` can otherwise route to a nearby demo-flavored tab.

When the user corrects something, append the correction. Do not delete or
rewrite old memory:

```skql
yeet-memory agent_memory drip
    (1003, 'user', 'I live in Seattle.');

yeet-memory agent_memory drip
    (1004, 'user', 'Actually I moved to NYC.');
```

SkibidiQL will keep `user_location=NYC` active and mark the Seattle fact as
invalidated within the same tab.

## Write extractor-friendly memories

Current ContextQL recognizes these useful shapes. If a fact matters, write it
back in one of these forms instead of vague prose.

```text
I live in ...
I moved to ...
I am in ...
my location is ...

I prefer ...
I like ...

remember that ...
always ...
never ...
do not ...
don't ...

I need ...
I want ...
todo: ...
debug this later: ...
investigate ...
look into ...

decision: ...
we decided ...
final call: ...

Any message ending in ? becomes an open_question atom.
```

Messages containing credential-shaped phrases such as `password`, `secret`,
`api key`, `api token`, `access token`, `bearer token`, `ssn`, `credit card`,
`confidential`, or `private key` are labeled `CONFIDENTIAL_CUSTOMER_DATA` and
redacted in rendered context. Generic project metrics like "average tokens"
should stay visible.

## Query before every model call

Use `spill-context` immediately before asking the LLM to answer. The
`only-if` string should be the current user task, not a generic query.

```skql
spill-context agent_memory
only-if 'explain how an actual AI model uses SkibidiQL context'
token-budget 800
receipts on;
```

If the task is clearly about one topic, add a tab filter:

```skql
spill-context agent_memory
vibe-tab 'project roadmap'
only-if 'update the README positioning'
token-budget 800
receipts on;
```

Interpret the output like this:

- `current_context`: put this in the model prompt.
- `view_atom`: same selected context, split into individual rows for parsing.
- `invalidated`: receipts only. Never use these as true facts.
- `redacted_atoms`: count of facts hidden by access policy.
- `token_cost`: approximate size of the pack.
- `access_policy`: labels and redaction mode used for this render.

If `current_context` is empty, tell the model that no durable context was
retrieved. Do not hallucinate memory.

Raw output rows use this stable shape:

```text
field=current_context | value=...
field=view_atom | value=...
field=invalidated | value=...
```

If you are not using `.agents/skibidi_context.py`, parse only rows whose field
is `current_context` for ordinary prompt construction.

## Prompt the LLM with a context pack

Recommended prompt section:

```text
SkibidiQL active context:
{current_context}

Rules:
- Treat this as the durable active memory for this turn.
- Ignore any stale fact not present here.
- Do not reveal redacted values.
- If the context is insufficient, ask or proceed from the current user message.
```

Keep receipts out of the normal model prompt unless you need provenance or
debugging. Receipts are for the agent controller; `current_context` is for the
LLM.

## Write back after the model answers

Write only durable state. Good writebacks:

- user preferences
- user constraints
- decisions
- corrections
- open questions
- active tasks
- debug follow-ups
- source/provenance summaries worth retrieving later

Avoid writing:

- throwaway chit-chat
- the whole assistant response when no durable fact changed
- hidden chain-of-thought
- raw secrets that do not need to be retrieved

Example:

```skql
yeet-memory agent_memory drip
    (1005, 'assistant', 'decision: agents should pass only current_context to the LLM.')
vibe-tab 'agent integration';
```

## Manage topic tabs

Tabs are explicit topic namespaces. They make retrieval smaller and keep
corrections local to a topic.

```skql
show-tabs agent_memory;
alias-tab agent_memory 'perf' to 'debugging sqlite perf';
merge-tabs agent_memory 'sqlite' into 'debugging sqlite perf';
vibe-tab agent_memory message 1002 'project roadmap';
show-context-objects agent_memory;
```

Use tabs for long-running threads that switch topics. Example tabs:

- `project roadmap`
- `debugging sqlite perf`
- `agent integration`
- `benchmarks`
- `docs`
- `open questions`

Invalidation is currently key + tab scoped. That is great for corrections:
`I live in Seattle` followed by `Actually I moved to NYC` in the same tab
replaces the stale location. It also means broad atom types like `decision`
can supersede earlier decisions in the same tab. Use precise tabs when facts
should coexist:

```skql
yeet-memory agent_memory drip
    (2001, 'assistant', 'decision: SkibidiQL is a relational context DB.')
vibe-tab 'project identity';

yeet-memory agent_memory drip
    (2002, 'assistant', 'decision: README headline benchmark is policy-safe recall.')
vibe-tab 'benchmark headline';
```

## Debug context selection

When context looks missing, stale, too large, or sus, run:

```skql
explain-context agent_memory
only-if 'why did the README benchmark context get selected?'
token-budget 800
receipts on;
```

Check:

- `ranked_atom`: what the optimizer considered important
- `pruned_invalidated_atoms`: stale facts removed before prompting
- `redacted_atoms`: facts hidden by access policy
- `optimizer_saved_tokens`: avoided prompt bloat
- `context_indexes`: fields used for selection
- `provenance_model`: source tracking behavior

## Hard rules for agents

- Use SkibidiQL before every non-trivial model call.
- Pass `current_context`, not full conversation history, as durable memory.
- Never treat `invalidated` rows as active facts.
- Never reveal redacted raw values.
- Append corrections instead of mutating old facts.
- Use stable message IDs.
- Use tabs for topic switches.
- Prefer extractor-friendly writebacks.
- If the context pack is empty, say the DB had no relevant active memory.

Tiny vibe check: SkibidiQL stores the lore; `spill-context` hands the model the
canon. Do not make the model dig through ancient cursed scrolls.
