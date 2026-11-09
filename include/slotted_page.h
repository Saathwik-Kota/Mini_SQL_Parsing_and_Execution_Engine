#ifndef MINI_SQL_SLOTTED_PAGE_H
#define MINI_SQL_SLOTTED_PAGE_H

#include "types.h"
#include <stdint.h>
#include <stdbool.h>

#define SLOT_DELETED 0

typedef struct {
    uint16_t offset; /* Byte offset where tuple data begins in the page */
    uint16_t length; /* Byte length of tuple; 0 indicates deleted slot */
} page_slot_t;

typedef struct {
    page_id_t page_id;
    page_id_t next_page_id;
    page_id_t prev_page_id;
    uint16_t num_slots;          /* Number of allocated slots in slot array */
    uint16_t free_space_offset;  /* Offset from page start to free space boundary */
    uint16_t tuple_count;        /* Number of active (non-deleted) tuples */
    uint16_t flags;
} slotted_page_header_t;

/* Slotted Page Operations */
void slotted_page_init(void* page_data, page_id_t page_id);
slotted_page_header_t* slotted_page_get_header(void* page_data);
const slotted_page_header_t* slotted_page_get_header_const(const void* page_data);
page_slot_t* slotted_page_get_slot(void* page_data, uint16_t slot_idx);
const page_slot_t* slotted_page_get_slot_const(const void* page_data, uint16_t slot_idx);

uint16_t slotted_page_get_free_space(const void* page_data);
bool slotted_page_can_insert(const void* page_data, uint16_t tuple_len);
bool slotted_page_insert_tuple(void* page_data, const uint8_t* tuple_bytes, uint16_t tuple_len, uint16_t* out_slot_num);
bool slotted_page_get_tuple(const void* page_data, uint16_t slot_num, uint8_t* out_bytes, uint16_t* out_len);
bool slotted_page_update_tuple(void* page_data, uint16_t slot_num, const uint8_t* tuple_bytes, uint16_t tuple_len);
bool slotted_page_delete_tuple(void* page_data, uint16_t slot_num);
void slotted_page_compact(void* page_data);

/* Tuple Serialization for Variable-Length Records */
uint16_t tuple_serialize(const schema_t* schema, const tuple_t* tuple, uint8_t* out_buffer, uint16_t max_size);
bool tuple_deserialize(const schema_t* schema, const uint8_t* buffer, uint16_t buffer_len, tuple_t* out_tuple);

#endif /* MINI_SQL_SLOTTED_PAGE_H */

