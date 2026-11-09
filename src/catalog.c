#include "../include/catalog.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

catalog_t* catalog_create(buffer_pool_manager_t* bpm) {
    if (!bpm) return NULL;
    catalog_t* cat = (catalog_t*)calloc(1, sizeof(catalog_t));
    if (!cat) return NULL;
    cat->bpm = bpm;
    cat->num_tables = 0;
    return cat;
}

void catalog_destroy(catalog_t* catalog) {
    if (!catalog) return;
    for (uint32_t i = 0; i < catalog->num_tables; i++) {
        if (catalog->tables[i].index) {
            bplus_tree_destroy(catalog->tables[i].index);
            catalog->tables[i].index = NULL;
        }
    }
    free(catalog);
}

bool catalog_create_table(catalog_t* catalog, const char* name, const schema_t* schema) {
    if (!catalog || !name || !schema || catalog->num_tables >= MAX_TABLES) {
        return false;
    }

    if (catalog_get_table(catalog, name) != NULL) {
        return false; /* Table already exists */
    }

    table_t* table = &catalog->tables[catalog->num_tables++];
    memset(table, 0, sizeof(table_t));
    strncpy(table->name, name, sizeof(table->name) - 1);
    table->schema = *schema;
    table->first_page_id = INVALID_PAGE_ID;
    table->last_page_id = INVALID_PAGE_ID;
    table->has_index = false;
    table->index_column_idx = -1;
    table->index = NULL;
    table->num_tuples = 0;

    return true;
}

table_t* catalog_get_table(catalog_t* catalog, const char* name) {
    if (!catalog || !name) return NULL;
    for (uint32_t i = 0; i < catalog->num_tables; i++) {
        if (strcmp(catalog->tables[i].name, name) == 0) {
            return &catalog->tables[i];
        }
    }
    return NULL;
}

bool catalog_create_index(catalog_t* catalog, const char* table_name, const char* col_name) {
    if (!catalog || !table_name || !col_name) return false;

    table_t* table = catalog_get_table(catalog, table_name);
    if (!table) return false;

    int col_idx = schema_get_column_index(&table->schema, col_name);
    if (col_idx < 0) return false;

    if (table->index) {
        bplus_tree_destroy(table->index);
        table->index = NULL;
    }

    table->index = bplus_tree_create();
    if (!table->index) return false;

    table->has_index = true;
    table->index_column_idx = col_idx;
    strncpy(table->index_name, col_name, sizeof(table->index_name) - 1);

    /* Populate index from existing tuples in the table heap */
    if (table->first_page_id != INVALID_PAGE_ID) {
        page_id_t curr_pid = table->first_page_id;
        while (curr_pid != INVALID_PAGE_ID) {
            frame_t* frame = bpm_fetch_page(catalog->bpm, curr_pid);
            if (!frame) break;

            const slotted_page_header_t* header = slotted_page_get_header_const(frame->data);
            for (uint16_t s = 0; s < header->num_slots; s++) {
                uint8_t tuple_bytes[PAGE_SIZE];
                uint16_t tuple_len = 0;
                if (slotted_page_get_tuple(frame->data, s, tuple_bytes, &tuple_len)) {
                    tuple_t tuple;
                    if (tuple_deserialize(&table->schema, tuple_bytes, tuple_len, &tuple)) {
                        int64_t key = 0;
                        if (table->schema.columns[col_idx].type == TYPE_INT) {
                            key = tuple.values[col_idx].int_val;
                        } else {
                            /* Basic string hash for indexing varchar */
                            const char* s_val = tuple.values[col_idx].str_val;
                            uint64_t h = 5381;
                            for (size_t c = 0; s_val[c] != '\0'; c++) {
                                h = ((h << 5) + h) + (unsigned char)s_val[c];
                            }
                            key = (int64_t)h;
                        }
                        rid_t rid = { curr_pid, s };
                        bplus_tree_insert(table->index, key, rid);
                    }
                }
            }

            page_id_t next_pid = header->next_page_id;
            bpm_unpin_page(catalog->bpm, curr_pid, false);
            curr_pid = next_pid;
        }
    }

    return true;
}

bool table_insert_tuple(table_t* table, buffer_pool_manager_t* bpm, const tuple_t* tuple, rid_t* out_rid) {
    if (!table || !bpm || !tuple || !out_rid) {
        return false;
    }

    uint8_t buffer[PAGE_SIZE];
    uint16_t len = tuple_serialize(&table->schema, tuple, buffer, sizeof(buffer));
    if (len == 0) return false;

    page_id_t target_pid = table->last_page_id;
    frame_t* frame = NULL;

    if (target_pid == INVALID_PAGE_ID) {
        /* Allocate initial table page */
        frame = bpm_new_page(bpm, &target_pid);
        if (!frame) return false;
        slotted_page_init(frame->data, target_pid);
        table->first_page_id = target_pid;
        table->last_page_id = target_pid;
    } else {
        frame = bpm_fetch_page(bpm, target_pid);
        if (!frame) return false;

        if (!slotted_page_can_insert(frame->data, len)) {
            bpm_unpin_page(bpm, target_pid, false);

            /* Allocate new page and link into doubly linked list */
            page_id_t new_pid;
            frame_t* new_frame = bpm_new_page(bpm, &new_pid);
            if (!new_frame) return false;
            slotted_page_init(new_frame->data, new_pid);

            /* Update previous page next pointer */
            frame_t* prev_frame = bpm_fetch_page(bpm, target_pid);
            if (prev_frame) {
                slotted_page_header_t* prev_hdr = slotted_page_get_header(prev_frame->data);
                prev_hdr->next_page_id = new_pid;
                bpm_unpin_page(bpm, target_pid, true);
            }

            slotted_page_header_t* new_hdr = slotted_page_get_header(new_frame->data);
            new_hdr->prev_page_id = target_pid;

            target_pid = new_pid;
            table->last_page_id = new_pid;
            frame = new_frame;
        }
    }

    uint16_t slot_num;
    if (!slotted_page_insert_tuple(frame->data, buffer, len, &slot_num)) {
        bpm_unpin_page(bpm, target_pid, false);
        return false;
    }

    bpm_unpin_page(bpm, target_pid, true);

    out_rid->page_id = target_pid;
    out_rid->slot_num = slot_num;
    table->num_tuples++;

    /* Update index if present */
    if (table->has_index && table->index) {
        int64_t key = 0;
        int col_idx = table->index_column_idx;
        if (table->schema.columns[col_idx].type == TYPE_INT) {
            key = tuple->values[col_idx].int_val;
        } else {
            const char* s_val = tuple->values[col_idx].str_val;
            uint64_t h = 5381;
            for (size_t c = 0; s_val[c] != '\0'; c++) {
                h = ((h << 5) + h) + (unsigned char)s_val[c];
            }
            key = (int64_t)h;
        }
        bplus_tree_insert(table->index, key, *out_rid);
    }

    return true;
}

bool table_get_tuple(const table_t* table, buffer_pool_manager_t* bpm, rid_t rid, tuple_t* out_tuple) {
    if (!table || !bpm || !out_tuple || rid.page_id == INVALID_PAGE_ID) {
        return false;
    }

    frame_t* frame = bpm_fetch_page(bpm, rid.page_id);
    if (!frame) return false;

    uint8_t buffer[PAGE_SIZE];
    uint16_t len = 0;
    bool ok = slotted_page_get_tuple(frame->data, rid.slot_num, buffer, &len);
    bpm_unpin_page(bpm, rid.page_id, false);

    if (!ok) return false;

    out_tuple->rid = rid;
    return tuple_deserialize(&table->schema, buffer, len, out_tuple);
}

bool table_delete_tuple(table_t* table, buffer_pool_manager_t* bpm, rid_t rid) {
    if (!table || !bpm || rid.page_id == INVALID_PAGE_ID) {
        return false;
    }

    /* If index present, read tuple first to remove index entry */
    if (table->has_index && table->index) {
        tuple_t old_tuple;
        if (table_get_tuple(table, bpm, rid, &old_tuple)) {
            int64_t key = 0;
            int col_idx = table->index_column_idx;
            if (table->schema.columns[col_idx].type == TYPE_INT) {
                key = old_tuple.values[col_idx].int_val;
            } else {
                const char* s_val = old_tuple.values[col_idx].str_val;
                uint64_t h = 5381;
                for (size_t c = 0; s_val[c] != '\0'; c++) {
                    h = ((h << 5) + h) + (unsigned char)s_val[c];
                }
                key = (int64_t)h;
            }
            bplus_tree_delete(table->index, key, rid);
        }
    }

    frame_t* frame = bpm_fetch_page(bpm, rid.page_id);
    if (!frame) return false;

    bool ok = slotted_page_delete_tuple(frame->data, rid.slot_num);
    bpm_unpin_page(bpm, rid.page_id, ok);

    if (ok) {
        table->num_tuples--;
    }

    return ok;
}

bool table_update_tuple(table_t* table, buffer_pool_manager_t* bpm, rid_t rid, const tuple_t* new_tuple) {
    if (!table || !bpm || !new_tuple || rid.page_id == INVALID_PAGE_ID) {
        return false;
    }

    uint8_t buffer[PAGE_SIZE];
    uint16_t len = tuple_serialize(&table->schema, new_tuple, buffer, sizeof(buffer));
    if (len == 0) return false;

    /* If index present, check if indexed key changed */
    if (table->has_index && table->index) {
        tuple_t old_tuple;
        if (table_get_tuple(table, bpm, rid, &old_tuple)) {
            int col_idx = table->index_column_idx;
            int64_t old_k = (table->schema.columns[col_idx].type == TYPE_INT) ?
                            old_tuple.values[col_idx].int_val : (int64_t)strlen(old_tuple.values[col_idx].str_val);
            int64_t new_k = (table->schema.columns[col_idx].type == TYPE_INT) ?
                            new_tuple->values[col_idx].int_val : (int64_t)strlen(new_tuple->values[col_idx].str_val);

            if (old_k != new_k) {
                bplus_tree_delete(table->index, old_k, rid);
                bplus_tree_insert(table->index, new_k, rid);
            }
        }
    }

    frame_t* frame = bpm_fetch_page(bpm, rid.page_id);
    if (!frame) return false;

    bool ok = slotted_page_update_tuple(frame->data, rid.slot_num, buffer, len);
    bpm_unpin_page(bpm, rid.page_id, ok);

    if (!ok) {
        /* If in-place update failed due to lack of page space, re-insert */
        table_delete_tuple(table, bpm, rid);
        rid_t new_rid;
        return table_insert_tuple(table, bpm, new_tuple, &new_rid);
    }

    return true;
}

