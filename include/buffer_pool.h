#ifndef MINI_SQL_BUFFER_POOL_H
#define MINI_SQL_BUFFER_POOL_H

#include "types.h"
#include "disk_manager.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define DEFAULT_BUFFER_POOL_SIZE 64

typedef struct {
    page_id_t page_id;
    int32_t pin_count;
    bool is_dirty;
    uint8_t data[PAGE_SIZE];
} frame_t;

typedef struct lru_node_t {
    frame_id_t frame_id;
    struct lru_node_t* prev;
    struct lru_node_t* next;
} lru_node_t;

typedef struct hash_entry_t {
    page_id_t page_id;
    frame_id_t frame_id;
    struct hash_entry_t* next;
} hash_entry_t;

typedef struct {
    disk_manager_t* dm;
    size_t pool_size;
    frame_t* frames;

    /* Free list */
    frame_id_t* free_list;
    size_t free_count;

    /* LRU replacer */
    lru_node_t* lru_nodes;
    bool* in_lru;
    lru_node_t* lru_head; /* Least recently used */
    lru_node_t* lru_tail; /* Most recently used */

    /* Page table: hash map from page_id to frame_id */
    hash_entry_t** page_table;
    size_t hash_size;

    /* Statistics */
    uint64_t num_requests;
    uint64_t num_hits;
    uint64_t num_misses;
} buffer_pool_manager_t;

/* Buffer Pool Operations */
buffer_pool_manager_t* bpm_create(disk_manager_t* dm, size_t pool_size);
void bpm_destroy(buffer_pool_manager_t* bpm);

frame_t* bpm_fetch_page(buffer_pool_manager_t* bpm, page_id_t page_id);
bool bpm_unpin_page(buffer_pool_manager_t* bpm, page_id_t page_id, bool is_dirty);
frame_t* bpm_new_page(buffer_pool_manager_t* bpm, page_id_t* out_page_id);
bool bpm_delete_page(buffer_pool_manager_t* bpm, page_id_t page_id);
bool bpm_flush_page(buffer_pool_manager_t* bpm, page_id_t page_id);
void bpm_flush_all(buffer_pool_manager_t* bpm);

/* Statistics */
double bpm_get_hit_rate(const buffer_pool_manager_t* bpm);
void bpm_print_stats(const buffer_pool_manager_t* bpm);
void bpm_reset_stats(buffer_pool_manager_t* bpm);

#endif /* MINI_SQL_BUFFER_POOL_H */

