# Mini SQL Parsing & Execution Engine

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
`-- src/
    |-- types.c            # Value constructors and comparison routines
    |-- disk_manager.c     # Page-level disk read/write/allocation
    |-- slotted_page.c     # Slotted-page tuple insertion, deletion, compaction
    |-- buffer_pool.c      # Buffer pool frame hashing, LRU replacer, writeback
    |-- bplus_tree.c       # B+ Tree node split, merge, search, range scans
    |-- catalog.c          # Table heap insertion, record retrieval, indexing
    |-- parser.c           # Handwritten recursive-descent SQL parser
    |-- planner.c          # Rule-based access path optimizer
    |-- executor.c         # Volcano iterator implementations
    `-- main.c             # Interactive REPL, benchmark suite, stats reporting
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
Run memory leak and pointer correctness tests:
```bash
make valgrind
```

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

