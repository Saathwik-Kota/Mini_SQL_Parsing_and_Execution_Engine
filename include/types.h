#ifndef MINI_SQL_TYPES_H
#define MINI_SQL_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PAGE_SIZE 4096
#define INVALID_PAGE_ID ((page_id_t)0xFFFFFFFF)
#define INVALID_FRAME_ID ((frame_id_t)-1)
#define MAX_COLUMNS 16
#define MAX_VARCHAR_LEN 256
#define MAX_TABLE_NAME 32
#define MAX_COLUMN_NAME 32

typedef uint32_t page_id_t;
typedef int32_t frame_id_t;

typedef enum {
    TYPE_INT,
    TYPE_VARCHAR
} data_type_t;

typedef struct {
    data_type_t type;
    bool is_null;
    int64_t int_val;
    char str_val[MAX_VARCHAR_LEN];
} value_t;

typedef struct {
    char name[MAX_COLUMN_NAME];
    data_type_t type;
    uint32_t max_len;
} column_t;

typedef struct {
    uint32_t num_columns;
    column_t columns[MAX_COLUMNS];
} schema_t;

typedef struct {
    page_id_t page_id;
    uint16_t slot_num;
} rid_t;

typedef struct {
    rid_t rid;
    uint32_t num_values;
    value_t values[MAX_COLUMNS];
} tuple_t;

/* Value constructors and utilities */
value_t value_make_int(int64_t val);
value_t value_make_str(const char* str);
value_t value_make_null(data_type_t type);
int value_compare(const value_t* a, const value_t* b);
void value_to_string(const value_t* val, char* out_buf, size_t buf_size);

/* Schema helper functions */
int schema_get_column_index(const schema_t* schema, const char* col_name);

#endif /* MINI_SQL_TYPES_H */

