# SkibidiQL

A standalone relational database for the SkibidiQL query language.
It includes its own persistent page storage, heap files, buffer pool, B+ tree
indexes, relational execution engine, and transactions. SQLite is optional and
used only as a compatibility and benchmark backend.

No SQLite library is required for the default build or runtime.

## Performance Highlight

Matched Release-build sample results against prepared, file-backed SQLite on the
same 1,000 rows and warm caches:

| Workload | Native | SQLite | Native / SQLite | Native peak RSS | SQLite peak RSS | Native est mem | Native buffer mem | Native raw point | Native value count | Native dense agg | Native ctx cache hits |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| point | 22.0 ms | 26.9 ms | 0.82x | 13.2 MiB | 13.2 MiB | 0.15 MiB | 0.06 MiB | 10000 | 0 | 0 | 0 |
| scan | 0.3 ms | 2.3 ms | 0.12x | 13.2 MiB | 13.2 MiB | 0.15 MiB | 0.06 MiB | 0 | 100 | 0 | 0 |
| count_miss | 0.1 ms | 0.2 ms | 0.47x | 13.2 MiB | 13.2 MiB | 0.23 MiB | 0.06 MiB | 0 | 0 | 0 | 0 |
| aggregate | 7.2 ms | 11.3 ms | 0.63x | 13.2 MiB | 13.2 MiB | 0.16 MiB | 0.06 MiB | 0 | 0 | 100 | 0 |
| join | 2.9 ms | 2.1 ms | 1.35x | 13.2 MiB | 13.2 MiB | 0.21 MiB | 0.05 MiB | 0 | 0 | 0 | 0 |
| join_miss | 0.0 ms | 0.4 ms | 0.05x | 13.2 MiB | 13.2 MiB | 0.21 MiB | 0.05 MiB | 0 | 0 | 0 | 0 |
| context_schema | 2.5 ms | 3.5 ms | 0.71x | 13.2 MiB | 13.2 MiB | 0.01 MiB | 0.00 MiB | 0 | 0 | 0 | 1000 |
| context_spill | 0.4 ms | 3.4 ms | 0.12x | 13.2 MiB | 13.2 MiB | 0.01 MiB | 0.00 MiB | 0 | 0 | 0 | 100 |
| context_spill_acl | 0.3 ms | 4.1 ms | 0.07x | 13.2 MiB | 13.2 MiB | 0.01 MiB | 0.00 MiB | 0 | 0 | 0 | 100 |
| context_objects | 4.9 ms | 7.3 ms | 0.68x | 13.2 MiB | 13.2 MiB | 1.86 MiB | 0.00 MiB | 0 | 0 | 0 | 10 |

See [Benchmarks](#benchmarks) and
[benchmarks/optimization_notes.md](benchmarks/optimization_notes.md) for
reproduction details.

## Language Reference

### Keyword Mappings

| SkibidiQL | SQL Equivalent | Description |
|-----------|----------------|-------------|
| `slay` | `SELECT` | Select columns |
| `no-cap` | `FROM` | Table source |
| `only-if` | `WHERE` | Row filter |
| `link-up` | `JOIN` / `INNER JOIN` | Join tables |
| `left-link-up` | `LEFT JOIN` | Left outer join |
| `mid-link-up` | `INNER JOIN` | Inner join (explicit) |
| `fr-fr` | `ON` | Join condition |
| `vibe-check` | `GROUP BY` | Group rows |
| `hits-different` | `ORDER BY` | Sort results |
| `bussin-only` | `HAVING` | Group filter |
| `yeet-into` | `INSERT INTO` | Insert rows |
| `drip` | `VALUES` | Value list |
| `glow-up` | `UPDATE` | Update rows |
| `be-like` | `SET` | Assign values |
| `ratio` | `DELETE FROM` | Delete rows |
| `manifest` | `CREATE TABLE` | Create table |
| `rizz-down` | `DROP TABLE` | Drop table |
| `manifest-snapshot` | `CREATE SNAPSHOT` | Freeze a reproducible training dataset |
| `manifest-dataset` | `CREATE DATASET` | Alias for `manifest-snapshot` |
| `with-seed` | `WITH SEED` | Snapshot/shuffle seed |
| `ship-torch` | `EXPORT TORCH` | Build a deterministic PyTorch batch plan |
| `export-torch` | `EXPORT TORCH` | Plain alias for `ship-torch` |
| `spill-batch` | `EXPLAIN BATCH` | Explain batch provenance / resume token |
| `explain-batch` | `EXPLAIN BATCH` | Plain alias for `spill-batch` |
| `batch-size` | `BATCH_SIZE` | Batched training export size |
| `world-size` | `WORLD_SIZE` | Distributed training replica count |
| `manifest-context` | `CREATE CONTEXT` | Create a prompt-context log |
| `yeet-memory` | `APPEND MEMORY` | Append a message and extract semantic atoms |
| `spill-context` | `SPILL CONTEXT` | Render the maintained current context view |
| `show-tabs` | `SHOW TABS` | List prompt-context topic tabs |
| `show-context-schemas` | `SHOW CONTEXT SCHEMAS` | Inspect the built-in context schema registry |
| `show-context-objects` | `SHOW CONTEXT OBJECTS` | Inspect schema-aware messages and atoms |
| `alias-tab` | `ALIAS TAB` | Normalize one tab label to another |
| `merge-tabs` | `MERGE TABS` | Move messages/atoms from one tab to another |
| `token-budget` | `TOKEN_BUDGET` | Context-view rendering budget |
| `vibe-tab` | `TAG MEMORY` | Topic tab / namespace for prompt context |
| `lowkey` | `AS` | Alias |
| `plus` | `AND` | Logical AND |
| `or-nah` | `OR` | Logical OR |
| `no-cap-not` | `NOT` | Logical NOT |
| `ghosted` | `NULL` | Null value |
| `unique-fr` | `DISTINCT` | Deduplicate |
| `cap-at` | `LIMIT` | Row limit |
| `skip` | `OFFSET` | Row offset |
| `up-only` | `ASC` | Ascending sort |
| `down-bad` | `DESC` | Descending sort |
| `main-character` | `PRIMARY KEY` | Primary key constraint |
| `side-character` | `FOREIGN KEY` | Foreign key constraint |

### Aggregate Functions

| SkibidiQL | SQL Equivalent | Description |
|-----------|----------------|-------------|
| `headcount(*)` | `COUNT(*)` | Count rows |
| `headcount(unique-fr col)` | `COUNT(DISTINCT col)` | Count distinct |
| `stack(col)` | `SUM(col)` | Sum values |
| `mid(col)` | `AVG(col)` | Average |
| `goat(col)` | `MAX(col)` | Maximum |
| `L(col)` | `MIN(col)` | Minimum |
| `LONE-WOLF(col)` | `LONE_WOLF(col)` | Native z-score outlier count |

### Advanced Analytics

| SkibidiQL | SQL Equivalent | Description |
|-----------|----------------|-------------|
| `biggest-W(col)` | `ORDER BY col DESC LIMIT 1` | Row with highest value (ARGMAX) |
| `biggest-L(col)` | `ORDER BY col ASC LIMIT 1` | Row with lowest value (ARGMIN) |
| `mid-fr(col)` | Median CTE | Statistical median |
| `percent-check(col, n)` | Percentile CTE | Nth percentile |
| `era [split-by ...] hits-different ...` | `RANK() OVER (...)` | Window rank function |

## Build Instructions

### Prerequisites

- CMake >= 3.14
- g++ with C++17 support

SQLite and pkg-config are needed only when building the optional compatibility
backend.

On Ubuntu/Debian:
```bash
sudo apt-get install cmake g++
```

On Fedora:
```bash
sudo dnf install cmake gcc-c++
```

On macOS (Homebrew):
```bash
brew install cmake
```

### Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
```

The compiled binary will be at `build/skibidi` (or
`build/Release/skibidi.exe` with a multi-config generator).

To additionally build SQLite compatibility tests and comparison benchmarks:

```bash
cmake -S . -B build-sqlite \
  -DCMAKE_BUILD_TYPE=Release \
  -DSKIBIDI_WITH_SQLITE=ON
cmake --build build-sqlite --config Release
```

## Usage

### Interactive REPL

```bash
./build/skibidi
```

```
SkibidiQL REPL v2.0.0 (native engine)
End queries with ';' or type 'exit'.

skibidi> slay name, age no-cap users only-if age > 18;
skibidi> exit
```

### Execute a File

```bash
./build/skibidi --file examples/schema.skql
```

### Use a Persistent Native Database

```bash
./build/skibidi --db mydata --file examples/schema.skql
```

`mydata/` contains the native catalog and table heap files.

### Transactions

The native REPL supports explicit transactions:

```text
skibidi> .begin
skibidi> yeet-into users drip (1, 'Ada');
skibidi> .commit
```

Use `.rollback` to restore the database to its state at `.begin`.

### Training Snapshots

SkibidiQL can now act like a tiny database-backed training dataset engine. The
main-character move is `manifest-snapshot`: it runs a source query once, freezes
the included native row IDs, assigns deterministic train/validation/test splits,
stores the seed, feature schema, label schema, schema fingerprint, table
version, and leakage warnings in the native catalog.

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

The SQL-ish spelling also works (for older folks):

```sql
CREATE SNAPSHOT train_v1 AS
SELECT age, country, clicked, user_id
FROM events
WHERE ts < '2026-01-01'
SPLIT BY user_id
WITH SEED 42
FEATURES (age FLOAT NORMALIZE ZSCORE, country CATEGORICAL ENCODE DICT)
LABEL clicked INT;
```

For training, ask for a deterministic PyTorch plan:

```skql
ship-torch train_v1
batch-size 256
shuffle deterministic
epoch 3
rank 0
world-size 8;
```

Batch provenance is first-class, so a loss spike can be traced back to raw rows:

```skql
spill-batch train_v1 batch-size 256 epoch 3 batch 1042 rank 0 world-size 8;
```

The result includes sample count, source `page:slot` row IDs, feature columns,
label distribution, split, seed, rank/world-size, worker, and a resume token.
The deterministic sampler is:

```text
epoch + global sample + rank/world-size -> stable row id order
```

Use `split-by user_id` for non-sus user-level splits. `split-by random by row`
is accepted, but if a `user_id` column exists and the same user lands in
multiple splits, the snapshot reports a leakage warning.

The tiny Python reader lives under `python/tensorql`:

```bash
python -m pip install -e python/tensorql
```

```python
from tensorql import TensorQLDataset
from torch.utils.data import DataLoader

ds = TensorQLDataset("mydata", dataset="train_v1", batch_size=256, epoch=3)
loader = DataLoader(ds, batch_size=None, num_workers=4)
```

`TensorQLDataset` yields already-batched records with `features`, `label`,
`rowid`, `snapshot`, `split`, `epoch`, and `rank`. Numeric columns become
PyTorch tensors when PyTorch is installed; text/blob columns stay as Python
lists for now.

### Prompt Views

SkibidiQL also has a small prototype for the idea:

> Prompt context should be a maintained database view over an append-only
> conversation log, not raw chat history.

The core loop is:

```text
raw messages
  -> semantic atoms
  -> invalidation / provenance
  -> token-budgeted current context view
```

Create a context log:

```skql
manifest-context convo_123;
```

Append messages. The current prototype uses deterministic rule extraction for
facts, preferences, constraints, tasks, open questions, decisions, debug
follow-ups, and a few demo-friendly dog facts, which keeps the demo
inspectable:

```skql
yeet-memory convo_123 drip
    (1, 'user', 'I live in Seattle.');

yeet-memory convo_123 drip
    (88, 'user', 'Actually I moved to NYC.');
```

The second message extracts `user_location=NYC` and invalidates the older
`user_location=Seattle` atom. Render the current prompt view:

```skql
spill-context convo_123
only-if 'Find restaurants near me'
token-budget 200
receipts on;
```

Example fields:

```text
field=current_context | value=fact user_location=NYC @message_88
field=invalidated | value=user_location=Seattle @message_1 invalidated_by=message_88
field=token_budget | value=200
field=token_cost | value=8
```

This is intentionally not “RAG but with chat logs.” The database bit is view
maintenance: active/invalidated atoms, provenance, contradiction handling, and
budget-aware rendering. The prototype persists contexts in the native catalog
alongside tables and snapshots.

Topic tabs are first-class too. An agent can put memories in an explicit
`vibe-tab`, ask SkibidiQL to pick one with `vibe-tab auto`, query tab inventory
with `show-tabs`, normalize labels with `alias-tab`, and merge messy labels
with `merge-tabs`:

```skql
alias-tab convo_123 'dog' to 'convo about dog';

yeet-memory convo_123 drip
    (7, 'user', 'My dog likes salmon.')
vibe-tab auto;

yeet-memory convo_123 drip
    (8, 'user', 'I like cat cafes.')
vibe-tab 'pet stuff';

show-tabs convo_123;

merge-tabs convo_123 'pet stuff' into 'dog';

spill-context convo_123
vibe-tab 'dog'
only-if 'what does my dog like?'
token-budget 200
receipts on;
```

Under the hood, messages and extracted atoms both store the tab. `spill-context`
filters atoms by resolved tab before ranking/rendering. `vibe-tab` retagging
and `merge-tabs` recompute per-tab invalidation so `main` does not stay
haunted by facts moved elsewhere. `show-tabs` returns tab names, message
counts, atom counts, active/invalidated counts, last message IDs, and aliases.
Tiny but real agent-memory affordance: the LLM can choose or ask for a label
like `convo about dog`, save it, inspect the tab map, then ask for only that
slice later.

SkibidiQL now also exposes a tiny Context Schema Registry, because agent memory
needs receipts about shape, policy, and storage routing:

```skql
show-context-schemas;
show-context-objects convo_123;
```

This returns built-in context object types like `ConversationMessage`,
`ContextAtom`, `TaskState`, `ToolInvocationLog`, and `UserProfile`, including
version, owner agent, sensitivity, retention policy, storage backend,
vectorized fields, access labels, indexed fields, and related schemas.
Messages and atoms now persist their own schema name/version, access labels,
storage route, and lightweight mentioned-entity metadata. `show-context-objects`
lists those objects as rows, while `spill-context` includes Dynamic Context
Fabric receipts such as the schema registry pair used for retrieval, the
structured/vector/blob storage route, access-policy labels, indexed fields,
and the number of redacted atoms. Sensitive rows like password/API-key notes
get tagged `CONFIDENTIAL_CUSTOMER_DATA` and render as
`[redacted:CONFIDENTIAL_CUSTOMER_DATA]`. Very official. Context has a bouncer
now.

Plain aliases also work:

```sql
CREATE CONTEXT convo_123;
APPEND MEMORY convo_123 drip (1, 'user', 'I live in Seattle.');
SPILL CONTEXT convo_123 query 'Find restaurants near me' token_budget 200;
```

### Transpile Only (No Execution)

```bash
./build/skibidi --transpile-only --file examples/hello.skql
```

### Verbose Mode

```bash
./build/skibidi --verbose --file examples/hello.skql
```

Verbose mode prints the token stream, AST, optimizer report, and generated SQL for each statement.

### Compilation Cache

The REPL keeps a bounded LRU cache of compiled SQL. Cache keys include a stable
catalog fingerprint, so schema changes cannot reuse stale optimized SQL.

```bash
./build/skibidi --cache-entries 256 --cache-stats
./build/skibidi --no-cache
```

The default limit is 128 entries and 4 MiB of estimated cache storage.

When the optional SQLite backend is built, SQLite prepared statements are also
cached:

```bash
./build/skibidi --statement-cache-entries 256 --cache-stats
./build/skibidi --no-statement-cache
```

Use a Release build for performance measurements.

### Read from stdin

```bash
echo "slay * no-cap users;" | ./build/skibidi --transpile-only
```

## Example Queries

### Basic SELECT

```skql
-- SkibidiQL
slay id, name, email
no-cap users
only-if age > 18
hits-different name up-only;
```

```sql
-- Generated SQL
SELECT id, name, email FROM users WHERE (age > 18) ORDER BY name ASC
```

### JOIN

```skql
slay u.name, o.total
no-cap users lowkey u
link-up orders lowkey o fr-fr u.id = o.user_id
only-if o.total > 100
hits-different o.total down-bad;
```

```sql
SELECT u.name, o.total FROM users AS u JOIN orders AS o ON (u.id = o.user_id) WHERE (o.total > 100) ORDER BY o.total DESC
```

### Aggregates

```skql
slay stack(amount) lowkey total, mid(amount) lowkey average
no-cap transactions;
```

```sql
SELECT SUM(amount) AS total, AVG(amount) AS average FROM transactions
```

`LONE-WOLF(col)` is a native aggregate that counts numeric values whose
absolute z-score is greater than 3.0 using the population standard deviation.
Empty, single-value, and zero-variance groups return `0`.

### ARGMAX (biggest-W)

```skql
slay biggest-W(salary)
no-cap employees
only-if department = 'Engineering';
```

```sql
SELECT * FROM employees WHERE (department = 'Engineering') ORDER BY salary DESC LIMIT 1
```

### Median (mid-fr)

```skql
slay mid-fr(salary) lowkey median_salary
no-cap employees;
```

```sql
WITH __data AS (SELECT salary FROM employees),
     __ordered AS (SELECT salary, ROW_NUMBER() OVER (ORDER BY salary) AS rn, COUNT(*) OVER () AS cnt FROM __data)
SELECT AVG(salary) AS median_salary FROM __ordered WHERE rn IN ((cnt + 1) / 2, (cnt + 2) / 2)
```

### Window Function (era)

```skql
slay name, salary, era split-by department hits-different salary down-bad lowkey rank
no-cap employees;
```

```sql
SELECT name, salary, RANK() OVER (PARTITION BY department ORDER BY salary DESC) AS rank FROM employees
```

### CREATE TABLE

```skql
manifest users (
    id INTEGER main-character,
    name TEXT no-cap-not ghosted,
    email TEXT no-cap-not ghosted,
    age INTEGER
);
```

```sql
CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, email TEXT NOT NULL, age INTEGER)
```

### INSERT

```skql
yeet-into users (id, name, age)
drip
    (1, 'Alice', 30),
    (2, 'Bob', 25);
```

```sql
INSERT INTO users (id, name, age) VALUES (1, 'Alice', 30), (2, 'Bob', 25)
```

### UPDATE

```skql
glow-up users
be-like age = 31
only-if name = 'Alice';
```

```sql
UPDATE users SET age = 31 WHERE (name = 'Alice')
```

### DELETE

```skql
ratio users only-if age < 18;
```

```sql
DELETE FROM users WHERE (age < 18)
```

## Architecture Overview

The SkibidiQL compiler follows a classical pipeline:

```
Source Text
    |
    v
[Lexer] (lexer.h / lexer.cpp)
    - Tokenizes SkibidiQL source
    - Handles hyphenated keywords with longest-match
    - Produces a flat vector of Token structs
    |
    v
[Parser] (parser.h / parser.cpp)
    - Recursive descent parser
    - Produces an AST using smart pointers
    - Supports all SkibidiQL statement types
    |
    v
[Metadata Catalog] (metadata.h / metadata.cpp)
    - Tracks schema in .skibidi_catalog.json
    - Tracks SkibidiQL training snapshots: query text, seed, schema fingerprint,
      row IDs, split assignment, feature specs, label spec, and warnings
    - Tracks SkibidiQL prompt logs: messages, semantic atoms, active/invalidated
      status, invalidation provenance, and rendered prompt-view metadata
    - Updated after successful CREATE TABLE / DROP TABLE execution
    - Validates column references (best-effort)
    - Exposes revision and schema fingerprint metadata
    |
    v
[Optimizer] (optimizer.h / optimizer.cpp)
    - Pass 1: Constant folding (2 + 3 -> 5)
    - Pass 2: Metadata-aware rewrites
      - COUNT(non-null column) -> COUNT(*)
      - Remove DISTINCT when a guaranteed non-null primary key is projected
      - Add LIMIT 1 and remove ORDER BY for primary-key point lookups
    - Pass 3: Predicate pushdown analysis
    - Pass 4: Projection pruning analysis
    - Pass 5: Dead code elimination (WHERE 1=0)
    |
    v
[Code Generator] (codegen.h / codegen.cpp)
    - Visitor-style traversal of AST
    - Produces standard SQL strings
    - Handles special rewrites (mid-fr, percent-check, biggest-W/L)
    |
    v
[Compilation Cache] (cache.h / compiler.h)
    - Stores generated SQL and optimizer reports
    - Bounded by entry count and estimated memory
    - Uses LRU eviction and catalog-aware keys
    |
    v
[Native Physical Engine] (native_engine.h / native_engine.cpp)
    - Indexed and heap access paths
    - Training snapshot materialization with deterministic splits
    - Deterministic distributed batch planning for PyTorch-style loaders
    - Batch provenance explain output for row-level debugging/resume tokens
    - Prompt append/extract/invalidate/render loop for SkibidiQL prompt views
    - Raw primary-key point opcode loop that decodes only projected columns
    - Direct raw aggregate scans for simple count/sum/min/max/avg workloads
    - Exact value-count stats for repeated filtered `COUNT(*)` equality scans
    - Dense typed small-domain `GROUP BY` loops for integer buckets
    - Rowid-seek join aggregate loop for fact-to-primary-key joins
    - Cached read-only virtual-memory heap views for seek-heavy reads
    - Min/max join-domain pruning for provably empty inner joins
    - SQLite-style streaming group aggregation after joins
    - Cached table-level min/max pruning for impossible aggregate filters
    - Column statistics with non-null counts, raw moments 1..5, and 16 buckets
    - Bloom-prefiltered hash joins for miss-heavy equi-joins
    - Typed column vectors with bit-packed null masks
    - Vectorized filters, projections, groups, and common aggregates
    - Statistics cache and dynamic-programming inner-join enumeration
    - Smaller-side hash builds, nested-loop fallback, sorts, windows, limits
    - Constraint checking and transaction rollback
    |
    v
[Storage Engine] (native_storage.h / native_storage.cpp)
    - Typed tuple serialization
    - Raw row views for copy-free scan consumers
    - Projection-aware decoding that skips unreferenced payloads
    - Early-stop projected decoding once the last referenced column is read
    - 4 KiB slotted pages
    - Persistent heap files
    - 1024-page default LRU buffer pool with dirty-page flushing
    |
    v
[B+ Tree] (native_index.h / native_index.cpp)
    - Primary-key point and range access
    - Rebuilt lazily from authoritative heap data
    |
    v
[Optional SQLite Backend] (executor.h / executor.cpp)
    - Compatibility execution and comparative benchmarks
```

### Caveats

This is NOT yet a production database:

- Durability/concurrency is now a first-cut native layer: undo-style WAL,
  startup crash recovery, a process-local lock manager, concurrent readers, and
  one serialized writer. It is not MVCC and does not yet use OS-level
  cross-process file locks.
- Primary-key B+ trees are rebuilt lazily and are not separately persisted.
- Inner equi-joins use cost-based left-deep enumeration. Outer joins and
  non-equality joins retain source order.
- Min/max pruning is table-level. Page-level zone maps would skip clustered
  ranges inside larger tables, but are not persisted yet.
- Join-domain pruning is table-level and currently attached to the
  rowid-seek aggregate path for inner primary-key joins.
- Bloom filters help miss-heavy hash joins; all-matching joins still pay the
  normal hash-probe and row-materialization costs.
- Bloom filters are not a cheat code for successful primary-key point hits:
  positive lookups still need the exact B+ tree seek and row read. The native
  point fast path is therefore an exact raw rowid/projection loop, while
  probabilistic filters remain best for misses and joins.
- `LONE-WOLF` is exact in the native engine. SQL generation emits
  `LONE_WOLF(col)`, so the optional SQLite backend would need a matching UDF to
  execute that aggregate directly.
- Virtual-memory heap views bypass buffer-pool copies/guards for selected
  read-heavy paths, but they do not erase all `Value`/`RawField` abstraction
  overhead. Mappings are read-only and invalidated on writes/rollback.
- Heap pages still store row-oriented tuples. Direct aggregate scans and the
  vector decoder materialize only referenced fields, but the engine does not
  yet use a columnar storage format, SIMD intrinsics, or generated machine
  code.
- `manifest-snapshot` currently snapshots one native table with an optional
  filter. Arbitrary join/materialized-transform snapshots are the next layer.
  The Python `TensorQLDataset` reader returns batched tensors for numeric
  fields and keeps text/blob fields as lists.
- Prompt-view extraction is currently deterministic rule-based NLP. It covers
  demo patterns for `user_location`, preferences, constraints, tasks, open
  questions, decisions, debug follow-ups, and dog facts. The database semantics
  are real; broad semantic extraction and learned relevance scoring are future
  work.

Those boundaries are intentionally isolated behind the storage and execution
interfaces, making persisted indexes/statistics, broader vectorization,
cross-process locks, MVCC, and JIT compilation natural next layers.

### Implemented Next-Step Features

These are now SkibidiQL features, not separate side projects:

- `show-tabs convo_123;` lists topic tabs with message counts, atom counts,
  active/invalidated counts, last message IDs, and aliases.
- `show-context-schemas;` lists the built-in CSR rows for agent context types:
  schema version, owner, sensitivity, retention, storage route, vectorized
  fields, access labels, indexes, and related schemas.
- `show-context-objects convo_123;` exposes actual DCF rows for messages and
  atoms: schema/version, resolved tab, status, access labels, storage route,
  source message, and redacted value.
- `vibe-tab auto` asks SkibidiQL to propose a tab from the message text before
  saving it. Current built-in suggestions include labels like
  `convo about dog`, `project roadmap`, `debugging sqlite perf`,
  `open questions`, `constraints`, `preferences`, and `current tasks`.
- `alias-tab convo_123 'dog' to 'convo about dog';` makes casual labels resolve
  to the canonical tab.
- `merge-tabs convo_123 'pet stuff' into 'dog';` moves messages and atoms,
  adds an alias from the old tab, and recomputes per-tab invalidation.
- Prompt extraction now covers the deterministic baseline plus open questions,
  decisions, debug follow-ups, simple constraints, and a few dog-demo facts.
- The native benchmark harness includes a `prompt` workload for tab-filtered
  prompt-view rendering over an aliased/merged prompt log.
- The native engine now has WAL + crash recovery + process-local locking, so
  multithreaded readers can vibe while writes are serialized behind the
  database gate.
- `spill-context` now emits DCF receipts showing schema registry usage,
  structured/vector/blob routing, access-policy labels, indexed fields,
  mentioned entities, and redacted-atom counts.

Still-future layers are intentionally heavier: arbitrary join snapshots,
memory-mapped training chunks, pinned PyTorch batches, persisted page-level zone
maps, cross-process lock files, MVCC snapshots, broader learned relevance
scoring, and JIT/SIMD hot paths. Those need bigger design passes and benchmark
receipts.

## Benchmarks

The native benchmark requires no SQLite:

```bash
cmake --build build --config Release --target skibidi_native_benchmark
./build/skibidi_native_benchmark --workload point
./build/skibidi_native_benchmark --workload scan --iterations 100
./build/skibidi_native_benchmark --workload aggregate --iterations 100
./build/skibidi_native_benchmark --workload prompt --rows 1000 --iterations 100
./build/skibidi_native_benchmark --workload context_spill_acl --rows 1000 --iterations 100
./build/skibidi_native_benchmark --workload context_objects --rows 1000 --iterations 10
```

Available native workloads are `point`, `scan`, `aggregate`, `prompt`,
`context_schema`, `context_spill`, `context_spill_acl`, and
`context_objects`. The `prompt`/`context_spill` workloads seed a SkibidiQL
prompt log with `vibe-tab auto`, `alias-tab`, `merge-tabs`, then repeatedly
run a tab-filtered `spill-context`. `context_spill_acl` adds confidential
password/API-key-shaped notes and verifies the redaction path stays hot.
`context_objects` benchmarks the DCF object inspector over schema-aware
message/atom rows. Context reads use a revision-aware result cache keyed by
context, tab, query, token budget, receipts mode, and catalog revision, so
repeated prompt views after a warm-up should report `context_cache_hits` in
the JSON output.
Output is JSON and includes access-path counters such as table scans, index
lookups, raw point hits, direct aggregate scans, exact value-count answers,
dense group aggregate loops, decoded/skipped columns, estimated native memory,
buffer-pool reads/evictions, min/max skips, join-domain skips, streaming
aggregate rows, rowid-seek join lookups, virtual-memory mapped reads,
Bloom-filter rejects, context cache hits/misses, atoms scored, atoms rendered,
and atoms redacted.

The optional SQLite build also provides comparative modes:

```bash
cmake --build build-sqlite --config Release --target skibidi_benchmark
python benchmarks/run_benchmarks.py \
  --binary build-sqlite/benchmarks/skibidi_benchmark \
  --workload point
```

That suite compares normal SQL, prepared SQL, uncached SkibidiQL, cached
SkibidiQL, and cached SkibidiQL with prepared-statement reuse.

For a matched native-engine comparison, build and run:

```bash
cmake -S . -B build-sqlite-wsl \
  -DCMAKE_BUILD_TYPE=Release \
  -DSKIBIDI_WITH_SQLITE=ON
cmake --build build-sqlite-wsl \
  --target skibidi_engine_comparison -j
python benchmarks/compare_engines.py \
  --binary build-sqlite-wsl/benchmarks/skibidi_engine_comparison \
  --rows 10000 --repeats 3
```

On WSL/Ubuntu, install the native build dependencies first if CMake cannot
find SQLite:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libsqlite3-dev
```

Avoid reusing a Windows-generated `build-sqlite` directory from inside WSL.
Use a separate Linux build directory such as `build-sqlite-wsl`; otherwise
`gmake` may complain that there is no `Makefile`.

The comparison uses separate processes, persistent database files, prepared
SQLite statements, one warm-up execution, and identical point, scan,
aggregation, skewed three-table join, miss-heavy join, ContextQL schema,
ContextQL spill, ACL-redaction spill, and context-object inspection workloads.
For ContextQL, SQLite gets equivalent normalized `context_messages`,
`context_atoms`, and `context_schemas` tables, so the comparison is normal SQL
over rows versus native SkibidiQL fabric APIs. Native comparison runs use the
same 1024-page default buffer pool as the CLI. To reproduce the old cache
thrash diagnosis, pass `--buffer-pages 128` and watch
`buffer_page_reads`/`buffer_evictions` jump. Extra workloads `count_miss` and
`join_miss` isolate min/max pruning and Bloom-filter join pruning. The summary
table reports both `Native est mem` (buffer pages plus estimated B+ tree,
statistics, and context-result-cache memory) and `Native buffer mem` (resident
buffer-pool pages only), plus raw-point/value-count/dense-aggregate counters
so fast-path coverage is visible. JSON for an individual run:

```bash
./build-sqlite/benchmarks/skibidi_engine_comparison \
  --engine native --workload join --rows 10000 --iterations 10
```
