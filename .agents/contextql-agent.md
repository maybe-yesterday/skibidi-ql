# ContextQL agent memory playbook

Use SkibidiQL as the agent's context runtime instead of pasting the whole raw
conversation into the model prompt.

## 1. Keep a persistent context database

Use one database directory per agent/workspace:

```bash
build/skibidi --db .skibidi_agent_ctx --file agent_context.skql
```

On Windows builds the binary may be `build\skibidi.exe`,
`build\Release\skibidi.exe`, or a locally compiled `build\codex_skibidi.exe`.

## 2. Initialize once

```skql
manifest-context agent_memory;
show-context-schemas;
```

## 3. Append every useful memory event

Give each message a stable monotonically increasing ID. Use `vibe-tab auto`
when the agent is unsure which topic bucket should own the memory.

```skql
yeet-memory agent_memory drip
    (1001, 'user', 'I prefer concise benchmark summaries.')
vibe-tab auto;
```

Useful message shapes:

- user preferences and constraints;
- decisions;
- open questions;
- debug-follow-up notes;
- tool results worth remembering;
- corrections that should invalidate older facts.

## 4. Before each model call, render context

Ask SkibidiQL for a token-budgeted maintained view:

```skql
spill-context agent_memory
only-if 'current task or question here'
token-budget 1200
receipts on;
```

Put the `current_context` row into the model context. Keep `view_atom` and
`invalidated` rows available for debugging/provenance.

## 5. Debug the prompt plan

When the context looks weird or expensive, inspect the prompt-view plan:

```skql
explain-context agent_memory
only-if 'current task or question here'
token-budget 1200
receipts on;
```

Look at `pruned_invalidated_atoms`, `optimizer_saved_tokens`,
`ranked_atom`, `context_indexes`, and `provenance_model`.

## 6. Maintain topic tabs

```skql
show-tabs agent_memory;
alias-tab agent_memory 'perf' to 'debugging sqlite perf';
merge-tabs agent_memory 'sqlite' into 'debugging sqlite perf';
show-context-objects agent_memory;
```

The agent should prefer querying a resolved `vibe-tab` when the task is scoped,
because tab filtering reduces prompt noise and makes invalidation local to that
topic.
