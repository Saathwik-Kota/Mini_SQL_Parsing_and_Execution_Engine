#include "../include/parser.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    TOK_EOF,
    TOK_IDENT,
    TOK_INT,
    TOK_STRING,
    TOK_COMMA,
    TOK_SEMICOLON,
    TOK_STAR,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_DOT,
    TOK_EQ,
    TOK_NEQ,
    TOK_LT,
    TOK_LTE,
    TOK_GT,
    TOK_GTE,

    /* Keywords */
    KW_SELECT,
    KW_FROM,
    KW_WHERE,
    KW_INSERT,
    KW_INTO,
    KW_VALUES,
    KW_UPDATE,
    KW_SET,
    KW_DELETE,
    KW_JOIN,
    KW_ON,
    KW_GROUP,
    KW_BY,
    KW_ORDER,
    KW_ASC,
    KW_DESC,
    KW_CREATE,
    KW_TABLE,
    KW_INDEX,
    KW_EXPLAIN,
    KW_AND,
    KW_OR,
    KW_INT,
    KW_BIGINT,
    KW_VARCHAR,
    KW_TEXT,
    KW_COUNT,
    KW_SUM,
    KW_AS
} token_type_t;

typedef struct {
    token_type_t type;
    char text[MAX_VARCHAR_LEN];
    int64_t int_val;
} token_t;

typedef struct {
    const char* src;
    size_t pos;
    size_t len;
    token_t current;
} lexer_t;

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

static bool case_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void lexer_next(lexer_t* lex);

static void lexer_init(lexer_t* lex, const char* sql) {
    lex->src = sql;
    lex->pos = 0;
    lex->len = strlen(sql);
    lexer_next(lex);
}

static void skip_whitespace(lexer_t* lex) {
    while (lex->pos < lex->len && isspace((unsigned char)lex->src[lex->pos])) {
        lex->pos++;
    }
}

static void lexer_next(lexer_t* lex) {
    skip_whitespace(lex);

    if (lex->pos >= lex->len) {
        lex->current.type = TOK_EOF;
        lex->current.text[0] = '\0';
        return;
    }

    char c = lex->src[lex->pos];

    if (c == ';') {
        lex->current.type = TOK_SEMICOLON;
        lex->current.text[0] = ';';
        lex->current.text[1] = '\0';
        lex->pos++;
        return;
    }
    if (c == ',') {
        lex->current.type = TOK_COMMA;
        lex->current.text[0] = ',';
        lex->current.text[1] = '\0';
        lex->pos++;
        return;
    }
    if (c == '*') {
        lex->current.type = TOK_STAR;
        lex->current.text[0] = '*';
        lex->current.text[1] = '\0';
        lex->pos++;
        return;
    }
    if (c == '(') {
        lex->current.type = TOK_LPAREN;
        lex->current.text[0] = '(';
        lex->current.text[1] = '\0';
        lex->pos++;
        return;
    }
    if (c == ')') {
        lex->current.type = TOK_RPAREN;
        lex->current.text[0] = ')';
        lex->current.text[1] = '\0';
        lex->pos++;
        return;
    }
    if (c == '.') {
        lex->current.type = TOK_DOT;
        lex->current.text[0] = '.';
        lex->current.text[1] = '\0';
        lex->pos++;
        return;
    }
    if (c == '=') {
        lex->current.type = TOK_EQ;
        lex->current.text[0] = '=';
        lex->current.text[1] = '\0';
        lex->pos++;
        return;
    }
    if (c == '!') {
        if (lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '=') {
            lex->current.type = TOK_NEQ;
            strcpy(lex->current.text, "!=");
            lex->pos += 2;
            return;
        }
    }
    if (c == '<') {
        if (lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '>') {
            lex->current.type = TOK_NEQ;
            strcpy(lex->current.text, "<>");
            lex->pos += 2;
            return;
        }
        if (lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '=') {
            lex->current.type = TOK_LTE;
            strcpy(lex->current.text, "<=");
            lex->pos += 2;
            return;
        }
        lex->current.type = TOK_LT;
        lex->current.text[0] = '<';
        lex->current.text[1] = '\0';
        lex->pos++;
        return;
    }
    if (c == '>') {
        if (lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '=') {
            lex->current.type = TOK_GTE;
            strcpy(lex->current.text, ">=");
            lex->pos += 2;
            return;
        }
        lex->current.type = TOK_GT;
        lex->current.text[0] = '>';
        lex->current.text[1] = '\0';
        lex->pos++;
        return;
    }

    /* String literal: '...' or "..." */
    if (c == '\'' || c == '"') {
        char quote = c;
        lex->pos++;
        size_t start = lex->pos;
        while (lex->pos < lex->len && lex->src[lex->pos] != quote) {
            lex->pos++;
        }
        size_t slen = lex->pos - start;
        if (slen >= sizeof(lex->current.text)) {
            slen = sizeof(lex->current.text) - 1;
        }
        memcpy(lex->current.text, lex->src + start, slen);
        lex->current.text[slen] = '\0';
        if (lex->pos < lex->len && lex->src[lex->pos] == quote) {
            lex->pos++;
        }
        lex->current.type = TOK_STRING;
        return;
    }

    /* Numeric literal (positive or negative) */
    if (isdigit((unsigned char)c) || (c == '-' && lex->pos + 1 < lex->len && isdigit((unsigned char)lex->src[lex->pos + 1]))) {
        size_t start = lex->pos;
        if (c == '-') lex->pos++;
        while (lex->pos < lex->len && isdigit((unsigned char)lex->src[lex->pos])) {
            lex->pos++;
        }
        size_t nlen = lex->pos - start;
        if (nlen >= sizeof(lex->current.text)) nlen = sizeof(lex->current.text) - 1;
        memcpy(lex->current.text, lex->src + start, nlen);
        lex->current.text[nlen] = '\0';
        lex->current.int_val = atoll(lex->current.text);
        lex->current.type = TOK_INT;
        return;
    }

    /* Identifiers and Keywords */
    if (isalpha((unsigned char)c) || c == '_') {
        size_t start = lex->pos;
        while (lex->pos < lex->len && (isalnum((unsigned char)lex->src[lex->pos]) || lex->src[lex->pos] == '_')) {
            lex->pos++;
        }
        size_t ilen = lex->pos - start;
        if (ilen >= sizeof(lex->current.text)) ilen = sizeof(lex->current.text) - 1;
        memcpy(lex->current.text, lex->src + start, ilen);
        lex->current.text[ilen] = '\0';

        /* Keyword check */
        if (case_eq(lex->current.text, "SELECT")) lex->current.type = KW_SELECT;
        else if (case_eq(lex->current.text, "FROM")) lex->current.type = KW_FROM;
        else if (case_eq(lex->current.text, "WHERE")) lex->current.type = KW_WHERE;
        else if (case_eq(lex->current.text, "INSERT")) lex->current.type = KW_INSERT;
        else if (case_eq(lex->current.text, "INTO")) lex->current.type = KW_INTO;
        else if (case_eq(lex->current.text, "VALUES")) lex->current.type = KW_VALUES;
        else if (case_eq(lex->current.text, "UPDATE")) lex->current.type = KW_UPDATE;
        else if (case_eq(lex->current.text, "SET")) lex->current.type = KW_SET;
        else if (case_eq(lex->current.text, "DELETE")) lex->current.type = KW_DELETE;
        else if (case_eq(lex->current.text, "JOIN")) lex->current.type = KW_JOIN;
        else if (case_eq(lex->current.text, "ON")) lex->current.type = KW_ON;
        else if (case_eq(lex->current.text, "GROUP")) lex->current.type = KW_GROUP;
        else if (case_eq(lex->current.text, "BY")) lex->current.type = KW_BY;
        else if (case_eq(lex->current.text, "ORDER")) lex->current.type = KW_ORDER;
        else if (case_eq(lex->current.text, "ASC")) lex->current.type = KW_ASC;
        else if (case_eq(lex->current.text, "DESC")) lex->current.type = KW_DESC;
        else if (case_eq(lex->current.text, "CREATE")) lex->current.type = KW_CREATE;
        else if (case_eq(lex->current.text, "TABLE")) lex->current.type = KW_TABLE;
        else if (case_eq(lex->current.text, "INDEX")) lex->current.type = KW_INDEX;
        else if (case_eq(lex->current.text, "EXPLAIN")) lex->current.type = KW_EXPLAIN;
        else if (case_eq(lex->current.text, "AND")) lex->current.type = KW_AND;
        else if (case_eq(lex->current.text, "OR")) lex->current.type = KW_OR;
        else if (case_eq(lex->current.text, "INT")) lex->current.type = KW_INT;
        else if (case_eq(lex->current.text, "BIGINT")) lex->current.type = KW_BIGINT;
        else if (case_eq(lex->current.text, "VARCHAR")) lex->current.type = KW_VARCHAR;
        else if (case_eq(lex->current.text, "TEXT")) lex->current.type = KW_TEXT;
        else if (case_eq(lex->current.text, "COUNT")) lex->current.type = KW_COUNT;
        else if (case_eq(lex->current.text, "SUM")) lex->current.type = KW_SUM;
        else if (case_eq(lex->current.text, "AS")) lex->current.type = KW_AS;
        else lex->current.type = TOK_IDENT;

        return;
    }

    lex->pos++;
    lexer_next(lex);
}

expr_t* expr_create_binary(binary_op_t op, expr_t* left, expr_t* right) {
    expr_t* e = (expr_t*)calloc(1, sizeof(expr_t));
    if (!e) return NULL;
    e->type = EXPR_BINARY_OP;
    e->binary.op = op;
    e->binary.left = left;
    e->binary.right = right;
    return e;
}

expr_t* expr_create_col_ref(const char* tbl, const char* col) {
    expr_t* e = (expr_t*)calloc(1, sizeof(expr_t));
    if (!e) return NULL;
    e->type = EXPR_COLUMN_REF;
    if (tbl) safe_strcpy(e->col_ref.table_name, tbl, sizeof(e->col_ref.table_name));
    if (col) safe_strcpy(e->col_ref.column_name, col, sizeof(e->col_ref.column_name));
    return e;
}

expr_t* expr_create_literal(value_t val) {
    expr_t* e = (expr_t*)calloc(1, sizeof(expr_t));
    if (!e) return NULL;
    e->type = EXPR_LITERAL;
    e->literal = val;
    return e;
}

void expr_free(expr_t* expr) {
    if (!expr) return;
    if (expr->type == EXPR_BINARY_OP) {
        expr_free(expr->binary.left);
        expr_free(expr->binary.right);
    }
    free(expr);
}

void statement_free(statement_t* stmt) {
    if (!stmt) return;
    if (stmt->type == STMT_SELECT) {
        expr_free(stmt->select_stmt.join_condition);
        expr_free(stmt->select_stmt.where_clause);
    } else if (stmt->type == STMT_UPDATE) {
        expr_free(stmt->update_stmt.where_clause);
    } else if (stmt->type == STMT_DELETE) {
        expr_free(stmt->delete_stmt.where_clause);
    }
}

static expr_t* parse_expr(lexer_t* lex);

static expr_t* parse_primary_expr(lexer_t* lex) {
    if (lex->current.type == TOK_INT) {
        value_t v = value_make_int(lex->current.int_val);
        lexer_next(lex);
        return expr_create_literal(v);
    }
    if (lex->current.type == TOK_STRING) {
        value_t v = value_make_str(lex->current.text);
        lexer_next(lex);
        return expr_create_literal(v);
    }
    if (lex->current.type == TOK_IDENT) {
        char ident1[MAX_COLUMN_NAME];
        safe_strcpy(ident1, lex->current.text, sizeof(ident1));
        lexer_next(lex);

        if (lex->current.type == TOK_DOT) {
            lexer_next(lex);
            if (lex->current.type == TOK_IDENT) {
                char ident2[MAX_COLUMN_NAME];
                safe_strcpy(ident2, lex->current.text, sizeof(ident2));
                lexer_next(lex);
                return expr_create_col_ref(ident1, ident2);
            }
        }
        return expr_create_col_ref(NULL, ident1);
    }
    if (lex->current.type == TOK_LPAREN) {
        lexer_next(lex);
        expr_t* e = parse_expr(lex);
        if (lex->current.type == TOK_RPAREN) {
            lexer_next(lex);
        }
        return e;
    }
    return NULL;
}

static expr_t* parse_comparison_expr(lexer_t* lex) {
    expr_t* left = parse_primary_expr(lex);
    if (!left) return NULL;

    binary_op_t op;
    bool has_op = true;

    if (lex->current.type == TOK_EQ) op = OP_EQ;
    else if (lex->current.type == TOK_NEQ) op = OP_NEQ;
    else if (lex->current.type == TOK_LT) op = OP_LT;
    else if (lex->current.type == TOK_LTE) op = OP_LTE;
    else if (lex->current.type == TOK_GT) op = OP_GT;
    else if (lex->current.type == TOK_GTE) op = OP_GTE;
    else has_op = false;

    if (has_op) {
        lexer_next(lex);
        expr_t* right = parse_primary_expr(lex);
        if (!right) {
            expr_free(left);
            return NULL;
        }
        return expr_create_binary(op, left, right);
    }

    return left;
}

static expr_t* parse_expr(lexer_t* lex) {
    expr_t* left = parse_comparison_expr(lex);
    if (!left) return NULL;

    while (lex->current.type == KW_AND || lex->current.type == KW_OR) {
        binary_op_t op = (lex->current.type == KW_AND) ? OP_AND : OP_OR;
        lexer_next(lex);
        expr_t* right = parse_comparison_expr(lex);
        if (!right) {
            expr_free(left);
            return NULL;
        }
        left = expr_create_binary(op, left, right);
    }

    return left;
}

static bool parse_create_table(lexer_t* lex, statement_t* out_stmt) {
    lexer_next(lex); /* skip TABLE */
    if (lex->current.type != TOK_IDENT) return false;

    out_stmt->type = STMT_CREATE_TABLE;
    safe_strcpy(out_stmt->create_table.table_name, lex->current.text, MAX_TABLE_NAME);
    lexer_next(lex);

    if (lex->current.type != TOK_LPAREN) return false;
    lexer_next(lex);

    schema_t* schema = &out_stmt->create_table.schema;
    schema->num_columns = 0;

    while (lex->current.type != TOK_RPAREN && lex->current.type != TOK_EOF) {
        if (lex->current.type != TOK_IDENT) return false;

        column_t* col = &schema->columns[schema->num_columns++];
        safe_strcpy(col->name, lex->current.text, MAX_COLUMN_NAME);
        lexer_next(lex);

        if (lex->current.type == KW_INT || lex->current.type == KW_BIGINT) {
            col->type = TYPE_INT;
            col->max_len = sizeof(int64_t);
            lexer_next(lex);
        } else if (lex->current.type == KW_VARCHAR || lex->current.type == KW_TEXT) {
            col->type = TYPE_VARCHAR;
            col->max_len = MAX_VARCHAR_LEN;
            lexer_next(lex);
            if (lex->current.type == TOK_LPAREN) {
                lexer_next(lex);
                if (lex->current.type == TOK_INT) {
                    col->max_len = (uint32_t)lex->current.int_val;
                    lexer_next(lex);
                }
                if (lex->current.type == TOK_RPAREN) {
                    lexer_next(lex);
                }
            }
        } else {
            return false;
        }

        if (lex->current.type == TOK_COMMA) {
            lexer_next(lex);
        } else {
            break;
        }
    }

    if (lex->current.type == TOK_RPAREN) {
        lexer_next(lex);
    }

    return schema->num_columns > 0;
}

static bool parse_create_index(lexer_t* lex, statement_t* out_stmt) {
    lexer_next(lex); /* skip INDEX */

    /* Optional index name: CREATE INDEX [idx_name] ON tbl (col) */
    if (lex->current.type == TOK_IDENT) {
        lexer_next(lex);
    }

    if (lex->current.type != KW_ON) return false;
    lexer_next(lex);

    if (lex->current.type != TOK_IDENT) return false;
    out_stmt->type = STMT_CREATE_INDEX;
    safe_strcpy(out_stmt->create_index.table_name, lex->current.text, MAX_TABLE_NAME);
    lexer_next(lex);

    if (lex->current.type != TOK_LPAREN) return false;
    lexer_next(lex);

    if (lex->current.type != TOK_IDENT) return false;
    safe_strcpy(out_stmt->create_index.column_name, lex->current.text, MAX_COLUMN_NAME);
    lexer_next(lex);

    if (lex->current.type == TOK_RPAREN) {
        lexer_next(lex);
    }

    return true;
}

static bool parse_insert(lexer_t* lex, statement_t* out_stmt) {
    lexer_next(lex); /* skip INSERT */
    if (lex->current.type != KW_INTO) return false;
    lexer_next(lex);

    if (lex->current.type != TOK_IDENT) return false;
    out_stmt->type = STMT_INSERT;
    safe_strcpy(out_stmt->insert_stmt.table_name, lex->current.text, MAX_TABLE_NAME);
    lexer_next(lex);

    if (lex->current.type != KW_VALUES) return false;
    lexer_next(lex);

    if (lex->current.type != TOK_LPAREN) return false;
    lexer_next(lex);

    out_stmt->insert_stmt.num_values = 0;
    while (lex->current.type != TOK_RPAREN && lex->current.type != TOK_EOF) {
        if (lex->current.type == TOK_INT) {
            out_stmt->insert_stmt.values[out_stmt->insert_stmt.num_values++] = value_make_int(lex->current.int_val);
            lexer_next(lex);
        } else if (lex->current.type == TOK_STRING) {
            out_stmt->insert_stmt.values[out_stmt->insert_stmt.num_values++] = value_make_str(lex->current.text);
            lexer_next(lex);
        } else {
            return false;
        }

        if (lex->current.type == TOK_COMMA) {
            lexer_next(lex);
        } else {
            break;
        }
    }

    if (lex->current.type == TOK_RPAREN) {
        lexer_next(lex);
    }

    return out_stmt->insert_stmt.num_values > 0;
}

static bool parse_update(lexer_t* lex, statement_t* out_stmt) {
    lexer_next(lex); /* skip UPDATE */
    if (lex->current.type != TOK_IDENT) return false;

    out_stmt->type = STMT_UPDATE;
    safe_strcpy(out_stmt->update_stmt.table_name, lex->current.text, MAX_TABLE_NAME);
    lexer_next(lex);

    if (lex->current.type != KW_SET) return false;
    lexer_next(lex);

    if (lex->current.type != TOK_IDENT) return false;
    safe_strcpy(out_stmt->update_stmt.set_column, lex->current.text, MAX_COLUMN_NAME);
    lexer_next(lex);

    if (lex->current.type != TOK_EQ) return false;
    lexer_next(lex);

    if (lex->current.type == TOK_INT) {
        out_stmt->update_stmt.new_value = value_make_int(lex->current.int_val);
        lexer_next(lex);
    } else if (lex->current.type == TOK_STRING) {
        out_stmt->update_stmt.new_value = value_make_str(lex->current.text);
        lexer_next(lex);
    } else {
        return false;
    }

    out_stmt->update_stmt.where_clause = NULL;
    if (lex->current.type == KW_WHERE) {
        lexer_next(lex);
        out_stmt->update_stmt.where_clause = parse_expr(lex);
    }

    return true;
}

static bool parse_delete(lexer_t* lex, statement_t* out_stmt) {
    lexer_next(lex); /* skip DELETE */
    if (lex->current.type != KW_FROM) return false;
    lexer_next(lex);

    if (lex->current.type != TOK_IDENT) return false;
    out_stmt->type = STMT_DELETE;
    safe_strcpy(out_stmt->delete_stmt.table_name, lex->current.text, MAX_TABLE_NAME);
    lexer_next(lex);

    out_stmt->delete_stmt.where_clause = NULL;
    if (lex->current.type == KW_WHERE) {
        lexer_next(lex);
        out_stmt->delete_stmt.where_clause = parse_expr(lex);
    }

    return true;
}

static bool parse_select(lexer_t* lex, statement_t* out_stmt) {
    lexer_next(lex); /* skip SELECT */
    out_stmt->type = STMT_SELECT;
    select_stmt_t* sel = &out_stmt->select_stmt;
    memset(sel, 0, sizeof(select_stmt_t));

    /* Parse projection items */
    if (lex->current.type == TOK_STAR) {
        sel->projections[0].type = PROJ_ALL;
        sel->num_projections = 1;
        lexer_next(lex);
    } else {
        while (lex->current.type != KW_FROM && lex->current.type != TOK_EOF) {
            projection_item_t* proj = &sel->projections[sel->num_projections++];
            if (lex->current.type == KW_COUNT) {
                proj->type = PROJ_AGG_COUNT;
                lexer_next(lex);
                if (lex->current.type == TOK_LPAREN) {
                    lexer_next(lex);
                    if (lex->current.type == TOK_STAR) {
                        safe_strcpy(proj->column_name, "*", MAX_COLUMN_NAME);
                        lexer_next(lex);
                    } else if (lex->current.type == TOK_IDENT) {
                        safe_strcpy(proj->column_name, lex->current.text, MAX_COLUMN_NAME);
                        lexer_next(lex);
                    }
                    if (lex->current.type == TOK_RPAREN) lexer_next(lex);
                }
            } else if (lex->current.type == KW_SUM) {
                proj->type = PROJ_AGG_SUM;
                lexer_next(lex);
                if (lex->current.type == TOK_LPAREN) {
                    lexer_next(lex);
                    if (lex->current.type == TOK_IDENT) {
                        safe_strcpy(proj->column_name, lex->current.text, MAX_COLUMN_NAME);
                        lexer_next(lex);
                    }
                    if (lex->current.type == TOK_RPAREN) lexer_next(lex);
                }
            } else if (lex->current.type == TOK_IDENT) {
                proj->type = PROJ_COLUMN;
                char ident[MAX_COLUMN_NAME];
                safe_strcpy(ident, lex->current.text, sizeof(ident));
                lexer_next(lex);
                if (lex->current.type == TOK_DOT) {
                    lexer_next(lex);
                    safe_strcpy(proj->table_name, ident, MAX_TABLE_NAME);
                    safe_strcpy(proj->column_name, lex->current.text, MAX_COLUMN_NAME);
                    lexer_next(lex);
                } else {
                    safe_strcpy(proj->column_name, ident, MAX_COLUMN_NAME);
                }
            } else {
                return false;
            }

            /* Optional AS alias */
            if (lex->current.type == KW_AS) {
                lexer_next(lex);
                if (lex->current.type == TOK_IDENT) {
                    safe_strcpy(proj->alias, lex->current.text, MAX_COLUMN_NAME);
                    lexer_next(lex);
                }
            }

            if (lex->current.type == TOK_COMMA) {
                lexer_next(lex);
            } else {
                break;
            }
        }
    }

    if (lex->current.type != KW_FROM) return false;
    lexer_next(lex);

    if (lex->current.type != TOK_IDENT) return false;
    safe_strcpy(sel->from_table, lex->current.text, MAX_TABLE_NAME);
    lexer_next(lex);

    /* Optional JOIN */
    if (lex->current.type == KW_JOIN) {
        lexer_next(lex);
        if (lex->current.type != TOK_IDENT) return false;
        sel->has_join = true;
        safe_strcpy(sel->join_table, lex->current.text, MAX_TABLE_NAME);
        lexer_next(lex);

        if (lex->current.type != KW_ON) return false;
        lexer_next(lex);
        sel->join_condition = parse_expr(lex);
    }

    /* Optional WHERE */
    if (lex->current.type == KW_WHERE) {
        lexer_next(lex);
        sel->where_clause = parse_expr(lex);
    }

    /* Optional GROUP BY */
    if (lex->current.type == KW_GROUP) {
        lexer_next(lex);
        if (lex->current.type != KW_BY) return false;
        lexer_next(lex);
        if (lex->current.type != TOK_IDENT) return false;
        sel->has_group_by = true;
        safe_strcpy(sel->group_by_col, lex->current.text, MAX_COLUMN_NAME);
        lexer_next(lex);
    }

    /* Optional ORDER BY */
    if (lex->current.type == KW_ORDER) {
        lexer_next(lex);
        if (lex->current.type != KW_BY) return false;
        lexer_next(lex);
        if (lex->current.type != TOK_IDENT) return false;
        sel->has_order_by = true;
        safe_strcpy(sel->order_by_col, lex->current.text, MAX_COLUMN_NAME);
        lexer_next(lex);

        if (lex->current.type == KW_DESC) {
            sel->is_desc = true;
            lexer_next(lex);
        } else if (lex->current.type == KW_ASC) {
            sel->is_desc = false;
            lexer_next(lex);
        }
    }

    return true;
}

bool parse_sql(const char* sql, statement_t* out_stmt) {
    if (!sql || !out_stmt) return false;
    memset(out_stmt, 0, sizeof(statement_t));

    lexer_t lex;
    lexer_init(&lex, sql);

    if (lex.current.type == TOK_EOF) return false;

    if (lex.current.type == KW_EXPLAIN) {
        out_stmt->is_explain = true;
        lexer_next(&lex);
    }

    if (lex.current.type == KW_SELECT) {
        return parse_select(&lex, out_stmt);
    } else if (lex.current.type == KW_INSERT) {
        return parse_insert(&lex, out_stmt);
    } else if (lex.current.type == KW_UPDATE) {
        return parse_update(&lex, out_stmt);
    } else if (lex.current.type == KW_DELETE) {
        return parse_delete(&lex, out_stmt);
    } else if (lex.current.type == KW_CREATE) {
        lexer_next(&lex);
        if (lex.current.type == KW_TABLE) {
            return parse_create_table(&lex, out_stmt);
        } else if (lex.current.type == KW_INDEX) {
            return parse_create_index(&lex, out_stmt);
        }
    }

    return false;
}

bool expr_evaluate(const expr_t* expr, const schema_t* schema, const tuple_t* tuple, value_t* out_val) {
    if (!expr || !out_val) return false;

    if (expr->type == EXPR_LITERAL) {
        *out_val = expr->literal;
        return true;
    }

    if (expr->type == EXPR_COLUMN_REF) {
        int idx = schema_get_column_index(schema, expr->col_ref.column_name);
        if (idx < 0 || (uint32_t)idx >= tuple->num_values) {
            return false;
        }
        *out_val = tuple->values[idx];
        return true;
    }

    if (expr->type == EXPR_BINARY_OP) {
        value_t left_val, right_val;
        if (!expr_evaluate(expr->binary.left, schema, tuple, &left_val)) return false;
        if (!expr_evaluate(expr->binary.right, schema, tuple, &right_val)) return false;

        if (expr->binary.op == OP_AND) {
            bool ok = (!left_val.is_null && left_val.int_val != 0) && (!right_val.is_null && right_val.int_val != 0);
            *out_val = value_make_int(ok ? 1 : 0);
            return true;
        }

        if (expr->binary.op == OP_OR) {
            bool ok = (!left_val.is_null && left_val.int_val != 0) || (!right_val.is_null && right_val.int_val != 0);
            *out_val = value_make_int(ok ? 1 : 0);
            return true;
        }

        int cmp = value_compare(&left_val, &right_val);
        bool res = false;
        switch (expr->binary.op) {
            case OP_EQ:  res = (cmp == 0); break;
            case OP_NEQ: res = (cmp != 0); break;
            case OP_LT:  res = (cmp < 0); break;
            case OP_LTE: res = (cmp <= 0); break;
            case OP_GT:  res = (cmp > 0); break;
            case OP_GTE: res = (cmp >= 0); break;
            default: break;
        }

        *out_val = value_make_int(res ? 1 : 0);
        return true;
    }

    return false;
}
