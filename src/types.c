#include "../include/types.h"
#include <stdio.h>
#include <string.h>

value_t value_make_int(int64_t val) {
    value_t v;
    v.type = TYPE_INT;
    v.is_null = false;
    v.int_val = val;
    v.str_val[0] = '\0';
    return v;
}

value_t value_make_str(const char* str) {
    value_t v;
    v.type = TYPE_VARCHAR;
    v.is_null = false;
    v.int_val = 0;
    if (str) {
        strncpy(v.str_val, str, sizeof(v.str_val) - 1);
        v.str_val[sizeof(v.str_val) - 1] = '\0';
    } else {
        v.str_val[0] = '\0';
    }
    return v;
}

value_t value_make_null(data_type_t type) {
    value_t v;
    v.type = type;
    v.is_null = true;
    v.int_val = 0;
    v.str_val[0] = '\0';
    return v;
}

int value_compare(const value_t* a, const value_t* b) {
    if (a->is_null && b->is_null) return 0;
    if (a->is_null) return -1;
    if (b->is_null) return 1;

    if (a->type == TYPE_INT && b->type == TYPE_INT) {
        if (a->int_val < b->int_val) return -1;
        if (a->int_val > b->int_val) return 1;
        return 0;
    }

    if (a->type == TYPE_VARCHAR && b->type == TYPE_VARCHAR) {
        return strcmp(a->str_val, b->str_val);
    }

    /* Cross-type fallback */
    if (a->type == TYPE_INT && b->type == TYPE_VARCHAR) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%lld", (long long)a->int_val);
        return strcmp(buf, b->str_val);
    }

    if (a->type == TYPE_VARCHAR && b->type == TYPE_INT) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%lld", (long long)b->int_val);
        return strcmp(a->str_val, buf);
    }

    return 0;
}

void value_to_string(const value_t* val, char* out_buf, size_t buf_size) {
    if (!val || !out_buf || buf_size == 0) return;

    if (val->is_null) {
        snprintf(out_buf, buf_size, "NULL");
        return;
    }

    if (val->type == TYPE_INT) {
        snprintf(out_buf, buf_size, "%lld", (long long)val->int_val);
    } else {
        snprintf(out_buf, buf_size, "%s", val->str_val);
    }
}

int schema_get_column_index(const schema_t* schema, const char* col_name) {
    if (!schema || !col_name) return -1;
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        if (strcmp(schema->columns[i].name, col_name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

