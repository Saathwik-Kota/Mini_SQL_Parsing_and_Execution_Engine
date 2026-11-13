# Mini SQL Parsing & Execution Engine

[![CI](https://github.com/Saathwik-Kota/Mini_SQL_Parsing_and_Execution_Engine/actions/workflows/ci.yml/badge.svg)](https://github.com/Saathwik-Kota/Mini_SQL_Parsing_and_Execution_Engine/actions/workflows/ci.yml)
[![CodeQL Security](https://github.com/Saathwik-Kota/Mini_SQL_Parsing_and_Execution_Engine/actions/workflows/codeql.yml/badge.svg)](https://github.com/Saathwik-Kota/Mini_SQL_Parsing_and_Execution_Engine/actions/workflows/codeql.yml)
[![Code Quality & Lint](https://github.com/Saathwik-Kota/Mini_SQL_Parsing_and_Execution_Engine/actions/workflows/lint.yml/badge.svg)](https://github.com/Saathwik-Kota/Mini_SQL_Parsing_and_Execution_Engine/actions/workflows/lint.yml)
[![Standard](https://img.shields.io/badge/C-C99-00599C?logo=c)](https://en.wikipedia.org/wiki/C99)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows-blue)](https://github.com/Saathwik-Kota/Mini_SQL_Parsing_and_Execution_Engine)

A production-grade, relational database management engine implemented entirely from scratch in pure ANSI C99 with zero external dependencies.

This project implements core RDBMS internals, including a slotted-page storage manager supporting variable-length records with on-line page compaction, an in-memory buffer pool manager with Least Recently Used (LRU) eviction and write-back caching, an order-M B+ Tree index with dynamic split/merge balancing, a rule-based query planner and optimizer with access-path selection, and a pull-based Volcano iterator execution engine.

---

## Architecture Overview

```
+--------------------------------------------------------------------------+
|                                REPL / CLI                                |
|                        (Parser Tokenizer & AST)                          |
+--------------------------------------------------------------------------+
                                     |
                                     v
+--------------------------------------------------------------------------+
|                         Rule-Based Query Planner                         |
|   AST Traversal -> Index Predicate Matching -> Logical to Physical Plan  |
|               (Access Path Selection: SeqScan vs IndexScan)              |
+--------------------------------------------------------------------------+
                                     |
                                     v
+--------------------------------------------------------------------------+
|                         Volcano Execution Engine                         |
|      Pull-based Iterator Model (init, next, close) via Function Ptrs     |
|   Operators: Project, Filter, Sort, Aggregate, Join, IndexScan, SeqScan  |
+--------------------------------------------------------------------------+
                    |                                  |
                    v                                  v
+---------------------------------------+  +-------------------------------+
|             System Catalog            |  |       B+ Tree Indexing        |
|  Table Schemas, Metadatas, Heap Links |  | Multi-level Order-8 Balancing |
+---------------------------------------+  +-------------------------------+
                    |                                  |
                    +------------------+---------------+
                                       |
                                       v
+--------------------------------------------------------------------------+
|                          Buffer Pool Manager                             |
|          Fixed Frame Array, Hash Page Table, LRU Eviction Policy         |
|             Dirty Page Writeback & Cache Hit Rate Statistics             |
+--------------------------------------------------------------------------+
                                     |
                                     v
+--------------------------------------------------------------------------+
|                       Slotted-Page Storage Engine                        |
|        4KB Pages: Header, Slot Array Growing Down, Tuples Growing Up     |
|             True VARCHAR Variable-Length Records & Compaction            |
+--------------------------------------------------------------------------+
                                     |
                                     v
+--------------------------------------------------------------------------+
|                               Disk Manager                               |
|               Raw Binary File I/O (PAGE_SIZE = 4096 bytes)               |
+--------------------------------------------------------------------------+
```

---

## Core Components

### 1. Storage & Slotted Pages (`slotted_page.h`, `slotted_page.c`)
- **Page Layout**: Fixed 4096-byte (4KB) pages aligned with hardware disk sectors.
- **Header**:
  - `page_id_t page_id`: Physical page identifier.
  - `page_id_t next_page_id`, `page_id_t prev_page_id`: Doubly-linked list for sequential table scans.
  - `uint16_t num_slots`: Number of slot directory entries.
  - `uint16_t free_space_offset`: Byte offset marking the boundary where variable-length records grow backward from the top of the page (offset 4096).
  - `uint16_t tuple_count`: Active (non-deleted) record count.
- **Variable-Length Record Handling**: True `VARCHAR` support. Slot directory contains `(offset, length)` pairs. Tuples store dynamic attribute counts followed by typed data payloads (8-byte integers, 2-byte prefix length + raw string bytes).
- **Fragmentation & Compaction**: When records are deleted or updated with varying lengths, dead space is created. `slotted_page_compact()` shifts all active records toward the end of the page and defragments contiguous free memory.

### 2. Buffer Pool Manager (`buffer_pool.h`, `buffer_pool.c`)
- **Frame Allocation**: Manages a fixed array of in-memory frames (default: 64 frames = 256KB pool).
- **Page Table**: Multiplicative hash map mapping `page_id -> frame_id` with bucket chaining for O(1) lookups.
- **Eviction Policy**: Least Recently Used (LRU) replacer using an unpinned doubly-linked list.
- **Dirty Page Management**: Modified pages are flagged dirty and flushed to disk on eviction or explicit synchronization (`bpm_flush_all`).
- **Cache Hit Rate Tracking**: Monitors total page requests, cache hits, misses, and displays runtime hit rate percentages.

### 3. B+ Tree Indexing (`bplus_tree.h`, `bplus_tree.c`)
- **Structure**: Multi-level balanced search tree with order $M = 8$.
- **Leaf Nodes**: Contain sorted `(key, RID)` pairs and bidirectional sibling pointers (`next`, `prev`) for range scans.
- **Internal Nodes**: Contain separator keys and child node pointers.
- **Non-Unique / Duplicate Key Support**: The catalog permits indexing columns without requiring unique values. The B+ tree supports non-unique keys through contiguous ordering across leaf nodes. The `bplus_tree_find_all()` API and `IndexScanExecutor` retrieve and yield all matching records for any given key.
- **Balancing Operations**:
  - Node splitting upon overflow ($N > M$).
  - Separator promotion up through parent hierarchy to dynamic root creation.
  - Underflow handling via sibling borrowing and node merging.

### 4. Rule-Based Query Planner (`planner.h`, `planner.c`)
- Traverses AST generated by the SQL parser.
- Inspects `WHERE` predicates against indexed table columns.
- **Optimization Rule**: If an equality condition matches an indexed column (`col = literal`), the planner routes execution to an `IndexScan` ($O(\log N)$) rather than a table heap `SeqScan` ($O(N)$).
- **Plan Inspection**: Supports `EXPLAIN <query>` to visualize physical execution plans.

### 5. Volcano Execution Engine (`executor.h`, `src/executor.c`)
- Implements the iterator model through function pointers simulating polymorphism:
  - `init(executor_t* self)`: Initializes operators, allocates state buffers, opens child iterators.
  - `next(executor_t* self, tuple_t* out)`: Pulls a tuple up the execution pipeline. Returns `false` upon exhaustion.
  - `close(executor_t* self)`: Releases state buffers and child iterators.
- **Operators**:
  - `SeqScanExecutor`: Iterates slotted pages in the table heap.
  - `IndexScanExecutor`: Probes B+ Tree index directly for target record IDs.
  - `FilterExecutor`: Evaluates boolean predicates on stream tuples.
  - `ProjectExecutor`: Maps columns and formats projected result attributes.
  - `NestedLoopJoinExecutor`: Inner-outer relational join algorithm.
  - `SortExecutor`: In-memory quicksort supporting `ASC` and `DESC`.
  - `AggregateExecutor`: Computes `COUNT` and `SUM` with optional `GROUP BY`.
  - `UpdateExecutor` & `DeleteExecutor`: Mutates table heap records and updates B+ Tree indexes.

---

## Benchmark Results

Empirical performance evaluation comparing Sequential Table Scans against B+ Tree Index Lookups across varying dataset sizes. Benchmarks were conducted on AMD64 architecture with 4KB slotted pages and an LRU buffer pool of 64 frames.

| Record Count | Sequential Scan Time | SeqScan Pages Read | B+ Tree Index Scan Time | Index Pages Read | Speedup Factor |
|:------------:|:--------------------:|:------------------:|:-----------------------:|:----------------:|:--------------:|
| 1,000        | 0.084 ms             | 12                 | 0.001 ms                | 1                | 140.0x faster  |
| 10,000       | 1.520 ms             | 119                | 0.003 ms                | 1                | 506.7x faster  |
| 50,000       | 7.594 ms             | 593                | 0.010 ms                | 1                | 767.0x faster  |
| 100,000      | 14.715 ms            | 1,188              | 0.011 ms                | 1                | 1,375.3x faster|
| 250,000      | 36.980 ms            | 2,995              | 0.009 ms                | 1                | 3,934.0x faster|

### Key Takeaways
1. **Algorithmic Complexity**: Sequential scan latency scales linearly (O(N)), whereas B+ Tree lookup latency remains nearly flat (O(log N)) across all orders of magnitude.
2. **Buffer Efficiency**: Sequential scan requires sweeping the entire table heap (2,995 pages for 250,000 rows), inducing buffer churn. Index scan requires accessing only the specific leaf page matching the target key.
3. **Throughput**: Bulk tuple insertion into slotted pages with concurrent B+ Tree indexing exceeds 1.5 million tuples per second.

---

## Directory Structure

```
.
|-- Makefile               # Build automation (all, run, benchmark, valgrind, clean)
|-- README.md              # Project documentation and benchmarks
|-- .gitignore             # Git ignore patterns for binaries, data, and cache
|-- .clangd                # Clangd language server system include configuration
|-- include/
|   |-- types.h            # Schema definitions, values, RID, tuple structures
|   |-- disk_manager.h     # Raw block-level file I/O operations
|   |-- slotted_page.h     # 4KB slotted-page layout, variable-length serialization
|   |-- buffer_pool.h      # LRU buffer pool manager and frame definitions
|   |-- bplus_tree.h       # Order-M B+ Tree index structures and declarations
|   |-- catalog.h          # System catalog and table heap management
|   |-- parser.h           # SQL tokenizer, AST nodes, and expression evaluator
|   |-- planner.h          # Rule-based query planner and EXPLAIN tree printer
|   `-- executor.h         # Volcano iterator interface and execution nodes
|-- src/
|   |-- types.c            # Value constructors and comparison routines
|   |-- disk_manager.c     # Page-level disk read/write/allocation
|   |-- slotted_page.c     # Slotted-page tuple insertion, deletion, compaction
|   |-- buffer_pool.c      # Buffer pool frame hashing, LRU replacer, writeback
|   |-- bplus_tree.c       # B+ Tree node split, merge, search, range scans
|   |-- catalog.c          # Table heap insertion, record retrieval, indexing
|   |-- parser.c           # Handwritten recursive-descent SQL parser
|   |-- planner.c          # Rule-based access path optimizer
|   |-- executor.c         # Volcano iterator implementations
|   `-- main.c             # Interactive REPL, benchmark suite, stats reporting
`-- tests/
    `-- test_suite.c       # Automated test suite (7 subsystems, 117 assertions)
```

---

## Build & Execution Instructions

### Prerequisites
- A standard C99 compiler (GCC 4.9+ or Clang 3.8+).
- GNU Make (or GCC direct invocation).

### Compilation
Using GNU Make:
```bash
make all
```

Using GCC directly:
```bash
gcc -Wall -Wextra -O2 -g -std=c99 -Iinclude -o mini_sql src/*.c
```

### Running the REPL
```bash
./mini_sql
```

### Running Automated Test Suite
The project contains an end-to-end automated unit and integration test suite (`tests/test_suite.c`) covering all 7 engine subsystems:
1. **Disk Manager**: Page allocation, block read/write operations, multi-page bounds.
2. **Slotted Page**: Variable-length tuple serialization, slot allocation, tuple deletion, and defragmenting page compaction.
3. **Buffer Pool**: LRU eviction verification under load (exceeding frame capacity), dirty page disk flushing, and cache hit rate tracking.
4. **B+ Tree Indexing**: Node splits across leaf and internal nodes, non-unique duplicate keys across split boundaries (`bplus_tree_find_all`), and deletion/borrowing.
5. **SQL Parser & AST**: Lexical analysis and AST generation for DDL, DML, WHERE predicates, ORDER BY, GROUP BY, aggregates, and joins.
6. **Query Planner**: Access path optimization (rule-based selection of `PLAN_INDEX_SCAN` on indexed columns vs `PLAN_SEQ_SCAN`), and plan tree generation.
7. **Volcano Execution Engine**: Full execution pipeline verification including SeqScan, IndexScan yielding non-unique duplicate keys, Filter, Project, Sort, Aggregate, Join, Update, and Delete.

Run the test suite using GNU Make:
```bash
make test
```
Or directly:
```bash
gcc -Wall -Wextra -O2 -g -std=c99 -Iinclude -o test_suite tests/test_suite.c src/types.c src/disk_manager.c src/slotted_page.c src/buffer_pool.c src/bplus_tree.c src/catalog.c src/parser.c src/planner.c src/executor.c
./test_suite
```
Or invoke directly via the CLI:
```bash
./mini_sql --test
```
Or within the REPL session:
```
minisql> .test
```

### Running Benchmarks
Run the automated benchmark suite:
```bash
make benchmark
```
Or inside the REPL:
```
minisql> .benchmark 100000
```

### Valgrind Memory Verification
Run memory leak and pointer correctness tests across the automated test suite:
```bash
make valgrind-test
```
Or test the interactive REPL and benchmark under Valgrind:
```bash
make valgrind
```

---

## Continuous Integration & Quality Assurance

The repository includes enterprise-grade GitHub Actions workflows to ensure reliability, correctness, and memory safety across platforms:

- **Multi-Platform Matrix (`.github/workflows/ci.yml`)**:
  - Compiles and runs the full test suite and benchmark on **Ubuntu (GCC & Clang)**, **macOS (Clang)**, and **Windows (MinGW GCC)**.
  - Generates downloadable standalone binary build artifacts for all platforms.
- **Memory Safety Sanitizers**:
  - Automatically compiles and executes test suites under **AddressSanitizer (ASan)** and **UndefinedBehaviorSanitizer (UBSan)** to detect buffer overruns, use-after-free, and unaligned accesses.
- **Dynamic Analysis with Valgrind**:
  - Runs automated leak detection on every push and pull request, requiring **0 byte memory leaks** across all 7 database subsystems and the REPL benchmark.
- **CodeQL Security Analysis (`.github/workflows/codeql.yml`)**:
  - GitHub Advanced Security static analysis scanning for CWE security vulnerabilities and unchecked memory flaws.
- **Static Analysis & Linting (`.github/workflows/lint.yml`)**:
  - Enforces zero compiler warnings with `-Wall -Wextra -Werror` and verifies code quality via **Cppcheck** and **clang-format**.
- **Automated Multi-Platform Release (`.github/workflows/release.yml`)**:
  - Automatically packages and signs release archives (`.tar.gz` and `.zip`) with SHA-256 checksums on tag releases.

---

## SQL Reference & REPL Commands

### REPL Meta Commands
| Command | Description |
|:---|:---|
| `.help` | Displays help manual and SQL syntax |
| `.tables` | Lists all tables in the database catalog |
| `.schema <table>` | Displays column definitions, types, and index metadata |
| `.stats` | Prints buffer pool hit rate, requests, misses, and disk I/O |
| `.clear_stats` | Resets buffer pool request and hit counters |
| `.test` | Runs the internal automated test suite (117 assertions across 7 subsystems) |
| `.benchmark [N]` | Runs automated benchmark with N rows (default: 100000) |
| `.exit` / `.quit` | Synchronizes dirty pages to disk and terminates engine |

### Supported SQL Syntax
```sql
-- DDL
CREATE TABLE users (id INT, name VARCHAR(32), age INT);
CREATE INDEX ON users (id);

-- DML
INSERT INTO users VALUES (1, 'Alice', 25);
INSERT INTO users VALUES (2, 'Bob', 30);
UPDATE users SET age = 26 WHERE id = 1;
DELETE FROM users WHERE id = 2;

-- Querying
SELECT * FROM users;
SELECT id, name FROM users WHERE id = 1;
SELECT * FROM users WHERE age > 20 ORDER BY age DESC;
SELECT COUNT(*), SUM(age) FROM users;
SELECT age, COUNT(*) FROM users GROUP BY age;

-- Relational Joins
SELECT users.name, orders.amount FROM users JOIN orders ON users.id = orders.user_id;

-- Query Planning Inspection
EXPLAIN SELECT * FROM users WHERE id = 1;
EXPLAIN SELECT * FROM users WHERE age > 20;
```

---

## Limitations & Future Work

1. **Write-Ahead Logging (WAL)**: Transactions currently commit immediately to the buffer pool without ARIES-style WAL (`undo`/`redo` logging).
2. **Concurrency Control**: Engine is single-threaded. Adding latching on buffer frames and Two-Phase Locking (2PL) for serializable transactions is planned.
3. **Secondary Index Persistence**: B+ Tree index structures are currently memory-backed and rebuilt from the table heap on catalog initialization. Serializing B+ tree nodes directly into index page frames is the next architectural milestone.
4. **Cost-Based Optimization (CBO)**: Current optimizer uses a rule-based heuristic. Implementing table statistics (histograms) for cost-based join reordering will further optimize complex queries.
5. **System Catalog & Cross-Session Schema Persistence (Page 0 Serialization)**: While raw 4KB slotted pages and record data are flushed and persisted to disk (`minisql.dat`), the system catalog (table schemas, column definitions, and root page pointers) is currently session-scoped in memory. Serializing the catalog metadata into a dedicated Page 0 meta-page for automatic schema rehydration across process restarts is planned as future work.

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

