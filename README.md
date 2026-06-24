# SkibidiQL

A standalone educational relational database for the SkibidiQL query language.
It includes its own persistent page storage, heap files, buffer pool, B+ tree
indexes, relational execution engine, and transactions. SQLite is optional and
used only as a compatibility and benchmark backend.

## Native Engine Highlight

The native backend follows the shape of a CSE 444/SimpleDB-style course engine:

- 4 KiB slotted pages and variable-width typed tuples
- Persistent heap files with an LRU buffer pool
- Rebuildable B+ tree primary-key indexes
- Indexed lookups, heap scans, hash joins, nested-loop joins, grouping,
  aggregates, sorting, limits, left joins, and window ranking
- 1,024-row column batches for filters, projections, and common aggregates
- Projection-aware tuple decoding that skips unreferenced fields without
  allocating their text/blob payloads
- Fixed-type integer, real, text, boolean, and blob vectors with bit-packed
  null masks
- Statistics-backed dynamic-programming join enumeration for inner equi-joins
- Primary-key, non-null, and foreign-key enforcement
- Atomic statement rollback and explicit `.begin`, `.commit`, `.rollback`
- A schema-aware compiled-plan cache

No SQLite library is required for the default build or runtime.

## Performance Highlight

Matched Release-build results against prepared, file-backed SQLite on the same
10,000 rows and warm caches:

| Workload | Iterations | Native | SQLite | Result | Peak RSS (native / SQLite) |
|---|---:|---:|---:|---|---:|
| Primary-key lookup | 10,000 | 77.5 ms | 892.2 ms | Native 11.5x faster | 8.1 / 5.2 MiB |
| Filtered count scan | 100 | 1,535.7 ms | 66.9 ms | SQLite 23.0x faster | 7.6 / 5.0 MiB |
| Filtered grouped sum | 100 | 2,348.8 ms | 340.8 ms | SQLite 6.9x faster | 7.7 / 5.3 MiB |
| Skewed three-table join | 10 | 626.1 ms | 74.8 ms | SQLite 8.4x faster | 16.9 / 5.5 MiB |

The vectorized scan is about 3.9x faster than the original 6.05-second
row-at-a-time result. In a direct before/after run, projected decoding and typed
vectors reduced filtered-scan time by 29% and grouped-aggregate time by 15%.
Cost-based join ordering plus shared immutable row schemas cut the join from
1.11 seconds to 0.63 seconds and peak RSS from about 36 MiB to 17 MiB. These
remain educational-engine measurements, not production database claims. See
[Benchmarks](#benchmarks) for reproduction.

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
    - Typed column vectors with bit-packed null masks
    - Vectorized filters, projections, groups, and common aggregates
    - Statistics cache and dynamic-programming inner-join enumeration
    - Smaller-side hash builds, nested-loop fallback, sorts, windows, limits
    - Constraint checking and transaction rollback
    |
    v
[Storage Engine] (native_storage.h / native_storage.cpp)
    - Typed tuple serialization
    - Projection-aware decoding that skips unreferenced payloads
    - 4 KiB slotted pages
    - Persistent heap files
    - LRU buffer pool with dirty-page flushing
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

### Key Design Decisions

1. **Hyphenated keywords**: The lexer reads `[a-zA-Z_][a-zA-Z0-9_-]*` greedily. Since every hyphenated keyword is globally unique (no two keywords share the same full string), a simple hash map lookup suffices for keyword identification.

2. **Longest-match**: Because hyphens are consumed as part of the token word, `no-cap-not` is always read as one token and looked up as a single key, beating `no-cap` automatically.

3. **Analytics rewrites**: `mid-fr`, `percent-check`, `biggest-W`, and `biggest-L` are detected at the SELECT statement codegen level and rewritten to CTE-based or ORDER BY-based SQL patterns.

4. **Catalog persistence**: Each native database directory contains its own
   `catalog.json`, alongside the table heap files.

5. **Cache correctness**: DDL statements are never cached. Cache keys include
   the complete schema fingerprint, so equivalent schemas can reuse SQL while
   different schemas cannot collide.

6. **Authoritative heap storage**: Heap pages are the source of truth. B+ tree
   indexes are rebuildable access paths, so an index can be discarded without
   losing table data.

7. **Educational transactions**: Statements are atomic. Explicit transactions
   use a database snapshot for rollback, providing single-process serializable
   behavior without implementing production WAL recovery.

### Current Engine Boundaries

This is a complete course-scale engine, not yet a production database:

- Transactions are single-process and snapshot-backed; there is no WAL,
  crash recovery, or concurrent lock manager yet.
- Primary-key B+ trees are rebuilt lazily and are not separately persisted.
- Inner equi-joins use cost-based left-deep enumeration. Outer joins and
  non-equality joins retain source order.
- Heap pages still store row-oriented tuples. The decoder materializes only
  referenced fields into typed column batches, but the engine does not yet use
  a columnar storage format, SIMD intrinsics, or generated machine code.

Those boundaries are intentionally isolated behind the storage and execution
interfaces, making WAL, locking, persisted indexes/statistics, broader
vectorization, and JIT compilation natural next layers.

## Benchmarks

The native benchmark requires no SQLite:

```bash
cmake --build build --config Release --target skibidi_native_benchmark
./build/skibidi_native_benchmark --workload point
./build/skibidi_native_benchmark --workload scan --iterations 100
./build/skibidi_native_benchmark --workload aggregate --iterations 100
```

Available native workloads are `point`, `scan`, and `aggregate`. Output is JSON
and includes access-path counters such as table scans and index lookups.

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
cmake --build build-sqlite --config Release \
  --target skibidi_engine_comparison
python benchmarks/compare_engines.py \
  --binary build-sqlite/benchmarks/skibidi_engine_comparison \
  --rows 10000 --repeats 3
```

The comparison uses separate processes, persistent database files, prepared
SQLite statements, one warm-up execution, and identical point, scan,
aggregation, and skewed three-table join workloads. JSON for an individual run:

```bash
./build-sqlite/benchmarks/skibidi_engine_comparison \
  --engine native --workload join --rows 10000 --iterations 10
```
