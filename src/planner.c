#include "../include/planner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void safe_strcpy(char* dest, const char* src, size_t max_size) {
    if (!dest || max_size == 0) return;
    if (!src) {
        dest[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= max_size) len = max_size - 1;
    memcpy(dest, src, len);
    dest[len] = '\0';
}

static plan_node_t* plan_node_create(plan_type_t type) {
    plan_node_t* node = (plan_node_t*)calloc(1, sizeof(plan_node_t));
    if (!node) return NULL;
    node->type = type;
    node->left_child = NULL;
    node->right_child = NULL;
    return node;
}

void plan_destroy(plan_node_t* plan) {
    if (!plan) return;
    plan_destroy(plan->left_child);
    plan_destroy(plan->right_child);
    free(plan);
}

static bool check_index_match(const table_t* table, const expr_t* expr, int64_t* out_key) {
    if (!table || !table->has_index || !expr) return false;

    if (expr->type == EXPR_BINARY_OP && expr->binary.op == OP_EQ) {
        /* Check col = literal */
        if (expr->binary.left->type == EXPR_COLUMN_REF &&
            strcmp(expr->binary.left->col_ref.column_name, table->index_name) == 0 &&
            expr->binary.right->type == EXPR_LITERAL &&
            expr->binary.right->literal.type == TYPE_INT) {
            *out_key = expr->binary.right->literal.int_val;
            return true;
        }

        /* Check literal = col */
        if (expr->binary.right->type == EXPR_COLUMN_REF &&
            strcmp(expr->binary.right->col_ref.column_name, table->index_name) == 0 &&
            expr->binary.left->type == EXPR_LITERAL &&
            expr->binary.left->literal.type == TYPE_INT) {
            *out_key = expr->binary.left->literal.int_val;
            return true;
        }
    }

    if (expr->type == EXPR_BINARY_OP && expr->binary.op == OP_AND) {
        if (check_index_match(table, expr->binary.left, out_key)) return true;
        if (check_index_match(table, expr->binary.right, out_key)) return true;
    }

    return false;
}

static plan_node_t* plan_table_scan(catalog_t* catalog, const char* table_name, const expr_t* where_clause) {
    table_t* table = catalog_get_table(catalog, table_name);
    if (!table) return NULL;

    int64_t target_key = 0;
    bool use_index = check_index_match(table, where_clause, &target_key);

    plan_node_t* scan_node = NULL;
    if (use_index) {
        scan_node = plan_node_create(PLAN_INDEX_SCAN);
        safe_strcpy(scan_node->index_scan.table_name, table_name, MAX_TABLE_NAME);
        safe_strcpy(scan_node->index_scan.index_col, table->index_name, MAX_COLUMN_NAME);
        scan_node->index_scan.target_key = target_key;
        scan_node->output_schema = table->schema;
    } else {
        scan_node = plan_node_create(PLAN_SEQ_SCAN);
        safe_strcpy(scan_node->seq_scan.table_name, table_name, MAX_TABLE_NAME);
        scan_node->output_schema = table->schema;
    }

    return scan_node;
}

static plan_node_t* plan_select(catalog_t* catalog, const select_stmt_t* stmt) {
    plan_node_t* root = plan_table_scan(catalog, stmt->from_table, stmt->where_clause);
    if (!root) return NULL;

    /* Handle JOIN */
    if (stmt->has_join) {
        plan_node_t* right_scan = plan_table_scan(catalog, stmt->join_table, NULL);
        if (!right_scan) {
            plan_destroy(root);
            return NULL;
        }

        plan_node_t* join_node = plan_node_create(PLAN_NESTED_LOOP_JOIN);
        join_node->left_child = root;
        join_node->right_child = right_scan;
        join_node->join.join_cond = stmt->join_condition;

        /* Combined schema */
        schema_t combined;
        combined.num_columns = 0;
        for (uint32_t i = 0; i < root->output_schema.num_columns && combined.num_columns < MAX_COLUMNS; i++) {
            combined.columns[combined.num_columns++] = root->output_schema.columns[i];
        }
        for (uint32_t i = 0; i < right_scan->output_schema.num_columns && combined.num_columns < MAX_COLUMNS; i++) {
            combined.columns[combined.num_columns++] = right_scan->output_schema.columns[i];
        }
        join_node->output_schema = combined;
        root = join_node;
    }

    /* Handle WHERE Filter (if not an index scan, or for post-filtering) */
    if (stmt->where_clause) {
        table_t* table = catalog_get_table(catalog, stmt->from_table);
        int64_t k;
        bool pure_index_lookup = check_index_match(table, stmt->where_clause, &k) &&
                                 stmt->where_clause->type == EXPR_BINARY_OP &&
                                 stmt->where_clause->binary.op == OP_EQ;

        if (!pure_index_lookup) {
            plan_node_t* filter_node = plan_node_create(PLAN_FILTER);
            filter_node->left_child = root;
            filter_node->filter.predicate = stmt->where_clause;
            filter_node->output_schema = root->output_schema;
            root = filter_node;
        }
    }

    /* Check for Aggregation */
    bool has_agg = false;
    for (uint32_t i = 0; i < stmt->num_projections; i++) {
        if (stmt->projections[i].type == PROJ_AGG_COUNT || stmt->projections[i].type == PROJ_AGG_SUM) {
            has_agg = true;
            break;
        }
    }

    if (has_agg || stmt->has_group_by) {
        plan_node_t* agg_node = plan_node_create(PLAN_AGGREGATE);
        agg_node->left_child = root;
        agg_node->aggregate.has_group_by = stmt->has_group_by;
        if (stmt->has_group_by) {
            safe_strcpy(agg_node->aggregate.group_col, stmt->group_by_col, MAX_COLUMN_NAME);
        }
        agg_node->aggregate.num_projections = stmt->num_projections;
        for (uint32_t i = 0; i < stmt->num_projections; i++) {
            agg_node->aggregate.projections[i] = stmt->projections[i];
        }

        schema_t agg_schema;
        agg_schema.num_columns = stmt->num_projections;
        for (uint32_t i = 0; i < stmt->num_projections; i++) {
            column_t* c = &agg_schema.columns[i];
            const char* col_name = stmt->projections[i].column_name;
            const char* alias = stmt->projections[i].alias;

            if (alias[0] != '\0') {
                safe_strcpy(c->name, alias, MAX_COLUMN_NAME);
            } else if (stmt->projections[i].type == PROJ_AGG_COUNT) {
                snprintf(c->name, sizeof(c->name), "COUNT(%.*s)", (int)(sizeof(c->name) - 8), col_name[0] ? col_name : "*");
            } else if (stmt->projections[i].type == PROJ_AGG_SUM) {
                snprintf(c->name, sizeof(c->name), "SUM(%.*s)", (int)(sizeof(c->name) - 6), col_name);
            } else {
                safe_strcpy(c->name, col_name, MAX_COLUMN_NAME);
            }

            if (stmt->projections[i].type == PROJ_COLUMN) {
                int idx = schema_get_column_index(&root->output_schema, col_name);
                c->type = (idx >= 0) ? root->output_schema.columns[idx].type : TYPE_INT;
                c->max_len = (idx >= 0) ? root->output_schema.columns[idx].max_len : MAX_VARCHAR_LEN;
            } else {
                c->type = TYPE_INT;
                c->max_len = sizeof(int64_t);
            }
        }

        agg_node->output_schema = agg_schema;
        root = agg_node;
    }

    /* Handle ORDER BY */
    if (stmt->has_order_by) {
        plan_node_t* sort_node = plan_node_create(PLAN_SORT);
        sort_node->left_child = root;
        safe_strcpy(sort_node->sort.sort_col, stmt->order_by_col, MAX_COLUMN_NAME);
        sort_node->sort.is_desc = stmt->is_desc;
        sort_node->output_schema = root->output_schema;
        root = sort_node;
    }

    /* Handle Projection (if not already aggregated) */
    if (!has_agg && !stmt->has_group_by && !(stmt->num_projections == 1 && stmt->projections[0].type == PROJ_ALL)) {
        plan_node_t* proj_node = plan_node_create(PLAN_PROJECT);
        proj_node->left_child = root;
        proj_node->project.num_projections = stmt->num_projections;
        for (uint32_t i = 0; i < stmt->num_projections; i++) {
            proj_node->project.projections[i] = stmt->projections[i];
        }

        schema_t proj_schema;
        proj_schema.num_columns = stmt->num_projections;
        for (uint32_t i = 0; i < stmt->num_projections; i++) {
            const char* col_name = stmt->projections[i].column_name;
            safe_strcpy(proj_schema.columns[i].name, stmt->projections[i].alias[0] ? stmt->projections[i].alias : col_name, MAX_COLUMN_NAME);
            int idx = schema_get_column_index(&root->output_schema, col_name);
            if (idx >= 0) {
                proj_schema.columns[i].type = root->output_schema.columns[idx].type;
                proj_schema.columns[i].max_len = root->output_schema.columns[idx].max_len;
            } else {
                proj_schema.columns[i].type = TYPE_INT;
                proj_schema.columns[i].max_len = sizeof(int64_t);
            }
        }
        proj_node->output_schema = proj_schema;
        root = proj_node;
    }

    return root;
}

plan_node_t* plan_create(catalog_t* catalog, const statement_t* stmt) {
    if (!catalog || !stmt) return NULL;

    if (stmt->type == STMT_SELECT) {
        return plan_select(catalog, &stmt->select_stmt);
    }

    if (stmt->type == STMT_INSERT) {
        table_t* table = catalog_get_table(catalog, stmt->insert_stmt.table_name);
        if (!table) return NULL;

        plan_node_t* node = plan_node_create(PLAN_INSERT);
        safe_strcpy(node->insert_plan.table_name, stmt->insert_stmt.table_name, MAX_TABLE_NAME);
        node->insert_plan.num_values = stmt->insert_stmt.num_values;
        for (uint32_t i = 0; i < stmt->insert_stmt.num_values; i++) {
            node->insert_plan.values[i] = stmt->insert_stmt.values[i];
        }

        schema_t s;
        s.num_columns = 1;
        safe_strcpy(s.columns[0].name, "rows_affected", MAX_COLUMN_NAME);
        s.columns[0].type = TYPE_INT;
        s.columns[0].max_len = sizeof(int64_t);
        node->output_schema = s;
        return node;
    }

    if (stmt->type == STMT_UPDATE) {
        table_t* table = catalog_get_table(catalog, stmt->update_stmt.table_name);
        if (!table) return NULL;

        plan_node_t* scan = plan_table_scan(catalog, stmt->update_stmt.table_name, stmt->update_stmt.where_clause);
        if (!scan) return NULL;

        plan_node_t* filter = scan;
        if (stmt->update_stmt.where_clause && scan->type != PLAN_INDEX_SCAN) {
            filter = plan_node_create(PLAN_FILTER);
            filter->left_child = scan;
            filter->filter.predicate = stmt->update_stmt.where_clause;
            filter->output_schema = scan->output_schema;
        }

        plan_node_t* node = plan_node_create(PLAN_UPDATE);
        node->left_child = filter;
        safe_strcpy(node->update_plan.table_name, stmt->update_stmt.table_name, MAX_TABLE_NAME);
        safe_strcpy(node->update_plan.set_col, stmt->update_stmt.set_column, MAX_COLUMN_NAME);
        node->update_plan.new_val = stmt->update_stmt.new_value;

        schema_t s;
        s.num_columns = 1;
        safe_strcpy(s.columns[0].name, "rows_affected", MAX_COLUMN_NAME);
        s.columns[0].type = TYPE_INT;
        s.columns[0].max_len = sizeof(int64_t);
        node->output_schema = s;
        return node;
    }

    if (stmt->type == STMT_DELETE) {
        table_t* table = catalog_get_table(catalog, stmt->delete_stmt.table_name);
        if (!table) return NULL;

        plan_node_t* scan = plan_table_scan(catalog, stmt->delete_stmt.table_name, stmt->delete_stmt.where_clause);
        if (!scan) return NULL;

        plan_node_t* filter = scan;
        if (stmt->delete_stmt.where_clause && scan->type != PLAN_INDEX_SCAN) {
            filter = plan_node_create(PLAN_FILTER);
            filter->left_child = scan;
            filter->filter.predicate = stmt->delete_stmt.where_clause;
            filter->output_schema = scan->output_schema;
        }

        plan_node_t* node = plan_node_create(PLAN_DELETE);
        node->left_child = filter;
        safe_strcpy(node->delete_plan.table_name, stmt->delete_stmt.table_name, MAX_TABLE_NAME);

        schema_t s;
        s.num_columns = 1;
        safe_strcpy(s.columns[0].name, "rows_affected", MAX_COLUMN_NAME);
        s.columns[0].type = TYPE_INT;
        s.columns[0].max_len = sizeof(int64_t);
        node->output_schema = s;
        return node;
    }

    return NULL;
}

void plan_explain(const plan_node_t* plan, int indent) {
    if (!plan) return;

    for (int i = 0; i < indent; i++) printf("  ");

    switch (plan->type) {
        case PLAN_SEQ_SCAN:
            printf("-> SeqScan: %s (Access Path: Heap Table Scan, Cost: O(N))\n", plan->seq_scan.table_name);
            break;
        case PLAN_INDEX_SCAN:
            printf("-> IndexScan: %s on %s = %lld (Access Path: B+ Tree Index Lookup, Cost: O(log N))\n",
                   plan->index_scan.table_name, plan->index_scan.index_col, (long long)plan->index_scan.target_key);
            break;
        case PLAN_FILTER:
            printf("-> Filter: (Predicate Evaluation)\n");
            break;
        case PLAN_PROJECT:
            printf("-> Project: [");
            for (uint32_t i = 0; i < plan->project.num_projections; i++) {
                printf("%s%s", plan->project.projections[i].column_name, i + 1 < plan->project.num_projections ? ", " : "");
            }
            printf("]\n");
            break;
        case PLAN_NESTED_LOOP_JOIN:
            printf("-> NestedLoopJoin: (Join Condition)\n");
            break;
        case PLAN_AGGREGATE:
            printf("-> Aggregate: [");
            for (uint32_t i = 0; i < plan->aggregate.num_projections; i++) {
                if (i > 0) printf(", ");
                if (plan->aggregate.projections[i].type == PROJ_AGG_COUNT) {
                    printf("COUNT(%s)", plan->aggregate.projections[i].column_name[0] ? plan->aggregate.projections[i].column_name : "*");
                } else if (plan->aggregate.projections[i].type == PROJ_AGG_SUM) {
                    printf("SUM(%s)", plan->aggregate.projections[i].column_name);
                } else {
                    printf("%s", plan->aggregate.projections[i].column_name);
                }
            }
            if (plan->aggregate.has_group_by) {
                printf("] GROUP BY [%s]\n", plan->aggregate.group_col);
            } else {
                printf("]\n");
            }
            break;
        case PLAN_SORT:
            printf("-> Sort: [%s %s]\n", plan->sort.sort_col, plan->sort.is_desc ? "DESC" : "ASC");
            break;
        case PLAN_INSERT:
            printf("-> Insert: into %s\n", plan->insert_plan.table_name);
            break;
        case PLAN_UPDATE:
            printf("-> Update: %s set %s\n", plan->update_plan.table_name, plan->update_plan.set_col);
            break;
        case PLAN_DELETE:
            printf("-> Delete: from %s\n", plan->delete_plan.table_name);
            break;
    }

    if (plan->left_child) plan_explain(plan->left_child, indent + 1);
    if (plan->right_child) plan_explain(plan->right_child, indent + 1);
}
