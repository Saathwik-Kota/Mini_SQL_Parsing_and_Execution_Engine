#include "../include/types.h"
#include "../include/disk_manager.h"
#include "../include/slotted_page.h"
#include "../include/buffer_pool.h"
#include "../include/bplus_tree.h"
#include "../include/catalog.h"
#include "../include/parser.h"
#include "../include/planner.h"
#include "../include/executor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

static double get_time_ms(void) {
    return ((double)clock() * 1000.0) / (double)CLOCKS_PER_SEC;
}

static void print_table_result(const schema_t* schema, tuple_t* tuples, size_t count, double time_ms, double hit_rate) {
    if (schema->num_columns == 0) {
        printf("Empty schema.\n");
        return;
    }

    size_t col_widths[MAX_COLUMNS];
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        col_widths[i] = strlen(schema->columns[i].name);
        if (col_widths[i] < 4) col_widths[i] = 4;
    }

    /* Measure maximum width per column */
    for (size_t r = 0; r < count; r++) {
        for (uint32_t c = 0; c < schema->num_columns && c < tuples[r].num_values; c++) {
            char buf[MAX_VARCHAR_LEN];
            value_to_string(&tuples[r].values[c], buf, sizeof(buf));
            size_t len = strlen(buf);
            if (len > col_widths[c]) {
                col_widths[c] = len;
            }
        }
    }

    /* Print horizontal divider */
    printf("+");
    for (uint32_t c = 0; c < schema->num_columns; c++) {
        for (size_t i = 0; i < col_widths[c] + 2; i++) printf("-");
        printf("+");
    }
    printf("\n");

    /* Print header row */
    printf("|");
    for (uint32_t c = 0; c < schema->num_columns; c++) {
        printf(" %-*s |", (int)col_widths[c], schema->columns[c].name);
    }
    printf("\n");

    /* Print separator */
    printf("+");
    for (uint32_t c = 0; c < schema->num_columns; c++) {
        for (size_t i = 0; i < col_widths[c] + 2; i++) printf("-");
        printf("+");
    }
    printf("\n");

    /* Print data rows */
    for (size_t r = 0; r < count; r++) {
        printf("|");
        for (uint32_t c = 0; c < schema->num_columns; c++) {
            char buf[MAX_VARCHAR_LEN];
            if (c < tuples[r].num_values) {
                value_to_string(&tuples[r].values[c], buf, sizeof(buf));
            } else {
                strcpy(buf, "NULL");
            }
            if (schema->columns[c].type == TYPE_INT) {
                printf(" %*s |", (int)col_widths[c], buf);
            } else {
                printf(" %-*s |", (int)col_widths[c], buf);
            }
        }
        printf("\n");
    }

    /* Print bottom divider */
    printf("+");
    for (uint32_t c = 0; c < schema->num_columns; c++) {
        for (size_t i = 0; i < col_widths[c] + 2; i++) printf("-");
        printf("+");
    }
    printf("\n");

    printf("%zu row%s in set (%.3f ms) [Buffer Hit Rate: %.1f%%]\n\n",
           count, count == 1 ? "" : "s", time_ms, hit_rate);
}

static void execute_statement(catalog_t* catalog, const statement_t* stmt, const char* original_sql) {
    (void)original_sql;

    if (stmt->type == STMT_CREATE_TABLE) {
        double start = get_time_ms();
        bool ok = catalog_create_table(catalog, stmt->create_table.table_name, &stmt->create_table.schema);
        double dur = get_time_ms() - start;
        if (ok) {
            printf("Table '%s' created successfully (%.3f ms).\n\n", stmt->create_table.table_name, dur);
        } else {
            printf("Error: Failed to create table '%s' (table already exists or limit reached).\n\n", stmt->create_table.table_name);
        }
        return;
    }

    if (stmt->type == STMT_CREATE_INDEX) {
        double start = get_time_ms();
        bool ok = catalog_create_index(catalog, stmt->create_index.table_name, stmt->create_index.column_name);
        double dur = get_time_ms() - start;
        if (ok) {
            printf("B+ Tree index created on %s(%s) successfully (%.3f ms).\n\n",
                   stmt->create_index.table_name, stmt->create_index.column_name, dur);
        } else {
            printf("Error: Failed to create index on %s(%s).\n\n",
                   stmt->create_index.table_name, stmt->create_index.column_name);
        }
        return;
    }

    /* Plan statement */
    plan_node_t* plan = plan_create(catalog, stmt);
    if (!plan) {
        printf("Error: Failed to generate execution plan (check table or column names).\n\n");
        return;
    }

    if (stmt->is_explain) {
        printf("=== Query Execution Plan ===\n");
        plan_explain(plan, 0);
        printf("\n");
        plan_destroy(plan);
        return;
    }

    /* Create and execute Volcano iterator pipeline */
    executor_t* exec = executor_create(catalog, plan);
    if (!exec) {
        printf("Error: Failed to initialize executor.\n\n");
        plan_destroy(plan);
        return;
    }

    double start = get_time_ms();
    exec->init(exec);

    size_t capacity = 128;
    size_t count = 0;
    tuple_t* results = (tuple_t*)malloc(capacity * sizeof(tuple_t));

    tuple_t t;
    while (exec->next(exec, &t)) {
        if (count >= capacity) {
            size_t new_cap = capacity * 2;
            tuple_t* new_res = (tuple_t*)realloc(results, new_cap * sizeof(tuple_t));
            if (new_res) results = new_res;
            capacity = new_cap;
        }
        results[count++] = t;
    }
    exec->close(exec);
    double duration = get_time_ms() - start;

    double hit_rate = bpm_get_hit_rate(catalog->bpm);

    if (stmt->type == STMT_INSERT || stmt->type == STMT_UPDATE || stmt->type == STMT_DELETE) {
        int64_t affected = (count > 0 && results[0].num_values > 0) ? results[0].values[0].int_val : 0;
        const char* verb = (stmt->type == STMT_INSERT) ? "inserted" : (stmt->type == STMT_UPDATE) ? "updated" : "deleted";
        printf("Query OK, %lld row%s %s (%.3f ms) [Buffer Hit Rate: %.1f%%]\n\n",
               (long long)affected, affected == 1 ? "" : "s", verb, duration, hit_rate);
    } else {
        print_table_result(&exec->schema, results, count, duration, hit_rate);
    }

    free(results);
    executor_destroy(exec);
    plan_destroy(plan);
}

static void run_benchmark(catalog_t* catalog, size_t row_count) {
    printf("=================================================================\n");
    printf("    DATABASE SYSTEMS BENCHMARK: SeqScan vs B+ Tree IndexScan     \n");
    printf("=================================================================\n");
    printf("Records to insert : %zu\n", row_count);
    printf("Storage Model     : 4KB Slotted Pages with Variable-Length Tuples\n");
    printf("Eviction Policy   : LRU Buffer Pool (Size: %zu frames)\n", catalog->bpm->pool_size);
    printf("Index Structure   : Multi-level B+ Tree (Order: %d)\n", BPLUS_MAX_KEYS);
    printf("-----------------------------------------------------------------\n");

    /* Create benchmark table */
    schema_t schema;
    schema.num_columns = 3;
    strcpy(schema.columns[0].name, "id");
    schema.columns[0].type = TYPE_INT;
    schema.columns[0].max_len = sizeof(int64_t);

    strcpy(schema.columns[1].name, "val");
    schema.columns[1].type = TYPE_INT;
    schema.columns[1].max_len = sizeof(int64_t);

    strcpy(schema.columns[2].name, "note");
    schema.columns[2].type = TYPE_VARCHAR;
    schema.columns[2].max_len = 64;

    catalog_create_table(catalog, "bench", &schema);
    table_t* table = catalog_get_table(catalog, "bench");
    if (!table) {
        printf("Error: Could not create benchmark table.\n");
        return;
    }

    /* Create index on id */
    catalog_create_index(catalog, "bench", "id");

    /* Populate data */
    printf("Populating %zu records into 'bench'...\n", row_count);
    double t_insert_start = get_time_ms();

    for (size_t i = 1; i <= row_count; i++) {
        tuple_t t;
        t.num_values = 3;
        t.values[0] = value_make_int((int64_t)i);
        t.values[1] = value_make_int((int64_t)(i * 3 + 7));

        char note_buf[64];
        snprintf(note_buf, sizeof(note_buf), "user_record_val_%zu", i);
        t.values[2] = value_make_str(note_buf);

        rid_t rid;
        table_insert_tuple(table, catalog->bpm, &t, &rid);
    }

    double t_insert_end = get_time_ms();
    double insert_total_ms = t_insert_end - t_insert_start;
    double throughput = (double)row_count / (insert_total_ms / 1000.0);
    printf("Insertion complete: %.2f ms (%.0f tuples/sec)\n", insert_total_ms, throughput);
    printf("B+ Tree Entries: %zu (Depth: %d)\n", bplus_tree_get_size(table->index), table->index->depth);

    /* Flush and reset stats for fair scan testing */
    bpm_flush_all(catalog->bpm);
    bpm_reset_stats(catalog->bpm);

    /* Target key near the 85% mark */
    int64_t target_id = (int64_t)(row_count * 85 / 100);
    printf("\nTarget Point Lookup: WHERE id = %lld\n", (long long)target_id);

    /* 1. Sequential Scan Benchmark */
    printf("\nRunning Sequential Scan (Heap Scan O(N))...\n");
    expr_t* seq_where = expr_create_binary(OP_EQ, expr_create_col_ref(NULL, "val"), expr_create_literal(value_make_int(target_id * 3 + 7)));
    statement_t seq_stmt;
    memset(&seq_stmt, 0, sizeof(seq_stmt));
    seq_stmt.type = STMT_SELECT;
    strcpy(seq_stmt.select_stmt.from_table, "bench");
    seq_stmt.select_stmt.num_projections = 1;
    seq_stmt.select_stmt.projections[0].type = PROJ_ALL;
    seq_stmt.select_stmt.where_clause = seq_where;

    plan_node_t* seq_plan = plan_create(catalog, &seq_stmt);
    executor_t* seq_exec = executor_create(catalog, seq_plan);
    bpm_reset_stats(catalog->bpm);

    double t_seq_start = get_time_ms();
    seq_exec->init(seq_exec);
    tuple_t res_tuple;
    size_t seq_found = 0;
    while (seq_exec->next(seq_exec, &res_tuple)) {
        seq_found++;
    }
    seq_exec->close(seq_exec);
    double t_seq_end = get_time_ms();
    double seq_duration_ms = t_seq_end - t_seq_start;
    double seq_hit_rate = bpm_get_hit_rate(catalog->bpm);
    uint64_t seq_reqs = catalog->bpm->num_requests;

    executor_destroy(seq_exec);
    expr_free(seq_where);
    plan_destroy(seq_plan);

    /* 2. B+ Tree Index Scan Benchmark */
    printf("Running B+ Tree Index Scan (Index Lookup O(log N))...\n");
    expr_t* idx_where = expr_create_binary(OP_EQ, expr_create_col_ref(NULL, "id"), expr_create_literal(value_make_int(target_id)));
    statement_t idx_stmt;
    memset(&idx_stmt, 0, sizeof(idx_stmt));
    idx_stmt.type = STMT_SELECT;
    strcpy(idx_stmt.select_stmt.from_table, "bench");
    idx_stmt.select_stmt.num_projections = 1;
    idx_stmt.select_stmt.projections[0].type = PROJ_ALL;
    idx_stmt.select_stmt.where_clause = idx_where;

    plan_node_t* idx_plan = plan_create(catalog, &idx_stmt);
    executor_t* idx_exec = executor_create(catalog, idx_plan);
    bpm_reset_stats(catalog->bpm);

    double t_idx_start = get_time_ms();
    idx_exec->init(idx_exec);
    size_t idx_found = 0;
    while (idx_exec->next(idx_exec, &res_tuple)) {
        idx_found++;
    }
    idx_exec->close(idx_exec);
    double t_idx_end = get_time_ms();
    double idx_duration_ms = t_idx_end - t_idx_start;
    double idx_hit_rate = bpm_get_hit_rate(catalog->bpm);
    uint64_t idx_reqs = catalog->bpm->num_requests;

    executor_destroy(idx_exec);
    expr_free(idx_where);
    plan_destroy(idx_plan);

    /* Print Benchmark Results Table */
    printf("\n+--------------------+---------------------+---------------------+\n");
    printf("| Metric             | Sequential Scan     | B+ Tree Index Scan  |\n");
    printf("+--------------------+---------------------+---------------------+\n");
    printf("| Access Strategy    | Table Heap Scan     | B+ Tree Point Probe |\n");
    printf("| Theoretical Cost   | O(N)                | O(log N)            |\n");
    printf("| Execution Time     | %14.3f ms   | %14.3f ms   |\n", seq_duration_ms, idx_duration_ms);
    printf("| Buffer Requests    | %19llu | %19llu |\n", (unsigned long long)seq_reqs, (unsigned long long)idx_reqs);
    printf("| Buffer Hit Rate    | %17.1f %% | %17.1f %% |\n", seq_hit_rate, idx_hit_rate);
    printf("| Tuples Retrieved   | %19zu | %19zu |\n", seq_found, idx_found);

    double speedup = (idx_duration_ms > 0.00001) ? (seq_duration_ms / idx_duration_ms) : (seq_duration_ms / 0.001);
    printf("| Speedup Multiplier | 1.00x (Baseline)    | %15.1fx faster |\n", speedup);
    printf("+--------------------+---------------------+---------------------+\n\n");
}

static void print_help(void) {
    printf("Mini SQL Parsing & Execution Engine (Pure C99)\n");
    printf("Commands:\n");
    printf("  .help                 Show this help manual\n");
    printf("  .tables               List all tables in catalog\n");
    printf("  .schema <table>       Show schema and indexes for a table\n");
    printf("  .stats                Show Buffer Pool Manager statistics and hit rate\n");
    printf("  .clear_stats          Reset Buffer Pool statistics\n");
    printf("  .benchmark [N]        Run SeqScan vs IndexScan benchmark (default: 100000)\n");
    printf("  .test                 Run the automated test suite\n");
    printf("  .exit / .quit         Flush pages to disk and exit\n\n");
    printf("SQL Features Supported:\n");
    printf("  CREATE TABLE tbl (col1 INT, col2 VARCHAR(len), ...);\n");
    printf("  CREATE INDEX [name] ON tbl (col);\n");
    printf("  INSERT INTO tbl VALUES (val1, val2, ...);\n");
    printf("  SELECT [* | col1, col2] FROM tbl [JOIN tbl2 ON cond] [WHERE cond] [GROUP BY col] [ORDER BY col [ASC|DESC]];\n");
    printf("  SELECT COUNT(*), SUM(col) FROM tbl [GROUP BY col];\n");
    printf("  UPDATE tbl SET col = val [WHERE cond];\n");
    printf("  DELETE FROM tbl [WHERE cond];\n");
    printf("  EXPLAIN <query>;\n\n");
}

static void print_schema(catalog_t* catalog, const char* tbl_name) {
    table_t* table = catalog_get_table(catalog, tbl_name);
    if (!table) {
        printf("Table '%s' not found.\n\n", tbl_name);
        return;
    }

    printf("Table: %s (Tuples: %llu, First Page: %u, Last Page: %u)\n",
           table->name, (unsigned long long)table->num_tuples, table->first_page_id, table->last_page_id);
    printf("+--------------------------------+------------+------------+\n");
    printf("| Column Name                    | Type       | Max Length |\n");
    printf("+--------------------------------+------------+------------+\n");
    for (uint32_t i = 0; i < table->schema.num_columns; i++) {
        const char* type_str = (table->schema.columns[i].type == TYPE_INT) ? "INT" : "VARCHAR";
        printf("| %-30s | %-10s | %-10u |\n",
               table->schema.columns[i].name, type_str, table->schema.columns[i].max_len);
    }
    printf("+--------------------------------+------------+------------+\n");

    if (table->has_index) {
        printf("Index: '%s' on column '%s' (B+ Tree Depth: %d, Entries: %zu)\n\n",
               table->index_name, table->schema.columns[table->index_column_idx].name,
               table->index ? table->index->depth : 0,
               table->index ? table->index->size : 0);
    } else {
        printf("Indexes: None\n\n");
    }
}

static void print_tables(catalog_t* catalog) {
    if (catalog->num_tables == 0) {
        printf("No tables in database.\n\n");
        return;
    }
    printf("+--------------------------------+------------+------------+\n");
    printf("| Table Name                     | Columns    | Rows       |\n");
    printf("+--------------------------------+------------+------------+\n");
    for (uint32_t i = 0; i < catalog->num_tables; i++) {
        table_t* t = &catalog->tables[i];
        printf("| %-30s | %-10u | %-10llu |\n",
               t->name, t->schema.num_columns, (unsigned long long)t->num_tuples);
    }
    printf("+--------------------------------+------------+------------+\n\n");
}

int main(int argc, char* argv[]) {
    if (argc > 1 && (strcmp(argv[1], "--test") == 0 || strcmp(argv[1], "-t") == 0)) {
        return system(
#if defined(_WIN32)
            "test_suite.exe"
#else
            "./test_suite"
#endif
        );
    }

    const char* db_file = "minisql.dat";
    if (argc > 1) {
        db_file = argv[1];
    }

    printf("=======================================================================\n");
    printf("   Mini SQL Parsing & Execution Engine (Pure C99 Production Grade)    \n");
    printf("=======================================================================\n");
    printf("Storage Engine   : 4KB Slotted Pages with True VARCHAR & Compaction   \n");
    printf("Buffer Manager   : %d Frames, LRU Eviction, Dirty Page Writeback      \n", DEFAULT_BUFFER_POOL_SIZE);
    printf("Indexing         : Multi-level B+ Tree with Split & Merge Logic       \n");
    printf("Query Optimizer  : Rule-Based Optimizer (IndexScan Routing & EXPLAIN) \n");
    printf("Execution Model  : Volcano Iterator Pipeline (Polymorphic Function Ptrs)\n");
    printf("Type '.help' for instructions, '.benchmark' for performance testing.   \n\n");

    disk_manager_t* dm = disk_manager_open(db_file);
    if (!dm) {
        fprintf(stderr, "Fatal: Could not open database file '%s'.\n", db_file);
        return 1;
    }

    buffer_pool_manager_t* bpm = bpm_create(dm, DEFAULT_BUFFER_POOL_SIZE);
    if (!bpm) {
        fprintf(stderr, "Fatal: Could not initialize Buffer Pool Manager.\n");
        disk_manager_close(dm);
        return 1;
    }

    catalog_t* catalog = catalog_create(bpm);
    if (!catalog) {
        fprintf(stderr, "Fatal: Could not initialize System Catalog.\n");
        bpm_destroy(bpm);
        disk_manager_close(dm);
        return 1;
    }

    char query_buffer[4096];
    query_buffer[0] = '\0';

    while (1) {
        if (query_buffer[0] == '\0') {
            printf("minisql> ");
        } else {
            printf("   ...> ");
        }
        fflush(stdout);

        char line[1024];
        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }

        /* Trim leading whitespace */
        char* p = line;
        while (isspace((unsigned char)*p)) p++;

        if (query_buffer[0] == '\0') {
            if (*p == '.') {
                /* Dot commands */
                if (strncmp(p, ".exit", 5) == 0 || strncmp(p, ".quit", 5) == 0) {
                    break;
                } else if (strncmp(p, ".help", 5) == 0) {
                    print_help();
                } else if (strncmp(p, ".tables", 7) == 0) {
                    print_tables(catalog);
                } else if (strncmp(p, ".schema", 7) == 0) {
                    char tbl[64];
                    if (sscanf(p + 7, "%63s", tbl) == 1) {
                        print_schema(catalog, tbl);
                    } else {
                        printf("Usage: .schema <table_name>\n\n");
                    }
                } else if (strncmp(p, ".stats", 6) == 0) {
                    bpm_print_stats(catalog->bpm);
                    printf("\n");
                } else if (strncmp(p, ".clear_stats", 12) == 0) {
                    bpm_reset_stats(catalog->bpm);
                    printf("Buffer pool stats reset.\n\n");
                } else if (strncmp(p, ".test", 5) == 0) {
                    printf("Running automated test suite...\n");
                    int res = system(
#if defined(_WIN32)
                        "test_suite.exe"
#else
                        "./test_suite"
#endif
                    );
                    (void)res;
                    printf("\n");
                } else if (strncmp(p, ".benchmark", 10) == 0) {
                    size_t rows = 100000;
                    sscanf(p + 10, "%zu", &rows);
                    if (rows < 1000) rows = 1000;
                    run_benchmark(catalog, rows);
                } else {
                    printf("Unknown command: %s. Type '.help' for available commands.\n\n", p);
                }
                continue;
            }
        }

        strncat(query_buffer, line, sizeof(query_buffer) - strlen(query_buffer) - 1);

        /* Check if statement is terminated with semicolon */
        size_t qlen = strlen(query_buffer);
        bool has_semicolon = false;
        for (size_t i = 0; i < qlen; i++) {
            if (query_buffer[i] == ';') {
                has_semicolon = true;
                break;
            }
        }

        if (has_semicolon) {
            statement_t stmt;
            if (parse_sql(query_buffer, &stmt)) {
                execute_statement(catalog, &stmt, query_buffer);
                statement_free(&stmt);
            } else {
                printf("Syntax Error: Could not parse query.\n\n");
            }
            query_buffer[0] = '\0';
        }
    }

    printf("\nFlushing buffer pool and saving database state...\n");
    bpm_flush_all(catalog->bpm);
    catalog_destroy(catalog);
    bpm_destroy(bpm);
    disk_manager_close(dm);

    printf("Engine shutdown cleanly. Goodbye!\n");
    return 0;
}
