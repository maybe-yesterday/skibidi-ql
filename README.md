# SkibidiQL

A serious compiler for the SkibidiQL query language - a Gen-Z flavored SQL dialect that transpiles to standard SQL and executes on SQLite.

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
- SQLite3 development libraries
- pkg-config

On Ubuntu/Debian:
```bash
sudo apt-get install cmake g++ libsqlite3-dev pkg-config
```

On Fedora:
```bash
sudo dnf install cmake gcc-c++ sqlite-devel pkgconf
```

On macOS (Homebrew):
```bash
brew install cmake sqlite3 pkg-config
```

### Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

The compiled binary will be at `build/skibidi`.

## Usage

### Interactive REPL

```bash
./build/skibidi
```

```
SkibidiQL REPL v1.0.0
Type your SkibidiQL queries (end with ';') or 'exit' to quit.

skibidi> slay name, age no-cap users only-if age > 18;
skibidi> exit
```

### Execute a File

```bash
./build/skibidi --file examples/schema.skql
```

### Use a Persistent Database

```bash
./build/skibidi --db mydata.db --file examples/schema.skql
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
    - Updated on CREATE TABLE / DROP TABLE
    - Validates column references (best-effort)
    |
    v
[Optimizer] (optimizer.h / optimizer.cpp)
    - Pass 1: Constant folding (2 + 3 -> 5)
    - Pass 2: Predicate pushdown analysis
    - Pass 3: Projection pruning analysis
    - Pass 4: Dead code elimination (WHERE 1=0)
    |
    v
[Code Generator] (codegen.h / codegen.cpp)
    - Visitor-style traversal of AST
    - Produces standard SQL strings
    - Handles special rewrites (mid-fr, percent-check, biggest-W/L)
    |
    v
[SQLite Execution] (main.cpp)
    - Executes generated SQL via sqlite3 C API
    - Prints results in key=value format
```

### Key Design Decisions

1. **Hyphenated keywords**: The lexer reads `[a-zA-Z_][a-zA-Z0-9_-]*` greedily. Since every hyphenated keyword is globally unique (no two keywords share the same full string), a simple hash map lookup suffices for keyword identification.

2. **Longest-match**: Because hyphens are consumed as part of the token word, `no-cap-not` is always read as one token and looked up as a single key, beating `no-cap` automatically.

3. **Analytics rewrites**: `mid-fr`, `percent-check`, `biggest-W`, and `biggest-L` are detected at the SELECT statement codegen level and rewritten to CTE-based or ORDER BY-based SQL patterns.

4. **Catalog persistence**: Schema is persisted in a JSON file (`.skibidi_catalog.json`) in the working directory, surviving across REPL sessions.
