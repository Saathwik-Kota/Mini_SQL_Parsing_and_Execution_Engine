#ifndef MINI_SQL_PARSER_H
#define MINI_SQL_PARSER_H

#include "types.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EXPR_BINARY_OP,
    EXPR_COLUMN_REF,
    EXPR_LITERAL
} expr_type_t;

typedef enum {
    OP_EQ,
    OP_NEQ,
    OP_LT,
    OP_LTE,
    OP_GT,
    OP_GTE,
    OP_AND,
    OP_OR
} binary_op_t;

typedef struct expr_t expr_t;

struct expr_t {
    expr_type_t type;
    union {
        struct {
            binary_op_t op;
            expr_t* left;
            expr_t* right;
        } binary;

        struct {
            char table_name[MAX_TABLE_NAME];
            char column_name[MAX_COLUMN_NAME];
        } col_ref;

        value_t literal;
    };
};

typedef enum {
    PROJ_ALL,
    PROJ_COLUMN,
    PROJ_AGG_COUNT,
    PROJ_AGG_SUM
} proj_type_t;

typedef struct {
    proj_type_t type;
    char table_name[MAX_TABLE_NAME];
    char column_name[MAX_COLUMN_NAME];
    char alias[MAX_COLUMN_NAME];
} projection_item_t;

typedef struct {
    projection_item_t projections[MAX_COLUMNS];
    uint32_t num_projections;
    char from_table[MAX_TABLE_NAME];

    bool has_join;
    char join_table[MAX_TABLE_NAME];
    expr_t* join_condition;

    expr_t* where_clause;

    bool has_group_by;
    char group_by_col[MAX_COLUMN_NAME];

    bool has_order_by;
    char order_by_col[MAX_COLUMN_NAME];
    bool is_desc;
} select_stmt_t;

typedef struct {
    char table_name[MAX_TABLE_NAME];
    value_t values[MAX_COLUMNS];
    uint32_t num_values;
} insert_stmt_t;

typedef struct {
    char table_name[MAX_TABLE_NAME];
    char set_column[MAX_COLUMN_NAME];
    value_t new_value;
    expr_t* where_clause;
} update_stmt_t;

typedef struct {
    char table_name[MAX_TABLE_NAME];
    expr_t* where_clause;
} delete_stmt_t;

typedef struct {
    char table_name[MAX_TABLE_NAME];
    schema_t schema;
} create_table_stmt_t;

typedef struct {
    char table_name[MAX_TABLE_NAME];
    char column_name[MAX_COLUMN_NAME];
} create_index_stmt_t;

typedef enum {
    STMT_SELECT,
    STMT_INSERT,
    STMT_UPDATE,
    STMT_DELETE,
    STMT_CREATE_TABLE,
    STMT_CREATE_INDEX
} stmt_type_t;

typedef struct {
    stmt_type_t type;
    bool is_explain;
    union {
        select_stmt_t select_stmt;
        insert_stmt_t insert_stmt;
        update_stmt_t update_stmt;
        delete_stmt_t delete_stmt;
        create_table_stmt_t create_table;
        create_index_stmt_t create_index;
    };
} statement_t;

/* Parser API */
bool parse_sql(const char* sql, statement_t* out_stmt);
void statement_free(statement_t* stmt);
void expr_free(expr_t* expr);

/* Expression Helpers */
expr_t* expr_create_binary(binary_op_t op, expr_t* left, expr_t* right);
expr_t* expr_create_col_ref(const char* tbl, const char* col);
expr_t* expr_create_literal(value_t val);
bool expr_evaluate(const expr_t* expr, const schema_t* schema, const tuple_t* tuple, value_t* out_val);

#endif /* MINI_SQL_PARSER_H */

