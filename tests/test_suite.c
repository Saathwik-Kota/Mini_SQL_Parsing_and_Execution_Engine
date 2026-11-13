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
#include <assert.h>
#include <stdbool.h>

#include <time.h>

static double get_time_ms(void) {
    return ((double)clock() * 1000.0) / (double)CLOCKS_PER_SEC;
}

static int total_tests = 0;
static int passed_tests = 0;
static int failed_tests = 0;

#define TEST_ASSERT(cond, msg) do { \
    total_tests++; \
    if (cond) { \
        passed_tests++; \
    } else { \
        failed_tests++; \
        fprintf(stderr, "  [FAILED] Line %d: %s\n", __LINE__, msg); \
    } \
} while (0)

#define RUN_TEST(test_func) do { \
    printf("Running %-50s ... ", #test_func); \
    fflush(stdout); \
    double t0 = get_time_ms(); \
    int prev_failed = failed_tests; \
    test_func(); \
    double t1 = get_time_ms(); \
    if (failed_tests == prev_failed) { \
        printf("[PASS] (%.2f ms)\n", t1 - t0); \
    } else { \
        printf("[FAIL] (%.2f ms)\n", t1 - t0); \
    } \
} while (0)

static void cleanup_file(const char* filename) {
    remove(filename);
}

/* ========================================================================= */
/* TEST 1: Disk Manager                                                      */
/* ========================================================================= */
static void test_disk_manager(void) {
    const char* test_db = "test_disk.dat";
    cleanup_file(test_db);

    disk_manager_t* dm = disk_manager_open(test_db);
    TEST_ASSERT(dm != NULL, "disk_manager_open failed");
    TEST_ASSERT(disk_manager_get_num_pages(dm) == 0, "Initial page count should be 0");

    page_id_t p0 = disk_manager_allocate_page(dm);
    TEST_ASSERT(p0 == 0, "First allocated page must be 0");
    TEST_ASSERT(disk_manager_get_num_pages(dm) == 1, "Page count should be 1");

    uint8_t write_buf[PAGE_SIZE] __attribute__((aligned(8)));
    memset(write_buf, 0xAB, PAGE_SIZE);
    bool wok = disk_manager_write_page(dm, p0, write_buf);
    TEST_ASSERT(wok, "disk_manager_write_page failed");

    uint8_t read_buf[PAGE_SIZE] __attribute__((aligned(8)));
    memset(read_buf, 0, PAGE_SIZE);
    bool rok = disk_manager_read_page(dm, p0, read_buf);
    TEST_ASSERT(rok, "disk_manager_read_page failed");
    TEST_ASSERT(memcmp(write_buf, read_buf, PAGE_SIZE) == 0, "Read data matches written data");

    disk_manager_close(dm);

    /* Reopen and verify persistence */
    dm = disk_manager_open(test_db);
    TEST_ASSERT(dm != NULL, "Reopening database file failed");
    TEST_ASSERT(disk_manager_get_num_pages(dm) == 1, "Page count persisted across close/open");

    memset(read_buf, 0, PAGE_SIZE);
    rok = disk_manager_read_page(dm, p0, read_buf);
    TEST_ASSERT(rok, "Reading page after reopen failed");
    TEST_ASSERT(memcmp(write_buf, read_buf, PAGE_SIZE) == 0, "Persisted page content matches");

    disk_manager_close(dm);
    cleanup_file(test_db);
}

/* ========================================================================= */
/* TEST 2: Slotted Page Storage & Compaction                                 */
/* ========================================================================= */
static void test_slotted_page_and_compaction(void) {
    uint8_t page[PAGE_SIZE] __attribute__((aligned(8)));
    slotted_page_init(page, 42);

    const slotted_page_header_t* hdr = slotted_page_get_header_const(page);
    TEST_ASSERT(hdr->page_id == 42, "Header page_id must match");
    TEST_ASSERT(hdr->num_slots == 0, "Initial slots should be 0");
    TEST_ASSERT(hdr->free_space_offset == PAGE_SIZE, "Free space offset starts at PAGE_SIZE");

    schema_t schema;
    schema.num_columns = 2;
    strcpy(schema.columns[0].name, "id");
    schema.columns[0].type = TYPE_INT;
    schema.columns[0].max_len = sizeof(int64_t);
    strcpy(schema.columns[1].name, "bio");
    schema.columns[1].type = TYPE_VARCHAR;
    schema.columns[1].max_len = 256;

    tuple_t t1;
    t1.num_values = 2;
    t1.values[0] = value_make_int(101);
    t1.values[1] = value_make_str("Variable length record alpha");

    uint8_t buf1[PAGE_SIZE] __attribute__((aligned(8)));
    uint16_t len1 = tuple_serialize(&schema, &t1, buf1, sizeof(buf1));
    TEST_ASSERT(len1 > 0, "Serialization of tuple 1 succeeded");

    uint16_t s0, s1, s2;
    bool ok0 = slotted_page_insert_tuple(page, buf1, len1, &s0);
    TEST_ASSERT(ok0 && s0 == 0, "Inserted slot 0");

    tuple_t t2;
    t2.num_values = 2;
    t2.values[0] = value_make_int(102);
    t2.values[1] = value_make_str("Beta record with different length");
    uint8_t buf2[PAGE_SIZE] __attribute__((aligned(8)));
    uint16_t len2 = tuple_serialize(&schema, &t2, buf2, sizeof(buf2));
    bool ok1 = slotted_page_insert_tuple(page, buf2, len2, &s1);
    TEST_ASSERT(ok1 && s1 == 1, "Inserted slot 1");

    tuple_t t3;
    t3.num_values = 2;
    t3.values[0] = value_make_int(103);
    t3.values[1] = value_make_str("Gamma record payload");
    uint8_t buf3[PAGE_SIZE] __attribute__((aligned(8)));
    uint16_t len3 = tuple_serialize(&schema, &t3, buf3, sizeof(buf3));
    bool ok2 = slotted_page_insert_tuple(page, buf3, len3, &s2);
    TEST_ASSERT(ok2 && s2 == 2, "Inserted slot 2");
    TEST_ASSERT(hdr->tuple_count == 3, "Page has 3 active tuples");

    /* Verify tuple reading */
    tuple_t read_t;
    uint8_t read_buf[PAGE_SIZE] __attribute__((aligned(8)));
    uint16_t rlen = 0;
    bool gok = slotted_page_get_tuple(page, s1, read_buf, &rlen);
    TEST_ASSERT(gok, "Get tuple slot 1 succeeded");
    bool dok = tuple_deserialize(&schema, read_buf, rlen, &read_t);
    TEST_ASSERT(dok, "Deserialization succeeded");
    TEST_ASSERT(read_t.values[0].int_val == 102, "Tuple id matched");
    TEST_ASSERT(strcmp(read_t.values[1].str_val, "Beta record with different length") == 0, "Tuple bio matched");

    /* Delete slot 1 (tombstone) and test compaction */
    bool del_ok = slotted_page_delete_tuple(page, s1);
    TEST_ASSERT(del_ok, "Deleted slot 1");
    TEST_ASSERT(hdr->tuple_count == 2, "Tuple count decreased to 2");

    uint16_t free_before = slotted_page_get_free_space(page);
    slotted_page_compact(page);
    uint16_t free_after = slotted_page_get_free_space(page);
    TEST_ASSERT(free_after > free_before, "Compaction reclaimed fragmented dead space");

    /* Verify active tuples 0 and 2 are still fully intact after compaction */
    gok = slotted_page_get_tuple(page, s0, read_buf, &rlen);
    TEST_ASSERT(gok, "Get tuple slot 0 after compaction succeeded");
    tuple_deserialize(&schema, read_buf, rlen, &read_t);
    TEST_ASSERT(read_t.values[0].int_val == 101, "Tuple 0 intact");

    gok = slotted_page_get_tuple(page, s2, read_buf, &rlen);
    TEST_ASSERT(gok, "Get tuple slot 2 after compaction succeeded");
    tuple_deserialize(&schema, read_buf, rlen, &read_t);
    TEST_ASSERT(read_t.values[0].int_val == 103, "Tuple 2 intact");
}

/* ========================================================================= */
/* TEST 3: Buffer Pool Manager & LRU Eviction                                */
/* ========================================================================= */
static void test_buffer_pool_manager(void) {
    const char* test_db = "test_bpm.dat";
    cleanup_file(test_db);

    disk_manager_t* dm = disk_manager_open(test_db);
    buffer_pool_manager_t* bpm = bpm_create(dm, 3); /* Small 3-frame buffer pool */
    TEST_ASSERT(bpm != NULL, "bpm_create succeeded");

    page_id_t p0, p1, p2, p3;
    frame_t* f0 = bpm_new_page(bpm, &p0);
    TEST_ASSERT(f0 != NULL && p0 == 0, "Allocated page 0 in buffer pool");
    f0->data[0] = 'A';
    bpm_unpin_page(bpm, p0, true);

    frame_t* f1 = bpm_new_page(bpm, &p1);
    TEST_ASSERT(f1 != NULL && p1 == 1, "Allocated page 1");
    f1->data[0] = 'B';
    bpm_unpin_page(bpm, p1, true);

    frame_t* f2 = bpm_new_page(bpm, &p2);
    TEST_ASSERT(f2 != NULL && p2 == 2, "Allocated page 2");
    f2->data[0] = 'C';
    bpm_unpin_page(bpm, p2, true);

    /* Buffer pool is now completely full (3 frames). Access p0 so it becomes MRU */
    frame_t* ref0 = bpm_fetch_page(bpm, p0);
    TEST_ASSERT(ref0 != NULL && ref0->data[0] == 'A', "Fetched p0 (hit)");
    bpm_unpin_page(bpm, p0, false);

    /* Allocate p3: Must evict LRU victim (which is p1, because p0 was accessed recently) */
    frame_t* f3 = bpm_new_page(bpm, &p3);
    TEST_ASSERT(f3 != NULL && p3 == 3, "Allocated p3 triggering LRU eviction of p1");
    f3->data[0] = 'D';
    bpm_unpin_page(bpm, p3, true);

    /* Verify p1 was evicted and written to disk */
    frame_t* ref1 = bpm_fetch_page(bpm, p1);
    TEST_ASSERT(ref1 != NULL && ref1->data[0] == 'B', "Fetched evicted p1 back from disk");
    bpm_unpin_page(bpm, p1, false);

    double hit_rate = bpm_get_hit_rate(bpm);
    TEST_ASSERT(hit_rate > 0.0, "Cache hit rate accounted correctly");

    bpm_destroy(bpm);
    disk_manager_close(dm);
    cleanup_file(test_db);
}

/* ========================================================================= */
/* TEST 4: B+ Tree Index with Duplicate / Non-Unique Keys (Fix 1 Check)      */
/* ========================================================================= */
static void test_bplus_tree_duplicates_and_splits(void) {
    bplus_tree_t* tree = bplus_tree_create();
    TEST_ASSERT(tree != NULL, "bplus_tree_create succeeded");

    /* Insert 25 records with duplicate keys (e.g. key 42 appears 5 times) */
    rid_t rids[25];
    for (int i = 0; i < 25; i++) {
        rids[i].page_id = (page_id_t)(i / 5);
        rids[i].slot_num = (uint16_t)(i % 5);
        int64_t key = (i < 10) ? 42 : (int64_t)(i * 10);
        bool ok = bplus_tree_insert(tree, key, rids[i]);
        TEST_ASSERT(ok, "bplus_tree_insert succeeded");
    }

    TEST_ASSERT(bplus_tree_get_size(tree) == 25, "Tree size is 25");
    TEST_ASSERT(tree->depth > 1, "Tree split and depth increased");

    /* Single search returns first matching */
    rid_t single_rid;
    bool found = bplus_tree_search(tree, 42, &single_rid);
    TEST_ASSERT(found, "bplus_tree_search found key 42");

    /* MULTI-ROW RETRIEVAL: bplus_tree_find_all must return ALL 10 matching RIDs */
    rid_t* all_rids = NULL;
    size_t count = bplus_tree_find_all(tree, 42, &all_rids);
    TEST_ASSERT(count == 10, "bplus_tree_find_all returned all 10 matching duplicate RIDs");
    TEST_ASSERT(all_rids != NULL, "all_rids buffer allocated");

    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT(all_rids[i].page_id == rids[i].page_id && all_rids[i].slot_num == rids[i].slot_num,
                    "Duplicate RID matched exactly");
    }
    free(all_rids);

    /* Range scan test */
    rid_t range_buf[16];
    size_t rcount = bplus_tree_range_scan(tree, 100, 200, range_buf, 16);
    TEST_ASSERT(rcount > 0, "Range scan found keys in range [100, 200]");

    /* Delete one duplicate and verify count becomes 9 */
    bool del_ok = bplus_tree_delete(tree, 42, rids[0]);
    TEST_ASSERT(del_ok, "Deleted one duplicate (42, rid[0])");

    count = bplus_tree_find_all(tree, 42, &all_rids);
    TEST_ASSERT(count == 9, "Remaining duplicate count is 9");
    free(all_rids);

    bplus_tree_destroy(tree);
}

/* ========================================================================= */
/* TEST 5: SQL Parser & AST Generation                                       */
/* ========================================================================= */
static void test_parser_and_expressions(void) {
    statement_t stmt;

    /* DDL Table */
    bool ok = parse_sql("CREATE TABLE products (sku INT, name VARCHAR(64), price INT);", &stmt);
    TEST_ASSERT(ok && stmt.type == STMT_CREATE_TABLE, "Parsed CREATE TABLE");
    TEST_ASSERT(strcmp(stmt.create_table.table_name, "products") == 0, "Table name is products");
    TEST_ASSERT(stmt.create_table.schema.num_columns == 3, "Schema has 3 columns");
    statement_free(&stmt);

    /* DDL Index */
    ok = parse_sql("CREATE INDEX ON products (sku);", &stmt);
    TEST_ASSERT(ok && stmt.type == STMT_CREATE_INDEX, "Parsed CREATE INDEX");
    statement_free(&stmt);

    /* Insert */
    ok = parse_sql("INSERT INTO products VALUES (901, 'Keyboard', 120);", &stmt);
    TEST_ASSERT(ok && stmt.type == STMT_INSERT, "Parsed INSERT");
    TEST_ASSERT(stmt.insert_stmt.num_values == 3, "Insert has 3 values");
    statement_free(&stmt);

    /* Select with Join, Where, Group By, Order By */
    ok = parse_sql("SELECT name, price FROM products WHERE price > 50 ORDER BY price DESC;", &stmt);
    TEST_ASSERT(ok && stmt.type == STMT_SELECT, "Parsed SELECT");
    TEST_ASSERT(stmt.select_stmt.has_order_by && stmt.select_stmt.is_desc, "Order by DESC detected");
    TEST_ASSERT(stmt.select_stmt.where_clause != NULL, "WHERE clause parsed");
    statement_free(&stmt);

    /* Expression evaluation on tuple */
    schema_t s;
    s.num_columns = 1;
    strcpy(s.columns[0].name, "val");
    s.columns[0].type = TYPE_INT;

    tuple_t t;
    t.num_values = 1;
    t.values[0] = value_make_int(100);

    expr_t* expr = expr_create_binary(OP_GT, expr_create_col_ref(NULL, "val"), expr_create_literal(value_make_int(50)));
    value_t res;
    bool eval_ok = expr_evaluate(expr, &s, &t, &res);
    TEST_ASSERT(eval_ok && res.int_val == 1, "Expression (val > 50) evaluated to TRUE");
    expr_free(expr);
}

/* ========================================================================= */
/* TEST 6: Rule-Based Query Optimizer & Access Path Routing                  */
/* ========================================================================= */
static void test_query_planner_optimizer(void) {
    const char* test_db = "test_opt.dat";
    cleanup_file(test_db);

    disk_manager_t* dm = disk_manager_open(test_db);
    buffer_pool_manager_t* bpm = bpm_create(dm, 10);
    catalog_t* catalog = catalog_create(bpm);

    schema_t schema;
    schema.num_columns = 3;
    strcpy(schema.columns[0].name, "id");
    schema.columns[0].type = TYPE_INT;
    schema.columns[0].max_len = 8;
    strcpy(schema.columns[1].name, "val");
    schema.columns[1].type = TYPE_INT;
    schema.columns[1].max_len = 8;
    strcpy(schema.columns[2].name, "dept");
    schema.columns[2].type = TYPE_VARCHAR;
    schema.columns[2].max_len = 32;

    catalog_create_table(catalog, "items", &schema);
    catalog_create_index(catalog, "items", "id");

    /* Plan 1: WHERE id = 100 on indexed column -> Must route to PLAN_INDEX_SCAN */
    statement_t stmt1;
    parse_sql("SELECT * FROM items WHERE id = 100;", &stmt1);
    plan_node_t* plan1 = plan_create(catalog, &stmt1);
    TEST_ASSERT(plan1 != NULL, "Plan 1 created");
    TEST_ASSERT(plan1->type == PLAN_INDEX_SCAN, "Optimizer routed indexed equality to PLAN_INDEX_SCAN");
    plan_destroy(plan1);
    statement_free(&stmt1);

    /* Plan 2: WHERE val > 50 on unindexed column -> Must route to PLAN_FILTER over PLAN_SEQ_SCAN */
    statement_t stmt2;
    parse_sql("SELECT * FROM items WHERE val > 50;", &stmt2);
    plan_node_t* plan2 = plan_create(catalog, &stmt2);
    TEST_ASSERT(plan2 != NULL, "Plan 2 created");
    TEST_ASSERT(plan2->type == PLAN_FILTER, "Optimizer wrapped unindexed predicate in PLAN_FILTER");
    TEST_ASSERT(plan2->left_child != NULL && plan2->left_child->type == PLAN_SEQ_SCAN, "Child is PLAN_SEQ_SCAN");
    plan_destroy(plan2);
    statement_free(&stmt2);

    catalog_destroy(catalog);
    bpm_destroy(bpm);
    disk_manager_close(dm);
    cleanup_file(test_db);
}

/* ========================================================================= */
/* TEST 7: End-to-End Volcano Pipeline & Non-Unique Index Scan (Fix 1)       */
/* ========================================================================= */
static void test_volcano_engine_duplicates(void) {
    const char* test_db = "test_volcano.dat";
    cleanup_file(test_db);

    disk_manager_t* dm = disk_manager_open(test_db);
    buffer_pool_manager_t* bpm = bpm_create(dm, 16);
    catalog_t* catalog = catalog_create(bpm);

    schema_t s;
    s.num_columns = 3;
    strcpy(s.columns[0].name, "id");
    s.columns[0].type = TYPE_INT;
    s.columns[0].max_len = 8;
    strcpy(s.columns[1].name, "code");
    s.columns[1].type = TYPE_INT; /* Non-unique indexed column */
    s.columns[1].max_len = 8;
    strcpy(s.columns[2].name, "name");
    s.columns[2].type = TYPE_VARCHAR;
    s.columns[2].max_len = 32;

    catalog_create_table(catalog, "records", &s);
    catalog_create_index(catalog, "records", "code");
    table_t* table = catalog_get_table(catalog, "records");

    /* Insert 6 rows: 4 rows share code = 777, 2 rows have code = 888 */
    for (int i = 1; i <= 6; i++) {
        tuple_t t;
        t.num_values = 3;
        t.values[0] = value_make_int(i);
        t.values[1] = value_make_int((i <= 4) ? 777 : 888);
        char name_buf[32];
        snprintf(name_buf, sizeof(name_buf), "item_%d", i);
        t.values[2] = value_make_str(name_buf);
        rid_t r;
        table_insert_tuple(table, bpm, &t, &r);
    }

    /* Query code = 777: MUST use IndexScan and return ALL 4 rows */
    statement_t stmt;
    parse_sql("SELECT * FROM records WHERE code = 777;", &stmt);
    plan_node_t* plan = plan_create(catalog, &stmt);
    TEST_ASSERT(plan != NULL && plan->type == PLAN_INDEX_SCAN, "Planned as IndexScan on code");

    executor_t* exec = executor_create(catalog, plan);
    exec->init(exec);

    tuple_t out;
    size_t rows_found = 0;
    while (exec->next(exec, &out)) {
        rows_found++;
        TEST_ASSERT(out.values[1].int_val == 777, "Matching row has code 777");
    }
    exec->close(exec);

    /* CRITICAL ASSERTION: All 4 duplicate records retrieved by IndexScan! */
    TEST_ASSERT(rows_found == 4, "IndexScan on non-unique column retrieved ALL 4 matching rows (Fix 1 Verified)");

    executor_destroy(exec);
    plan_destroy(plan);
    statement_free(&stmt);

    /* TEST MULTI-AGGREGATE QUERY WITH GROUP BY: SELECT code, COUNT(*), SUM(id) FROM records GROUP BY code; */
    statement_t agg_stmt;
    parse_sql("SELECT code, COUNT(*), SUM(id) FROM records GROUP BY code;", &agg_stmt);
    plan_node_t* agg_plan = plan_create(catalog, &agg_stmt);
    TEST_ASSERT(agg_plan != NULL && agg_plan->type == PLAN_AGGREGATE, "Planned as Aggregate with GROUP BY");
    TEST_ASSERT(agg_plan->output_schema.num_columns == 3, "Output schema has 3 columns (code, COUNT(*), SUM(id))");

    executor_t* agg_exec = executor_create(catalog, agg_plan);
    agg_exec->init(agg_exec);
    size_t groups_found = 0;
    while (agg_exec->next(agg_exec, &out)) {
        groups_found++;
        TEST_ASSERT(out.num_values == 3, "Output tuple has 3 values");
        if (out.values[0].int_val == 777) {
            TEST_ASSERT(out.values[1].int_val == 4, "Group 777 count is 4");
            TEST_ASSERT(out.values[2].int_val == 10, "Group 777 sum(id) is 10");
        } else if (out.values[0].int_val == 888) {
            TEST_ASSERT(out.values[1].int_val == 2, "Group 888 count is 2");
            TEST_ASSERT(out.values[2].int_val == 11, "Group 888 sum(id) is 11");
        }
    }
    TEST_ASSERT(groups_found == 2, "Found 2 distinct groups");
    agg_exec->close(agg_exec);
    executor_destroy(agg_exec);
    plan_destroy(agg_plan);
    statement_free(&agg_stmt);

    /* TEST GLOBAL MULTI-AGGREGATE QUERY: SELECT COUNT(*), SUM(id) FROM records; */
    statement_t glob_stmt;
    parse_sql("SELECT COUNT(*), SUM(id) FROM records;", &glob_stmt);
    plan_node_t* glob_plan = plan_create(catalog, &glob_stmt);
    TEST_ASSERT(glob_plan != NULL && glob_plan->output_schema.num_columns == 2, "Global agg has 2 columns");
    executor_t* glob_exec = executor_create(catalog, glob_plan);
    glob_exec->init(glob_exec);
    TEST_ASSERT(glob_exec->next(glob_exec, &out), "Global agg returned 1 row");
    TEST_ASSERT(out.num_values == 2, "Global agg has 2 values");
    TEST_ASSERT(out.values[0].int_val == 6, "Total count is 6");
    TEST_ASSERT(out.values[1].int_val == 21, "Total sum(id) is 21");
    glob_exec->close(glob_exec);
    executor_destroy(glob_exec);
    plan_destroy(glob_plan);
    statement_free(&glob_stmt);

    catalog_destroy(catalog);
    bpm_destroy(bpm);
    disk_manager_close(dm);
    cleanup_file(test_db);
}

int main(void) {
    printf("=======================================================================\n");
    printf("        Mini SQL Parsing & Execution Engine - Automated Test Suite     \n");
    printf("=======================================================================\n");

    double start_all = get_time_ms();

    RUN_TEST(test_disk_manager);
    RUN_TEST(test_slotted_page_and_compaction);
    RUN_TEST(test_buffer_pool_manager);
    RUN_TEST(test_bplus_tree_duplicates_and_splits);
    RUN_TEST(test_parser_and_expressions);
    RUN_TEST(test_query_planner_optimizer);
    RUN_TEST(test_volcano_engine_duplicates);

    double end_all = get_time_ms();

    printf("\n=======================================================================\n");
    printf("Test Results: %d/%d passed (%.2f%%)", passed_tests, total_tests,
           total_tests > 0 ? ((double)passed_tests / total_tests) * 100.0 : 0.0);
    if (failed_tests == 0) {
        printf(" - ALL TESTS PASSED!\n");
    } else {
        printf(" - %d FAILURES DETECTED!\n", failed_tests);
    }
    printf("Total Elapsed Time: %.2f ms\n", end_all - start_all);
    printf("=======================================================================\n");

    return (failed_tests == 0) ? 0 : 1;
}

