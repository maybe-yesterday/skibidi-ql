# SkibidiQL

A standalone relational database for the SkibidiQL query language.
It includes its own persistent page storage, heap files, buffer pool, B+ tree
indexes, relational execution engine, and transactions. SQLite is optional and
used only as a compatibility and benchmark backend.

No SQLite library is required for the default build or runtime.

## Performance Highlight

Matched Release-build sample results against prepared, file-backed SQLite on the
same 10,000 rows and warm caches:

| Workload | Iterations | Native | SQLite | Result | Peak RSS (native / SQLite) |
|---|---:|---:|---:|---|---:|
| Primary-key lookup | 10,000 | 89.6 ms | 310.0 ms | Native 3.5x faster | 7.4 / 5.0 MiB |
| Filtered count scan | 100 | 205.6 ms | 24.0 ms | SQLite 8.6x faster | 7.4 / 5.0 MiB |
| Impossible count via min/max | 100 | 0.1 ms | 3.4 ms | Native ~34x faster | 7.4 / 5.0 MiB |
| Filtered grouped sum | 100 | 352.1 ms | 120.1 ms | SQLite 2.9x faster | 7.4 / 5.2 MiB |
| Skewed three-table join | 10 | 97.5 ms | 51.0 ms | SQLite 1.9x faster | 7.4 / 5.2 MiB |
| Miss-heavy three-table join | 10 | <0.1 ms | 4.4 ms | Native metadata skip | 7.0 / 4.8 MiB |

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
    - Direct raw aggregate scans for simple count/sum/min/max/avg workloads
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

- Transactions are single-process and snapshot-backed; there is no WAL,
  crash recovery, or concurrent lock manager yet.
- Primary-key B+ trees are rebuilt lazily and are not separately persisted.
- Inner equi-joins use cost-based left-deep enumeration. Outer joins and
  non-equality joins retain source order.
- Min/max pruning is table-level. Page-level zone maps would skip clustered
  ranges inside larger tables, but are not persisted yet.
- Join-domain pruning is table-level and currently attached to the
  rowid-seek aggregate path for inner primary-key joins.
- Bloom filters help miss-heavy hash joins; all-matching joins still pay the
  normal hash-probe and row-materialization costs.
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
and includes access-path counters such as table scans, index lookups, direct
aggregate scans, decoded/skipped columns, buffer-pool reads/evictions,
min/max skips, join-domain skips, streaming aggregate rows, rowid-seek join
lookups, virtual-memory mapped reads, and Bloom-filter rejects.

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
aggregation, and skewed three-table join workloads. Native comparison runs use
the same 1024-page default buffer pool as the CLI. To reproduce the old cache
thrash diagnosis, pass `--buffer-pages 128` and watch
`buffer_page_reads`/`buffer_evictions` jump. Extra workloads `count_miss` and
`join_miss` isolate min/max pruning and Bloom-filter join pruning. JSON for an
individual run:

```bash
./build-sqlite/benchmarks/skibidi_engine_comparison \
  --engine native --workload join --rows 10000 --iterations 10
```
