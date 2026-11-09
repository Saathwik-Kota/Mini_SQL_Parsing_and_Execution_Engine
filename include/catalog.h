#ifndef MINI_SQL_CATALOG_H
#define MINI_SQL_CATALOG_H

#include "types.h"
#include "buffer_pool.h"
#include "slotted_page.h"
#include "bplus_tree.h"
#include <stdint.h>
#include <stdbool.h>

#define MAX_TABLES 16

typedef struct {
    char name[MAX_TABLE_NAME];
    schema_t schema;
    page_id_t first_page_id;
    page_id_t last_page_id;
    bool has_index;
    int index_column_idx;
    char index_name[MAX_COLUMN_NAME];
    bplus_tree_t* index;
    uint64_t num_tuples;
} table_t;

typedef struct {
    buffer_pool_manager_t* bpm;
    table_t tables[MAX_TABLES];
    uint32_t num_tables;
} catalog_t;

catalog_t* catalog_create(buffer_pool_manager_t* bpm);
void catalog_destroy(catalog_t* catalog);

bool catalog_create_table(catalog_t* catalog, const char* name, const schema_t* schema);
table_t* catalog_get_table(catalog_t* catalog, const char* name);
bool catalog_create_index(catalog_t* catalog, const char* table_name, const char* col_name);

/* Table Heap Operations */
bool table_insert_tuple(table_t* table, buffer_pool_manager_t* bpm, const tuple_t* tuple, rid_t* out_rid);
bool table_get_tuple(const table_t* table, buffer_pool_manager_t* bpm, rid_t rid, tuple_t* out_tuple);
bool table_update_tuple(table_t* table, buffer_pool_manager_t* bpm, rid_t rid, const tuple_t* new_tuple);
bool table_delete_tuple(table_t* table, buffer_pool_manager_t* bpm, rid_t rid);

#endif /* MINI_SQL_CATALOG_H */

