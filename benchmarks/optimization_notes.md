# Native engine optimization notes

These notes track the SQLite comparison pass that motivated the current native
scan changes.

## SQLite behavior checked

Using SQLite 3.49.1 through the local Python/SQLite build:

- `SELECT COUNT(*) FROM users WHERE active = 1`
  - Plan: `SCAN users`
  - VDBE shape: `Rewind -> Column -> Ne -> AggStep -> Next`
  - No B-tree explains this workload; it is a tight row scan and aggregate.
- `SELECT category, SUM(score) FROM users WHERE active = 1 GROUP BY category`
  - Plan: `SCAN users` plus `USE TEMP B-TREE FOR GROUP BY`
  - The temp B-tree is for grouping, not for filtering the scan.
- The three-table join scans facts and does primary-key rowid seeks into the
  dimension tables, then sorts/groups.
- SQLite aggregation is an incremental `AggStep`/`AggFinal` loop. It does not
  store every row in a group unless the query shape forces additional
  materialization.

## Iterations implemented

1. Raw row views
   - Added `SlottedPage::readView` and heap raw-row scans so native aggregate
     scans can consume in-page tuple bytes without copying each row into a
     `std::vector<uint8_t>`.

2. Direct aggregate scan
   - Added a conservative single-table aggregate path for simple predicates,
     `headcount`, `stack`, `mid`, `goat`, and `L`.
   - This bypasses `StoredRow`, generic `Tuple` batches, predicate vectors, and
     AST evaluation for common aggregate scans.

3. Allocation cleanup
   - Reused decoded-field scratch storage across rows.
   - Scalar `headcount` gets a tight counter loop instead of group/projection
     state.

4. Early-stop projected decoding
   - Raw and public projected tuple decoders now stop once the last referenced
     column is decoded.

5. Buffer-pool diagnosis and default
   - Added buffer capacity/read/eviction counters.
   - Raised the default native buffer pool from 128 to 1024 pages.
   - The benchmark table is about 143 pages; the old 128-page default caused
     repeated scan churn.

6. Bloom-prefiltered hash joins
   - Added a small in-memory Bloom filter next to each hash join build table.
   - Probe keys that are definitely absent skip the `unordered_multimap`
     lookup.
   - This is helpful for miss-heavy joins and intentionally neutral for
     all-matching joins.

7. Cached min/max aggregate pruning
   - Added table-level min/max ranges for columns used by simple aggregate
     predicates.
   - Direct aggregate scans use those ranges to skip impossible predicates such
     as `id > max(id)` or `category < min(category)`.
   - Ranges are invalidated with the existing table statistics on writes.

8. SQLite-style streaming aggregation
   - Added per-group aggregate state for simple grouped queries after joins.
   - This supports `headcount`, `stack`, `mid`, `goat`, `L`, and
     `LONE-WOLF`, and falls back to the older row-vector grouping for
     `HAVING`, `DISTINCT`, `ORDER BY`, `LIMIT`, and more complex expressions.
   - The key change is memory shape: keep counters/sums/extrema/samples per
     group instead of retaining every joined `EvalRow` in each group.

9. Richer column range metadata
   - Column ranges now keep non-null count, raw moments 1 through 5, and a
     16-bucket numeric histogram alongside min/max.
   - Min/max pruning still uses the exact range endpoints. The extra moments
     and buckets give the optimizer/statistics cache enough shape information
     for outlier and cost-model work without changing the storage format.

10. `LONE-WOLF` outlier aggregation
   - Added `LONE-WOLF(col)` syntax and native execution.
   - It counts numeric values with absolute z-score greater than 3.0 using
     population standard deviation.
   - Exact grouped execution keeps per-group numeric samples because the final
     outlier threshold is only known after the group mean and variance are
     known.

11. Rowid-seek join aggregate loop
   - Added a SQLite-shaped fast path for inner joins from a scanned base table
     into joined tables' primary keys.
   - The loop scans the base table as raw rows, seeks each joined primary key
     through the in-memory B+ tree, reads joined rows by `RowId`, decodes only
     referenced joined columns, and updates aggregate state immediately.
   - This bypasses cost-based hash join enumeration, hash table builds, Bloom
     filters, joined `EvalRow` materialization, and the follow-up streaming
     aggregation pass for the matched query shape.
   - The path is conservative: simple aggregate projections/group keys only,
     no `WHERE`, `HAVING`, `DISTINCT`, `ORDER BY`, `LIMIT`, outer joins, or
     non-primary-key join predicates. Non-matching queries still use the CBO
     hash-join path.

12. Cached virtual-memory heap views
   - Added read-only memory-mapped heap views (`MapViewOfFile` on Windows,
     `mmap` on POSIX) for seek-heavy read paths.
   - The native engine caches mapped table views and invalidates them on
     writes, drops, restores, and transaction rollback.
   - The rowid-seek aggregate loop uses mapped base-table scans and mapped
     rowid reads for joined rows, falling back to the buffer pool when mapping
     is unavailable.
   - This removes hot-loop buffer-pool hash lookups/page guards for the
     matching join case. It does not remove all tuple decode or `Value` /
     `RawField` abstraction overhead.

13. Min/max join-domain pruning
   - Added a rowid-seek aggregate preflight for inner equality joins.
   - For each base-table foreign-key column and joined primary-key column, the
     engine compares cached table-level min/max ranges.
   - If any equality domain is disjoint, the inner join is provably empty:
     scalar aggregates return their empty input identity and grouped queries
     return no rows without scanning the base table or doing B+ tree lookups.
   - The miss-heavy benchmark is exactly this shape:
     `facts.d2_id` is outside `dimension_two.id`.

14. Raw point opcode loop
   - Primary-key point selects now run an exact narrow loop:
     B+ tree lookup, raw rowid read, projected-column decode, tuple emit.
   - This skips full-row `EvalRow` materialization and avoids decoding
     unreferenced payload columns.
   - Bloom filters remain useful for negative membership tests, but successful
     point hits still need the exact index/row read.

15. Dense small-domain group aggregation
   - Single-table grouped aggregates with one not-null integer/boolean group
     key and a small min/max domain now use a dense typed group array.
   - This avoids hashing boxed `Tuple` keys for common bucketed OLAP shapes
     such as `category, SUM(score) WHERE active = 1 GROUP BY category`.

16. Exact value-count filtered counts
   - Column range metadata now keeps exact value counts while the distinct
     count stays small.
   - Scalar `COUNT(*) WHERE col = literal` and `COUNT(col) WHERE col = literal`
     can answer from metadata after warm-up instead of scanning rows.
   - This targets the benchmark `COUNT(*) WHERE active = 1` shape and is the
     native analogue of a tiny exact bitmap/materialized-stat shortcut.

17. Memory metric cleanup
   - `engine_memory_bytes` now estimates buffer pages plus B+ tree keys,
     statistics metadata, mapped-view handles, and cached context results.
   - `buffer_memory_bytes` remains the raw resident-buffer-page count, which is
     why older tables showed deceptively tiny native memory on context
     workloads.

## Measured deltas

Release build, 10,000 rows, timed loop after one warm-up:

| Change | Workload | Native elapsed | Buffer reads | Buffer evictions |
|---|---:|---:|---:|---:|
| Old 128-page default | scan x100 | 1,473.9 ms | 14,300 | 14,300 |
| 1024-page default | scan x100 | 163.4 ms | 0 | 0 |
| Old 128-page default | grouped aggregate x50 | 967.8 ms | 7,150 | 7,150 |
| 1024-page default | grouped aggregate x100 | 341.3 ms | 0 | 0 |

Additional targeted runs:

| Mechanism | Workload | Native elapsed | Relevant counters |
|---|---:|---:|---|
| Min/max skip | impossible count x100 | 0.323 ms | 100 scans skipped, 1,000,000 rows skipped, 0 rows scanned |
| Bloom join prefilter | miss-heavy 3-table join x10 | 125.7 ms | 110,000 Bloom checks, 110,000 rejects, 0 hash probes |
| Streaming grouped join | matching 3-table join x10 | 374.3 ms | 10 streaming aggregate queries, 100,000 streamed aggregate rows, 200,000 hash probes |
| Rowid-seek grouped join | matching 3-table join x10 | 270.1 ms | 10 rowid-seek queries, 200,000 rowid lookups, 0 hash probes |
| Rowid-seek miss-heavy join | miss-heavy 3-table join x10 | 36.7 ms | 10 rowid-seek queries, 200,000 rowid lookups, 100,000 misses |
| Mapped rowid-seek grouped join | matching 3-table join x10 | 129.7 ms | 10 mapped scans, 100,000 mapped rowid reads, 0 hash probes |
| Join-domain skip | miss-heavy 3-table join x10 | 0.036 ms | 10 join-domain skips, 100,000 rows skipped, 0 rowid lookups |
| Raw point loop | point hit x1000, 1,000 rows | 10.2 ms on local MSVC smoke | 1,000 raw point hits, 2,000 decoded columns, 3,000 skipped columns |
| Exact value count | filtered count x100, 1,000 rows | 0.7 ms on local MSVC smoke | 100 value-count queries, 0 raw rows scanned after warm-up |
| Dense grouped aggregate | grouped aggregate x100, 1,000 rows | 35.5 ms on local MSVC smoke | 100 dense group aggregate queries, 50,000 matching rows |

The main diagnosis: for repeated scan workloads the previous bottleneck was
buffer-pool thrash, not B-trees. Remaining gaps versus SQLite are now mostly
executor tightness, tuple format, B+ tree lookup overhead, and SQLite's very
compact aggregate/join opcode loops. The raw point and dense aggregate paths
remove more `Value`/`EvalRow` overhead for narrow cases, and exact value counts
let repeated equality counts skip the scan entirely after warm-up. Generic
scans are still row-store loops over serialized tuples rather than
SIMD/columnar kernels.
The rowid-seek path no longer materializes combined join rows for primary-key
dimension joins, but it is still C++ object code over `Value`/`RawField`
abstractions rather than a compact bytecode VM over cache-tuned record cursors.
When table-level domains prove an inner join empty, metadata now wins outright
and avoids the executor loop entirely.
