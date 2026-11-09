#ifndef MINI_SQL_PLANNER_H
#define MINI_SQL_PLANNER_H

#include "types.h"
#include "parser.h"
#include "catalog.h"
#include <stdbool.h>

typedef enum {
    PLAN_SEQ_SCAN,
    PLAN_INDEX_SCAN,
    PLAN_FILTER,
    PLAN_PROJECT,
    PLAN_NESTED_LOOP_JOIN,
    PLAN_AGGREGATE,
    PLAN_SORT,
    PLAN_INSERT,
    PLAN_UPDATE,
    PLAN_DELETE
} plan_type_t;

typedef struct plan_node_t plan_node_t;

struct plan_node_t {
    plan_type_t type;
    schema_t output_schema;
    plan_node_t* left_child;
    plan_node_t* right_child;

    union {
        struct {
            char table_name[MAX_TABLE_NAME];
        } seq_scan;

        struct {
            char table_name[MAX_TABLE_NAME];
            char index_col[MAX_COLUMN_NAME];
            int64_t target_key;
        } index_scan;

        struct {
            expr_t* predicate;
        } filter;

        struct {
            projection_item_t projections[MAX_COLUMNS];
            uint32_t num_projections;
        } project;

        struct {
            expr_t* join_cond;
        } join;

        struct {
            bool has_group_by;
            char group_col[MAX_COLUMN_NAME];
            proj_type_t agg_type;
            char agg_col[MAX_COLUMN_NAME];
            char alias[MAX_COLUMN_NAME];
        } aggregate;

        struct {
            char sort_col[MAX_COLUMN_NAME];
            bool is_desc;
        } sort;

        struct {
            char table_name[MAX_TABLE_NAME];
            value_t values[MAX_COLUMNS];
            uint32_t num_values;
        } insert_plan;

        struct {
            char table_name[MAX_TABLE_NAME];
            char set_col[MAX_COLUMN_NAME];
            value_t new_val;
        } update_plan;

        struct {
            char table_name[MAX_TABLE_NAME];
        } delete_plan;
    };
};

/* Planner API */
plan_node_t* plan_create(catalog_t* catalog, const statement_t* stmt);
void plan_destroy(plan_node_t* plan);
void plan_explain(const plan_node_t* plan, int indent);

#endif /* MINI_SQL_PLANNER_H */

