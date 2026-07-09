# SkibidiQL

SkibidiQL is a relational database for agent context management.

> Context is (relational) data.

Agent context should not be a giant prompt string, a pile of JSON blobs, or a
vibe-based "last N messages" window. It should be durable, queryable,
auditable, invalidatable, benchmarkable relational state.

SkibidiQL stores conversations, facts, preferences, tasks, tool traces,
provenance, access labels, topic tabs, and prompt-ready context views as data.
Agents can append context, query context, explain context, invalidate stale
context, and render only the relevant token-budgeted slice for the next model
call.

We decided into include a lot of modern lingo in this project to make this more exciting
to younger folks, appeal to general audience, and to appreciate the evolution
of language instead of avoiding change. 

Under the hood, SkibidiQL also ships a native C++17 storage/execution engine:
persistent slotted pages, heap files, B+ tree primary-key indexes, LRU buffer
pool, WAL-style rollback, process-local locking, typed/vectorized scans,
aggregates, joins, optimizer stats, and a REPL. SQLite is optional and only
used for compatibility and benchmarks.

No SQLite library is required for the default build.

## Why this exists

Most agent-memory systems treat context like prompt decoration:

```text
raw chat history -> giant prompt -> vibes
```

That gets cooked as soon as the agent needs database-shaped guarantees:

- deterministic retrieval instead of "last N messages"
- provenance for what fact came from which message
- invalidation when the user corrects themselves
- topic tabs the agent can query, merge, and normalize
- access labels and redaction
- token-budgeted context with measurable recall
- durable state that survives restarts

SkibidiQL's model is:

```text
raw messages
  -> relational context rows
  -> extracted atoms + metadata
  -> active / invalidated facts with provenance
  -> indexed topic/query slices
  -> token-budgeted model context with receipts
```

The prompt view is just an output format. The core product is the context
database.

## Quickstart

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
```

Run the REPL:

```bash
./build/skibidi
```

Run a script against a persistent native DB:

```bash
./build/skibidi --db mydata --file examples/contextql.skql
```

Useful modes:

```bash
./build/skibidi --transpile-only --file examples/hello.skql
./build/skibidi --verbose --file examples/hello.skql
./build/skibidi --cache-stats --db mydata
```

## ContextQL: relational context management

Create a context database namespace:

```skql
manifest-context convo_123;
```

Append memories. SkibidiQL stores the message as a row and extracts inspectable
semantic atoms for facts, preferences, constraints, tasks, open questions,
decisions, debug follow-ups, and a few demo-friendly pet facts.

```skql
yeet-memory convo_123 drip
    (1, 'user', 'I live in Seattle.');

yeet-memory convo_123 drip
    (88, 'user', 'Actually I moved to NYC.');
```

The second message invalidates the older `user_location=Seattle` atom and keeps
`user_location=NYC` active.

Query the context DB and render a model-context view:

```skql
spill-context convo_123
only-if 'Find restaurants near me'
token-budget 200
receipts on;
```

Example output shape:

```text
field=current_context | value=fact user_location=NYC @message_88
field=invalidated | value=user_location=Seattle @message_1 invalidated_by=message_88
field=token_cost | value=8
```

Ask why context was chosen:

```skql
explain-context convo_123
only-if 'Find restaurants near me'
token-budget 200
receipts on;
```

Use topic tabs as explicit agent-managed namespaces:

```skql
alias-tab convo_123 'dog' to 'convo about dog';

yeet-memory convo_123 drip
    (7, 'user', 'My dog likes salmon.')
vibe-tab auto;

show-tabs convo_123;

spill-context convo_123
vibe-tab 'dog'
only-if 'what does my dog like?'
token-budget 200
receipts on;
```

Agents can use the binary as their context DB instead of dragging the whole
conversation into every prompt. See [.agents/contextql-agent.md](.agents/contextql-agent.md)
for the short runtime playbook.

## Tiny language taste

SkibidiQL is SQL-like, but the syntax has brainrot seasoning:

```skql
manifest users (
    id INTEGER main-character,
    name TEXT no-cap-not ghosted,
    age INTEGER
);

yeet-into users (id, name, age)
drip (1, 'Ada', 37), (2, 'Grace', 31);

slay name, age
no-cap users
only-if age > 18
hits-different age down-bad;
```

Common mappings:

| SkibidiQL | SQL-ish meaning |
|---|---|
| `slay` | `SELECT` |
| `no-cap` | `FROM` |
| `only-if` | `WHERE` |
| `link-up` / `left-link-up` | `JOIN` / `LEFT JOIN` |
| `vibe-check` | `GROUP BY` |
| `hits-different` | `ORDER BY` |
| `yeet-into ... drip` | `INSERT INTO ... VALUES` |
| `glow-up ... be-like` | `UPDATE ... SET` |
| `ratio` | `DELETE FROM` |
| `manifest` | `CREATE TABLE` / `CREATE CONTEXT` |
| `ghosted` | `NULL` |
| `main-character` | `PRIMARY KEY` |

Native analytics include `headcount`, `stack`, `mid`, `goat`, `L`,
`biggest-W`, `mid-fr`, window `era`, and `LONE-WOLF` outlier counts.

## Native engine

SkibidiQL is not just a transpiler. The native engine exists so agent context
can be stored and queried without depending on SQLite:

- persistent slotted pages and heap files
- LRU buffer pool and dirty-page flushing
- primary-key B+ tree access
- transaction rollback, undo-style WAL, and process-local lock manager
- metadata-aware optimizer and schema-aware compiled-plan cache
- column/statistics metadata with min/max, non-null counts, moments, buckets,
  exact value-counts, and Bloom filters
- cost-based inner-join enumeration
- rowid seek joins, hash joins, streaming aggregation, filtered counts, and
  min/max pruning
- typed column vectors with null bitmaps for vectorized scans/aggregates
- ContextQL context-view cache keyed by catalog revision, query, tab, token
  budget, and receipts mode

Still not a production database:

- no MVCC snapshots yet
- no cross-process OS file locking yet
- B+ trees are rebuilt lazily from heap data
- ContextQL extraction is deterministic/rule-based, not learned NLP
- storage is still row-oriented; SIMD/JIT/codegen are future boss fights

## Related feature: TensorQL dataset snapshots

TensorQL is a related snapshot/export layer built on the same idea: important
model inputs should be reproducible data. It freezes a query, stores row IDs,
assigns deterministic splits, remembers feature/label schema, and explains
which raw rows produced a batch.

```skql
manifest-snapshot train_v1 lowkey
slay age, country, clicked, user_id
no-cap events
only-if ts < '2026-01-01'
split-by user_id
with-seed 42
features (
    age FLOAT NORMALIZE ZSCORE,
    country CATEGORICAL ENCODE DICT
)
label clicked INT;
```

```skql
ship-torch train_v1
batch-size 256
shuffle deterministic
epoch 3
rank 0
world-size 8;
```

```python
from tensorql import TensorQLDataset
from torch.utils.data import DataLoader

ds = TensorQLDataset("mydata", dataset="train_v1", batch_size=256, epoch=3)
loader = DataLoader(ds, batch_size=None, num_workers=4)
```

## Benchmarks

### Context strategy benchmark

This compares SkibidiQL context retrieval against non-SkibidiQL baselines:
full-history prompt stuffing, recency windows, and keyword scans.

```bash
cmake --build build --config Release --target skibidi_context_benchmark
./build/benchmarks/skibidi_context_benchmark \
  --messages 1000 --iterations 30 --token-budget 512
```

`--messages` seeds that many prior conversation messages once. `--iterations`
runs that many retrieval calls against the fixed conversation; it is the timing
sample count, not more stored messages.

Sample local result for query `Find restaurants near me`:

| Method | Time | Avg tokens | Tok/input msg | Needed recall | Scanned msgs |
|---|---:|---:|---:|---:|---:|
| full history | 12.752 ms | 12205 | 12.205 | 100% | 1000 |
| recency window | 0.721 ms | 491 | 0.491 | 100% | 41 |
| keyword scan | 4.282 ms | 480 | 0.480 | 50% | 1000 |
| ContextQL varied | 43.319 ms | 185 | 0.185 | 100% | 1000 |
| ContextQL cached | 0.921 ms | 185 | 0.185 | 100% | 0 |

Raw speed alone is not the flex; recency is cheap because it does almost no
context management. The useful metric is quality per token. ContextQL returns
the needed facts in fewer prompt tokens while preserving invalidation,
provenance, ACL receipts, and cacheable relational context semantics.

### Native / SQLite comparison

SQLite comparison is optional:

```bash
cmake -S . -B build-sqlite \
  -DCMAKE_BUILD_TYPE=Release \
  -DSKIBIDI_WITH_SQLITE=ON
cmake --build build-sqlite --config Release
python benchmarks/compare_engines.py \
  --binary build-sqlite/benchmarks/skibidi_engine_comparison \
  --rows 10000 --repeats 3
```

Compact Release-build highlight from a 1,000-row local run:

| Workload | Native / SQLite | Note |
|---|---:|---|
| point lookup | 0.82x | native raw point path beat SQLite here |
| scan | 0.12x | vector/raw scan fast path |
| aggregate | 0.63x | dense/direct aggregate paths |
| join | 1.35x | SQLite still slightly ahead |
| join miss | 0.05x | min/max/Bloom pruning wins |
| context spill | 0.12x | cached relational context view |
| context spill + ACL | 0.07x | redaction path stays hot |

Benchmark notes live in [benchmarks/optimization_notes.md](benchmarks/optimization_notes.md).

## Useful runtime knobs

```bash
SKIBIDI_BUFFER_PAGES=2048 ./build/skibidi --cache-stats
SKIBIDI_VECTOR_BATCH_ROWS=4096 ./build/skibidi --file examples/hello.skql
SKIBIDI_BLOOM_BITS_PER_VALUE=16 ./build/skibidi --cache-stats
SKIBIDI_EXACT_VALUE_COUNT_LIMIT=8192 ./build/skibidi --cache-stats
```

Available knobs: `SKIBIDI_BUFFER_PAGES`, `SKIBIDI_CACHE_ENTRIES`,
`SKIBIDI_STATEMENT_CACHE_ENTRIES`, `SKIBIDI_VECTOR_BATCH_ROWS`,
`SKIBIDI_BLOOM_MIN_BITS`, `SKIBIDI_BLOOM_BITS_PER_VALUE`, and
`SKIBIDI_EXACT_VALUE_COUNT_LIMIT`.

## Repo map

```text
src/                  compiler, optimizer, storage, native engine, ContextQL
test/                 unit and integration tests
examples/             runnable SkibidiQL scripts
benchmarks/           native, context, and SQLite comparison benchmarks
python/tensorql/      tiny PyTorch dataset reader
.agents/              ContextQL agent-memory playbook
```

## The one-line pitch

SkibidiQL is a relational database for agent context management: context gets
schemas, indexes, invalidation, provenance, token budgets, benchmarks, and just
enough brainrot syntax to stay emotionally survivable.
