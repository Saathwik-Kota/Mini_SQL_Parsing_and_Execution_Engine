#include "../include/slotted_page.h"
#include <string.h>
#include <stdlib.h>

void slotted_page_init(void* page_data, page_id_t page_id) {
    if (!page_data) {
        return;
    }
    memset(page_data, 0, PAGE_SIZE);
    slotted_page_header_t* header = (slotted_page_header_t*)page_data;
    header->page_id = page_id;
    header->next_page_id = INVALID_PAGE_ID;
    header->prev_page_id = INVALID_PAGE_ID;
    header->num_slots = 0;
    header->free_space_offset = PAGE_SIZE;
    header->tuple_count = 0;
    header->flags = 0;
}

slotted_page_header_t* slotted_page_get_header(void* page_data) {
    return (slotted_page_header_t*)page_data;
}

const slotted_page_header_t* slotted_page_get_header_const(const void* page_data) {
    return (const slotted_page_header_t*)page_data;
}

page_slot_t* slotted_page_get_slot(void* page_data, uint16_t slot_idx) {
    if (!page_data) {
        return NULL;
    }
    slotted_page_header_t* header = slotted_page_get_header(page_data);
    if (slot_idx >= header->num_slots) {
        return NULL;
    }
    page_slot_t* slots = (page_slot_t*)((uint8_t*)page_data + sizeof(slotted_page_header_t));
    return &slots[slot_idx];
}

const page_slot_t* slotted_page_get_slot_const(const void* page_data, uint16_t slot_idx) {
    if (!page_data) {
        return NULL;
    }
    const slotted_page_header_t* header = slotted_page_get_header_const(page_data);
    if (slot_idx >= header->num_slots) {
        return NULL;
    }
    const page_slot_t* slots = (const page_slot_t*)((const uint8_t*)page_data + sizeof(slotted_page_header_t));
    return &slots[slot_idx];
}

uint16_t slotted_page_get_free_space(const void* page_data) {
    if (!page_data) {
        return 0;
    }
    const slotted_page_header_t* header = slotted_page_get_header_const(page_data);
    uint16_t slots_end = (uint16_t)(sizeof(slotted_page_header_t) + header->num_slots * sizeof(page_slot_t));
    if (header->free_space_offset < slots_end) {
        return 0;
    }
    return header->free_space_offset - slots_end;
}

void slotted_page_compact(void* page_data) {
    if (!page_data) {
        return;
    }
    slotted_page_header_t* header = slotted_page_get_header(page_data);
    uint8_t temp[PAGE_SIZE] __attribute__((aligned(8)));
    memset(temp, 0, PAGE_SIZE);

    slotted_page_header_t* temp_hdr = (slotted_page_header_t*)temp;
    *temp_hdr = *header;
    temp_hdr->free_space_offset = PAGE_SIZE;

    page_slot_t* orig_slots = (page_slot_t*)((uint8_t*)page_data + sizeof(slotted_page_header_t));
    page_slot_t* temp_slots = (page_slot_t*)((uint8_t*)temp + sizeof(slotted_page_header_t));

    uint16_t current_free = PAGE_SIZE;

    for (uint16_t i = 0; i < header->num_slots; i++) {
        if (orig_slots[i].length > 0) {
            current_free -= orig_slots[i].length;
            memcpy(temp + current_free, (uint8_t*)page_data + orig_slots[i].offset, orig_slots[i].length);
            temp_slots[i].offset = current_free;
            temp_slots[i].length = orig_slots[i].length;
        } else {
            temp_slots[i].offset = 0;
            temp_slots[i].length = 0;
        }
    }

    temp_hdr->free_space_offset = current_free;
    memcpy(page_data, temp, PAGE_SIZE);
}

bool slotted_page_can_insert(const void* page_data, uint16_t tuple_len) {
    if (!page_data) {
        return false;
    }
    const slotted_page_header_t* header = slotted_page_get_header_const(page_data);
    uint16_t free_space = slotted_page_get_free_space(page_data);

    /* Check if an empty slot exists */
    bool has_reusable_slot = false;
    const page_slot_t* slots = (const page_slot_t*)((const uint8_t*)page_data + sizeof(slotted_page_header_t));
    for (uint16_t i = 0; i < header->num_slots; i++) {
        if (slots[i].length == 0) {
            has_reusable_slot = true;
            break;
        }
    }

    uint16_t needed = tuple_len + (has_reusable_slot ? 0 : sizeof(page_slot_t));
    if (free_space >= needed) {
        return true;
    }

    /* Check if compaction could yield enough space */
    uint16_t active_bytes = 0;
    for (uint16_t i = 0; i < header->num_slots; i++) {
        if (slots[i].length > 0) {
            active_bytes += slots[i].length;
        }
    }
    uint16_t potential_free = (uint16_t)(PAGE_SIZE - (sizeof(slotted_page_header_t) + header->num_slots * sizeof(page_slot_t)) - active_bytes);
    return potential_free >= needed;
}

bool slotted_page_insert_tuple(void* page_data, const uint8_t* tuple_bytes, uint16_t tuple_len, uint16_t* out_slot_num) {
    if (!page_data || !tuple_bytes || !out_slot_num) {
        return false;
    }

    slotted_page_header_t* header = slotted_page_get_header(page_data);
    page_slot_t* slots = (page_slot_t*)((uint8_t*)page_data + sizeof(slotted_page_header_t));

    /* Check for an existing deleted slot */
    int reuse_idx = -1;
    for (uint16_t i = 0; i < header->num_slots; i++) {
        if (slots[i].length == 0) {
            reuse_idx = (int)i;
            break;
        }
    }

    uint16_t needed_space = tuple_len + (reuse_idx >= 0 ? 0 : sizeof(page_slot_t));
    uint16_t free_space = slotted_page_get_free_space(page_data);

    if (free_space < needed_space) {
        slotted_page_compact(page_data);
        free_space = slotted_page_get_free_space(page_data);
        if (free_space < needed_space) {
            return false;
        }
    }

    uint16_t slot_idx;
    if (reuse_idx >= 0) {
        slot_idx = (uint16_t)reuse_idx;
    } else {
        slot_idx = header->num_slots;
        header->num_slots++;
    }

    header->free_space_offset -= tuple_len;
    memcpy((uint8_t*)page_data + header->free_space_offset, tuple_bytes, tuple_len);

    slots[slot_idx].offset = header->free_space_offset;
    slots[slot_idx].length = tuple_len;
    header->tuple_count++;

    *out_slot_num = slot_idx;
    return true;
}

bool slotted_page_get_tuple(const void* page_data, uint16_t slot_num, uint8_t* out_bytes, uint16_t* out_len) {
    if (!page_data || !out_bytes || !out_len) {
        return false;
    }

    const slotted_page_header_t* header = slotted_page_get_header_const(page_data);
    if (slot_num >= header->num_slots) {
        return false;
    }

    const page_slot_t* slot = slotted_page_get_slot_const(page_data, slot_num);
    if (!slot || slot->length == 0) {
        return false;
    }

    memcpy(out_bytes, (const uint8_t*)page_data + slot->offset, slot->length);
    *out_len = slot->length;
    return true;
}

bool slotted_page_update_tuple(void* page_data, uint16_t slot_num, const uint8_t* tuple_bytes, uint16_t tuple_len) {
    if (!page_data || !tuple_bytes) {
        return false;
    }

    slotted_page_header_t* header = slotted_page_get_header(page_data);
    if (slot_num >= header->num_slots) {
        return false;
    }

    page_slot_t* slot = slotted_page_get_slot(page_data, slot_num);
    if (!slot || slot->length == 0) {
        return false;
    }

    if (tuple_len <= slot->length) {
        /* Overwrite in-place */
        memcpy((uint8_t*)page_data + slot->offset, tuple_bytes, tuple_len);
        slot->length = tuple_len;
        return true;
    }

    /* Tuple grew: mark old space deleted, then attempt reallocation */
    slot->length = 0;
    header->tuple_count--;

    uint16_t free_space = slotted_page_get_free_space(page_data);
    if (free_space < tuple_len) {
        slotted_page_compact(page_data);
        free_space = slotted_page_get_free_space(page_data);
        if (free_space < tuple_len) {
            return false;
        }
    }

    header->free_space_offset -= tuple_len;
    memcpy((uint8_t*)page_data + header->free_space_offset, tuple_bytes, tuple_len);

    slot->offset = header->free_space_offset;
    slot->length = tuple_len;
    header->tuple_count++;

    return true;
}

bool slotted_page_delete_tuple(void* page_data, uint16_t slot_num) {
    if (!page_data) {
        return false;
    }

    slotted_page_header_t* header = slotted_page_get_header(page_data);
    if (slot_num >= header->num_slots) {
        return false;
    }

    page_slot_t* slot = slotted_page_get_slot(page_data, slot_num);
    if (!slot || slot->length == 0) {
        return false;
    }

    slot->length = 0;
    slot->offset = 0;
    header->tuple_count--;
    return true;
}

uint16_t tuple_serialize(const schema_t* schema, const tuple_t* tuple, uint8_t* out_buffer, uint16_t max_size) {
    if (!schema || !tuple || !out_buffer) {
        return 0;
    }

    uint16_t offset = 0;
    uint16_t num_cols = (uint16_t)schema->num_columns;
    if (offset + sizeof(uint16_t) > max_size) return 0;
    memcpy(out_buffer + offset, &num_cols, sizeof(uint16_t));
    offset += sizeof(uint16_t);

    for (uint32_t i = 0; i < schema->num_columns; i++) {
        const value_t* val = &tuple->values[i];
        uint8_t is_null = val->is_null ? 1 : 0;
        if (offset + sizeof(uint8_t) > max_size) return 0;
        out_buffer[offset++] = is_null;

        if (is_null) {
            continue;
        }

        if (schema->columns[i].type == TYPE_INT) {
            if (offset + sizeof(int64_t) > max_size) return 0;
            memcpy(out_buffer + offset, &val->int_val, sizeof(int64_t));
            offset += sizeof(int64_t);
        } else if (schema->columns[i].type == TYPE_VARCHAR) {
            uint16_t str_len = (uint16_t)strlen(val->str_val);
            if (offset + sizeof(uint16_t) + str_len > max_size) return 0;
            memcpy(out_buffer + offset, &str_len, sizeof(uint16_t));
            offset += sizeof(uint16_t);
            memcpy(out_buffer + offset, val->str_val, str_len);
            offset += str_len;
        }
    }

    return offset;
}

bool tuple_deserialize(const schema_t* schema, const uint8_t* buffer, uint16_t buffer_len, tuple_t* out_tuple) {
    if (!schema || !buffer || !out_tuple) {
        return false;
    }

    uint16_t offset = 0;
    if (offset + sizeof(uint16_t) > buffer_len) return false;
    uint16_t num_cols;
    memcpy(&num_cols, buffer + offset, sizeof(uint16_t));
    offset += sizeof(uint16_t);

    if (num_cols != schema->num_columns) {
        return false;
    }

    out_tuple->num_values = num_cols;

    for (uint32_t i = 0; i < schema->num_columns; i++) {
        if (offset + sizeof(uint8_t) > buffer_len) return false;
        uint8_t is_null = buffer[offset++];
        out_tuple->values[i].type = schema->columns[i].type;
        out_tuple->values[i].is_null = (is_null != 0);

        if (is_null) {
            out_tuple->values[i].int_val = 0;
            out_tuple->values[i].str_val[0] = '\0';
            continue;
        }

        if (schema->columns[i].type == TYPE_INT) {
            if (offset + sizeof(int64_t) > buffer_len) return false;
            memcpy(&out_tuple->values[i].int_val, buffer + offset, sizeof(int64_t));
            offset += sizeof(int64_t);
            out_tuple->values[i].str_val[0] = '\0';
        } else if (schema->columns[i].type == TYPE_VARCHAR) {
            if (offset + sizeof(uint16_t) > buffer_len) return false;
            uint16_t str_len;
            memcpy(&str_len, buffer + offset, sizeof(uint16_t));
            offset += sizeof(uint16_t);

            if (offset + str_len > buffer_len || str_len >= sizeof(out_tuple->values[i].str_val)) {
                return false;
            }
            memcpy(out_tuple->values[i].str_val, buffer + offset, str_len);
            out_tuple->values[i].str_val[str_len] = '\0';
            offset += str_len;
            out_tuple->values[i].int_val = 0;
        }
    }

    return true;
}

