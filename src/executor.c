#include "../include/executor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==================== SEQ SCAN EXECUTOR ==================== */
typedef struct {
    catalog_t* catalog;
    table_t* table;
    page_id_t curr_page_id;
    uint16_t curr_slot;
    frame_t* curr_frame;
} seq_scan_state_t;

static void seq_scan_init(executor_t* self) {
    seq_scan_state_t* st = (seq_scan_state_t*)self->state;
    if (st->curr_frame) {
        bpm_unpin_page(st->catalog->bpm, st->curr_page_id, false);
        st->curr_frame = NULL;
    }
    st->curr_page_id = st->table->first_page_id;
    st->curr_slot = 0;
}

static bool seq_scan_next(executor_t* self, tuple_t* out_tuple) {
    seq_scan_state_t* st = (seq_scan_state_t*)self->state;

    while (st->curr_page_id != INVALID_PAGE_ID) {
        if (!st->curr_frame) {
            st->curr_frame = bpm_fetch_page(st->catalog->bpm, st->curr_page_id);
            if (!st->curr_frame) return false;
        }

        const slotted_page_header_t* hdr = slotted_page_get_header_const(st->curr_frame->data);
        while (st->curr_slot < hdr->num_slots) {
            uint16_t slot = st->curr_slot++;
            uint8_t buf[PAGE_SIZE];
            uint16_t len = 0;
            if (slotted_page_get_tuple(st->curr_frame->data, slot, buf, &len)) {
                if (tuple_deserialize(&st->table->schema, buf, len, out_tuple)) {
                    out_tuple->rid.page_id = st->curr_page_id;
                    out_tuple->rid.slot_num = slot;
                    return true;
                }
            }
        }

        /* End of page, advance to next page in table heap */
        page_id_t next_pid = hdr->next_page_id;
        bpm_unpin_page(st->catalog->bpm, st->curr_page_id, false);
        st->curr_frame = NULL;
        st->curr_page_id = next_pid;
        st->curr_slot = 0;
    }

    return false;
}

static void seq_scan_close(executor_t* self) {
    seq_scan_state_t* st = (seq_scan_state_t*)self->state;
    if (st->curr_frame) {
        bpm_unpin_page(st->catalog->bpm, st->curr_page_id, false);
        st->curr_frame = NULL;
    }
}

/* ==================== INDEX SCAN EXECUTOR ==================== */
typedef struct {
    catalog_t* catalog;
    table_t* table;
    int64_t target_key;
    rid_t* rids;
    size_t num_rids;
    size_t cursor;
} index_scan_state_t;

static void index_scan_init(executor_t* self) {
    index_scan_state_t* st = (index_scan_state_t*)self->state;
    free(st->rids);
    st->rids = NULL;
    st->num_rids = 0;
    st->cursor = 0;

    if (st->table->has_index && st->table->index) {
        rid_t r;
        if (bplus_tree_search(st->table->index, st->target_key, &r)) {
            st->rids = (rid_t*)malloc(sizeof(rid_t));
            st->rids[0] = r;
            st->num_rids = 1;
        }
    }
}

static bool index_scan_next(executor_t* self, tuple_t* out_tuple) {
    index_scan_state_t* st = (index_scan_state_t*)self->state;
    while (st->cursor < st->num_rids) {
        rid_t r = st->rids[st->cursor++];
        if (table_get_tuple(st->table, st->catalog->bpm, r, out_tuple)) {
            return true;
        }
    }
    return false;
}

static void index_scan_close(executor_t* self) {
    index_scan_state_t* st = (index_scan_state_t*)self->state;
    free(st->rids);
    st->rids = NULL;
    st->num_rids = 0;
    st->cursor = 0;
}

/* ==================== FILTER EXECUTOR ==================== */
typedef struct {
    executor_t* child;
    expr_t* predicate;
} filter_state_t;

static void filter_init(executor_t* self) {
    filter_state_t* st = (filter_state_t*)self->state;
    st->child->init(st->child);
}

static bool filter_next(executor_t* self, tuple_t* out_tuple) {
    filter_state_t* st = (filter_state_t*)self->state;
    while (st->child->next(st->child, out_tuple)) {
        value_t val;
        if (expr_evaluate(st->predicate, &st->child->schema, out_tuple, &val)) {
            if (!val.is_null && val.int_val != 0) {
                return true;
            }
        }
    }
    return false;
}

static void filter_close(executor_t* self) {
    filter_state_t* st = (filter_state_t*)self->state;
    st->child->close(st->child);
}

/* ==================== PROJECT EXECUTOR ==================== */
typedef struct {
    executor_t* child;
    projection_item_t projections[MAX_COLUMNS];
    uint32_t num_projections;
} project_state_t;

static void project_init(executor_t* self) {
    project_state_t* st = (project_state_t*)self->state;
    st->child->init(st->child);
}

static bool project_next(executor_t* self, tuple_t* out_tuple) {
    project_state_t* st = (project_state_t*)self->state;
    tuple_t in_tuple;
    if (!st->child->next(st->child, &in_tuple)) {
        return false;
    }

    out_tuple->rid = in_tuple.rid;
    out_tuple->num_values = st->num_projections;

    for (uint32_t i = 0; i < st->num_projections; i++) {
        if (st->projections[i].type == PROJ_ALL) {
            *out_tuple = in_tuple;
            return true;
        }

        int idx = schema_get_column_index(&st->child->schema, st->projections[i].column_name);
        if (idx >= 0 && (uint32_t)idx < in_tuple.num_values) {
            out_tuple->values[i] = in_tuple.values[idx];
        } else {
            out_tuple->values[i] = value_make_null(TYPE_INT);
        }
    }

    return true;
}

static void project_close(executor_t* self) {
    project_state_t* st = (project_state_t*)self->state;
    st->child->close(st->child);
}

/* ==================== NESTED LOOP JOIN EXECUTOR ==================== */
typedef struct {
    executor_t* left;
    executor_t* right;
    expr_t* join_cond;
    tuple_t curr_left;
    bool has_left;
} join_state_t;

static void join_init(executor_t* self) {
    join_state_t* st = (join_state_t*)self->state;
    st->left->init(st->left);
    st->right->init(st->right);
    st->has_left = st->left->next(st->left, &st->curr_left);
}

static bool join_next(executor_t* self, tuple_t* out_tuple) {
    join_state_t* st = (join_state_t*)self->state;

    while (st->has_left) {
        tuple_t right_tuple;
        while (st->right->next(st->right, &right_tuple)) {
            tuple_t candidate;
            candidate.num_values = 0;
            candidate.rid = st->curr_left.rid;

            for (uint32_t i = 0; i < st->curr_left.num_values && candidate.num_values < MAX_COLUMNS; i++) {
                candidate.values[candidate.num_values++] = st->curr_left.values[i];
            }
            for (uint32_t i = 0; i < right_tuple.num_values && candidate.num_values < MAX_COLUMNS; i++) {
                candidate.values[candidate.num_values++] = right_tuple.values[i];
            }

            if (!st->join_cond) {
                *out_tuple = candidate;
                return true;
            }

            value_t match_val;
            if (expr_evaluate(st->join_cond, &self->schema, &candidate, &match_val)) {
                if (!match_val.is_null && match_val.int_val != 0) {
                    *out_tuple = candidate;
                    return true;
                }
            }
        }

        /* Reset right child and advance left child */
        st->right->init(st->right);
        st->has_left = st->left->next(st->left, &st->curr_left);
    }

    return false;
}

static void join_close(executor_t* self) {
    join_state_t* st = (join_state_t*)self->state;
    st->left->close(st->left);
    st->right->close(st->right);
}

/* ==================== SORT EXECUTOR ==================== */
typedef struct {
    executor_t* child;
    int sort_col_idx;
    bool is_desc;
    tuple_t* buffer;
    size_t count;
    size_t capacity;
    size_t cursor;
} sort_state_t;

static int sort_comparator_col_idx = 0;
static bool sort_comparator_is_desc = false;

static int sort_tuple_cmp(const void* a, const void* b) {
    const tuple_t* t1 = (const tuple_t*)a;
    const tuple_t* t2 = (const tuple_t*)b;
    int cmp = value_compare(&t1->values[sort_comparator_col_idx], &t2->values[sort_comparator_col_idx]);
    return sort_comparator_is_desc ? -cmp : cmp;
}

static void sort_init(executor_t* self) {
    sort_state_t* st = (sort_state_t*)self->state;
    free(st->buffer);
    st->buffer = NULL;
    st->count = 0;
    st->capacity = 64;
    st->cursor = 0;
    st->buffer = (tuple_t*)malloc(st->capacity * sizeof(tuple_t));

    st->child->init(st->child);
    tuple_t t;
    while (st->child->next(st->child, &t)) {
        if (st->count >= st->capacity) {
            st->capacity *= 2;
            st->buffer = (tuple_t*)realloc(st->buffer, st->capacity * sizeof(tuple_t));
        }
        st->buffer[st->count++] = t;
    }
    st->child->close(st->child);

    if (st->count > 1) {
        sort_comparator_col_idx = st->sort_col_idx;
        sort_comparator_is_desc = st->is_desc;
        qsort(st->buffer, st->count, sizeof(tuple_t), sort_tuple_cmp);
    }
}

static bool sort_next(executor_t* self, tuple_t* out_tuple) {
    sort_state_t* st = (sort_state_t*)self->state;
    if (st->cursor < st->count) {
        *out_tuple = st->buffer[st->cursor++];
        return true;
    }
    return false;
}

static void sort_close(executor_t* self) {
    sort_state_t* st = (sort_state_t*)self->state;
    free(st->buffer);
    st->buffer = NULL;
    st->count = 0;
    st->capacity = 0;
    st->cursor = 0;
}

/* ==================== AGGREGATE EXECUTOR ==================== */
typedef struct {
    value_t key;
    int64_t count;
    int64_t sum;
} agg_group_t;

typedef struct {
    executor_t* child;
    bool has_group_by;
    int group_col_idx;
    proj_type_t agg_type;
    int agg_col_idx;
    agg_group_t groups[512];
    size_t num_groups;
    size_t cursor;
} aggregate_state_t;

static void aggregate_init(executor_t* self) {
    aggregate_state_t* st = (aggregate_state_t*)self->state;
    st->num_groups = 0;
    st->cursor = 0;

    if (!st->has_group_by) {
        st->num_groups = 1;
        st->groups[0].count = 0;
        st->groups[0].sum = 0;
        st->groups[0].key = value_make_null(TYPE_INT);
    }

    st->child->init(st->child);
    tuple_t t;
    while (st->child->next(st->child, &t)) {
        size_t group_idx = 0;
        if (st->has_group_by) {
            value_t key = t.values[st->group_col_idx];
            bool found = false;
            for (size_t i = 0; i < st->num_groups; i++) {
                if (value_compare(&st->groups[i].key, &key) == 0) {
                    group_idx = i;
                    found = true;
                    break;
                }
            }
            if (!found && st->num_groups < 512) {
                group_idx = st->num_groups++;
                st->groups[group_idx].key = key;
                st->groups[group_idx].count = 0;
                st->groups[group_idx].sum = 0;
            }
        }

        st->groups[group_idx].count++;
        if (st->agg_type == PROJ_AGG_SUM && st->agg_col_idx >= 0) {
            st->groups[group_idx].sum += t.values[st->agg_col_idx].int_val;
        }
    }
    st->child->close(st->child);
}

static bool aggregate_next(executor_t* self, tuple_t* out_tuple) {
    aggregate_state_t* st = (aggregate_state_t*)self->state;
    if (st->cursor < st->num_groups) {
        size_t idx = st->cursor++;
        int64_t agg_val = (st->agg_type == PROJ_AGG_SUM) ? st->groups[idx].sum : st->groups[idx].count;

        if (st->has_group_by) {
            out_tuple->num_values = 2;
            out_tuple->values[0] = st->groups[idx].key;
            out_tuple->values[1] = value_make_int(agg_val);
        } else {
            out_tuple->num_values = 1;
            out_tuple->values[0] = value_make_int(agg_val);
        }
        return true;
    }
    return false;
}

static void aggregate_close(executor_t* self) {
    aggregate_state_t* st = (aggregate_state_t*)self->state;
    st->num_groups = 0;
    st->cursor = 0;
}

/* ==================== INSERT EXECUTOR ==================== */
typedef struct {
    catalog_t* catalog;
    table_t* table;
    tuple_t tuple;
    bool done;
} insert_state_t;

static void insert_init(executor_t* self) {
    insert_state_t* st = (insert_state_t*)self->state;
    st->done = false;
}

static bool insert_next(executor_t* self, tuple_t* out_tuple) {
    insert_state_t* st = (insert_state_t*)self->state;
    if (st->done) return false;

    rid_t rid;
    bool ok = table_insert_tuple(st->table, st->catalog->bpm, &st->tuple, &rid);

    out_tuple->num_values = 1;
    out_tuple->values[0] = value_make_int(ok ? 1 : 0);
    st->done = true;
    return true;
}

static void insert_close(executor_t* self) {
    insert_state_t* st = (insert_state_t*)self->state;
    st->done = true;
}

/* ==================== UPDATE EXECUTOR ==================== */
typedef struct {
    catalog_t* catalog;
    table_t* table;
    executor_t* child;
    int set_col_idx;
    value_t new_val;
    bool done;
} update_state_t;

static void update_init(executor_t* self) {
    update_state_t* st = (update_state_t*)self->state;
    st->child->init(st->child);
    st->done = false;
}

static bool update_next(executor_t* self, tuple_t* out_tuple) {
    update_state_t* st = (update_state_t*)self->state;
    if (st->done) return false;

    int64_t updated_count = 0;
    tuple_t t;

    size_t capacity = 64;
    size_t count = 0;
    rid_t* rids_to_update = (rid_t*)malloc(capacity * sizeof(rid_t));
    tuple_t* new_tuples = (tuple_t*)malloc(capacity * sizeof(tuple_t));

    while (st->child->next(st->child, &t)) {
        if (count >= capacity) {
            capacity *= 2;
            rids_to_update = (rid_t*)realloc(rids_to_update, capacity * sizeof(rid_t));
            new_tuples = (tuple_t*)realloc(new_tuples, capacity * sizeof(tuple_t));
        }
        rids_to_update[count] = t.rid;
        t.values[st->set_col_idx] = st->new_val;
        new_tuples[count] = t;
        count++;
    }
    st->child->close(st->child);

    for (size_t i = 0; i < count; i++) {
        if (table_update_tuple(st->table, st->catalog->bpm, rids_to_update[i], &new_tuples[i])) {
            updated_count++;
        }
    }

    free(rids_to_update);
    free(new_tuples);

    out_tuple->num_values = 1;
    out_tuple->values[0] = value_make_int(updated_count);
    st->done = true;
    return true;
}

static void update_close(executor_t* self) {
    update_state_t* st = (update_state_t*)self->state;
    st->child->close(st->child);
    st->done = true;
}

/* ==================== DELETE EXECUTOR ==================== */
typedef struct {
    catalog_t* catalog;
    table_t* table;
    executor_t* child;
    bool done;
} delete_state_t;

static void delete_init(executor_t* self) {
    delete_state_t* st = (delete_state_t*)self->state;
    st->child->init(st->child);
    st->done = false;
}

static bool delete_next(executor_t* self, tuple_t* out_tuple) {
    delete_state_t* st = (delete_state_t*)self->state;
    if (st->done) return false;

    int64_t deleted_count = 0;
    tuple_t t;

    size_t capacity = 64;
    size_t count = 0;
    rid_t* rids_to_delete = (rid_t*)malloc(capacity * sizeof(rid_t));

    while (st->child->next(st->child, &t)) {
        if (count >= capacity) {
            capacity *= 2;
            rids_to_delete = (rid_t*)realloc(rids_to_delete, capacity * sizeof(rid_t));
        }
        rids_to_delete[count++] = t.rid;
    }
    st->child->close(st->child);

    for (size_t i = 0; i < count; i++) {
        if (table_delete_tuple(st->table, st->catalog->bpm, rids_to_delete[i])) {
            deleted_count++;
        }
    }

    free(rids_to_delete);

    out_tuple->num_values = 1;
    out_tuple->values[0] = value_make_int(deleted_count);
    st->done = true;
    return true;
}

static void delete_close(executor_t* self) {
    delete_state_t* st = (delete_state_t*)self->state;
    st->child->close(st->child);
    st->done = true;
}

/* ==================== EXECUTOR CREATION FACTORY ==================== */
executor_t* executor_create(catalog_t* catalog, const plan_node_t* plan) {
    if (!catalog || !plan) return NULL;

    executor_t* exec = (executor_t*)calloc(1, sizeof(executor_t));
    if (!exec) return NULL;
    exec->schema = plan->output_schema;

    switch (plan->type) {
        case PLAN_SEQ_SCAN: {
            seq_scan_state_t* st = (seq_scan_state_t*)calloc(1, sizeof(seq_scan_state_t));
            st->catalog = catalog;
            st->table = catalog_get_table(catalog, plan->seq_scan.table_name);
            st->curr_page_id = INVALID_PAGE_ID;
            st->curr_slot = 0;
            st->curr_frame = NULL;
            exec->state = st;
            exec->init = seq_scan_init;
            exec->next = seq_scan_next;
            exec->close = seq_scan_close;
            break;
        }

        case PLAN_INDEX_SCAN: {
            index_scan_state_t* st = (index_scan_state_t*)calloc(1, sizeof(index_scan_state_t));
            st->catalog = catalog;
            st->table = catalog_get_table(catalog, plan->index_scan.table_name);
            st->target_key = plan->index_scan.target_key;
            st->rids = NULL;
            st->num_rids = 0;
            st->cursor = 0;
            exec->state = st;
            exec->init = index_scan_init;
            exec->next = index_scan_next;
            exec->close = index_scan_close;
            break;
        }

        case PLAN_FILTER: {
            filter_state_t* st = (filter_state_t*)calloc(1, sizeof(filter_state_t));
            st->child = executor_create(catalog, plan->left_child);
            st->predicate = plan->filter.predicate;
            exec->state = st;
            exec->init = filter_init;
            exec->next = filter_next;
            exec->close = filter_close;
            break;
        }

        case PLAN_PROJECT: {
            project_state_t* st = (project_state_t*)calloc(1, sizeof(project_state_t));
            st->child = executor_create(catalog, plan->left_child);
            st->num_projections = plan->project.num_projections;
            memcpy(st->projections, plan->project.projections, sizeof(st->projections));
            exec->state = st;
            exec->init = project_init;
            exec->next = project_next;
            exec->close = project_close;
            break;
        }

        case PLAN_NESTED_LOOP_JOIN: {
            join_state_t* st = (join_state_t*)calloc(1, sizeof(join_state_t));
            st->left = executor_create(catalog, plan->left_child);
            st->right = executor_create(catalog, plan->right_child);
            st->join_cond = plan->join.join_cond;
            exec->state = st;
            exec->init = join_init;
            exec->next = join_next;
            exec->close = join_close;
            break;
        }

        case PLAN_SORT: {
            sort_state_t* st = (sort_state_t*)calloc(1, sizeof(sort_state_t));
            st->child = executor_create(catalog, plan->left_child);
            st->sort_col_idx = schema_get_column_index(&st->child->schema, plan->sort.sort_col);
            if (st->sort_col_idx < 0) st->sort_col_idx = 0;
            st->is_desc = plan->sort.is_desc;
            exec->state = st;
            exec->init = sort_init;
            exec->next = sort_next;
            exec->close = sort_close;
            break;
        }

        case PLAN_AGGREGATE: {
            aggregate_state_t* st = (aggregate_state_t*)calloc(1, sizeof(aggregate_state_t));
            st->child = executor_create(catalog, plan->left_child);
            st->has_group_by = plan->aggregate.has_group_by;
            st->group_col_idx = plan->aggregate.has_group_by ?
                                schema_get_column_index(&st->child->schema, plan->aggregate.group_col) : -1;
            st->agg_type = plan->aggregate.agg_type;
            st->agg_col_idx = schema_get_column_index(&st->child->schema, plan->aggregate.agg_col);
            exec->state = st;
            exec->init = aggregate_init;
            exec->next = aggregate_next;
            exec->close = aggregate_close;
            break;
        }

        case PLAN_INSERT: {
            insert_state_t* st = (insert_state_t*)calloc(1, sizeof(insert_state_t));
            st->catalog = catalog;
            st->table = catalog_get_table(catalog, plan->insert_plan.table_name);
            st->tuple.num_values = plan->insert_plan.num_values;
            memcpy(st->tuple.values, plan->insert_plan.values, sizeof(plan->insert_plan.values));
            exec->state = st;
            exec->init = insert_init;
            exec->next = insert_next;
            exec->close = insert_close;
            break;
        }

        case PLAN_UPDATE: {
            update_state_t* st = (update_state_t*)calloc(1, sizeof(update_state_t));
            st->catalog = catalog;
            st->table = catalog_get_table(catalog, plan->update_plan.table_name);
            st->child = executor_create(catalog, plan->left_child);
            st->set_col_idx = schema_get_column_index(&st->table->schema, plan->update_plan.set_col);
            st->new_val = plan->update_plan.new_val;
            exec->state = st;
            exec->init = update_init;
            exec->next = update_next;
            exec->close = update_close;
            break;
        }

        case PLAN_DELETE: {
            delete_state_t* st = (delete_state_t*)calloc(1, sizeof(delete_state_t));
            st->catalog = catalog;
            st->table = catalog_get_table(catalog, plan->delete_plan.table_name);
            st->child = executor_create(catalog, plan->left_child);
            exec->state = st;
            exec->init = delete_init;
            exec->next = delete_next;
            exec->close = delete_close;
            break;
        }
    }

    return exec;
}

void executor_destroy(executor_t* exec) {
    if (!exec) return;

    if (exec->close) {
        exec->close(exec);
    }

    free(exec->state);
    free(exec);
}
