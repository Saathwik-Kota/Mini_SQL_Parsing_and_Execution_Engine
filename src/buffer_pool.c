#include "../include/buffer_pool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static inline size_t hash_func(page_id_t page_id, size_t hash_size) {
    return ((size_t)page_id * 2654435761u) % hash_size;
}

static frame_id_t page_table_find(const buffer_pool_manager_t* bpm, page_id_t page_id) {
    size_t bucket = hash_func(page_id, bpm->hash_size);
    hash_entry_t* curr = bpm->page_table[bucket];
    while (curr) {
        if (curr->page_id == page_id) {
            return curr->frame_id;
        }
        curr = curr->next;
    }
    return INVALID_FRAME_ID;
}

static void page_table_insert(buffer_pool_manager_t* bpm, page_id_t page_id, frame_id_t frame_id) {
    size_t bucket = hash_func(page_id, bpm->hash_size);
    hash_entry_t* entry = (hash_entry_t*)malloc(sizeof(hash_entry_t));
    if (!entry) return;
    entry->page_id = page_id;
    entry->frame_id = frame_id;
    entry->next = bpm->page_table[bucket];
    bpm->page_table[bucket] = entry;
}

static void page_table_remove(buffer_pool_manager_t* bpm, page_id_t page_id) {
    size_t bucket = hash_func(page_id, bpm->hash_size);
    hash_entry_t* curr = bpm->page_table[bucket];
    hash_entry_t* prev = NULL;

    while (curr) {
        if (curr->page_id == page_id) {
            if (prev) {
                prev->next = curr->next;
            } else {
                bpm->page_table[bucket] = curr->next;
            }
            free(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

static void lru_remove(buffer_pool_manager_t* bpm, frame_id_t frame_id) {
    if (!bpm->in_lru[frame_id]) {
        return;
    }

    lru_node_t* node = &bpm->lru_nodes[frame_id];
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        bpm->lru_head = node->next;
    }

    if (node->next) {
        node->next->prev = node->prev;
    } else {
        bpm->lru_tail = node->prev;
    }

    node->prev = NULL;
    node->next = NULL;
    bpm->in_lru[frame_id] = false;
}

static void lru_insert(buffer_pool_manager_t* bpm, frame_id_t frame_id) {
    if (bpm->in_lru[frame_id]) {
        lru_remove(bpm, frame_id);
    }

    lru_node_t* node = &bpm->lru_nodes[frame_id];
    node->frame_id = frame_id;
    node->prev = bpm->lru_tail;
    node->next = NULL;

    if (bpm->lru_tail) {
        bpm->lru_tail->next = node;
    } else {
        bpm->lru_head = node;
    }
    bpm->lru_tail = node;
    bpm->in_lru[frame_id] = true;
}

static bool lru_victim(buffer_pool_manager_t* bpm, frame_id_t* out_frame_id) {
    if (!bpm->lru_head) {
        return false;
    }

    frame_id_t victim = bpm->lru_head->frame_id;
    lru_remove(bpm, victim);
    *out_frame_id = victim;
    return true;
}

buffer_pool_manager_t* bpm_create(disk_manager_t* dm, size_t pool_size) {
    if (!dm || pool_size == 0) {
        return NULL;
    }

    buffer_pool_manager_t* bpm = (buffer_pool_manager_t*)calloc(1, sizeof(buffer_pool_manager_t));
    if (!bpm) return NULL;

    bpm->dm = dm;
    bpm->pool_size = pool_size;
    bpm->frames = (frame_t*)calloc(pool_size, sizeof(frame_t));
    bpm->free_list = (frame_id_t*)malloc(pool_size * sizeof(frame_id_t));
    bpm->lru_nodes = (lru_node_t*)calloc(pool_size, sizeof(lru_node_t));
    bpm->in_lru = (bool*)calloc(pool_size, sizeof(bool));

    bpm->hash_size = pool_size * 2 + 1;
    bpm->page_table = (hash_entry_t**)calloc(bpm->hash_size, sizeof(hash_entry_t*));

    if (!bpm->frames || !bpm->free_list || !bpm->lru_nodes || !bpm->in_lru || !bpm->page_table) {
        bpm_destroy(bpm);
        return NULL;
    }

    for (size_t i = 0; i < pool_size; i++) {
        bpm->frames[i].page_id = INVALID_PAGE_ID;
        bpm->frames[i].pin_count = 0;
        bpm->frames[i].is_dirty = false;
        bpm->free_list[i] = (frame_id_t)i;
        bpm->lru_nodes[i].frame_id = (frame_id_t)i;
    }
    bpm->free_count = pool_size;
    bpm->lru_head = NULL;
    bpm->lru_tail = NULL;

    bpm->num_requests = 0;
    bpm->num_hits = 0;
    bpm->num_misses = 0;

    return bpm;
}

void bpm_destroy(buffer_pool_manager_t* bpm) {
    if (!bpm) return;

    bpm_flush_all(bpm);

    if (bpm->page_table) {
        for (size_t i = 0; i < bpm->hash_size; i++) {
            hash_entry_t* curr = bpm->page_table[i];
            while (curr) {
                hash_entry_t* next = curr->next;
                free(curr);
                curr = next;
            }
        }
        free(bpm->page_table);
    }

    free(bpm->frames);
    free(bpm->free_list);
    free(bpm->lru_nodes);
    free(bpm->in_lru);
    free(bpm);
}

frame_t* bpm_fetch_page(buffer_pool_manager_t* bpm, page_id_t page_id) {
    if (!bpm || page_id == INVALID_PAGE_ID) {
        return NULL;
    }

    bpm->num_requests++;
    frame_id_t frame_id = page_table_find(bpm, page_id);

    if (frame_id != INVALID_FRAME_ID) {
        bpm->num_hits++;
        frame_t* frame = &bpm->frames[frame_id];
        if (frame->pin_count == 0) {
            lru_remove(bpm, frame_id);
        }
        frame->pin_count++;
        return frame;
    }

    /* Cache miss */
    bpm->num_misses++;
    frame_id_t target_frame = INVALID_FRAME_ID;

    if (bpm->free_count > 0) {
        target_frame = bpm->free_list[--bpm->free_count];
    } else {
        if (!lru_victim(bpm, &target_frame)) {
            /* All buffer pool frames pinned */
            return NULL;
        }

        /* Victim frame handling */
        frame_t* victim = &bpm->frames[target_frame];
        if (victim->is_dirty) {
            disk_manager_write_page(bpm->dm, victim->page_id, victim->data);
            victim->is_dirty = false;
        }
        page_table_remove(bpm, victim->page_id);
    }

    /* Read requested page from disk */
    frame_t* frame = &bpm->frames[target_frame];
    frame->page_id = page_id;
    frame->pin_count = 1;
    frame->is_dirty = false;

    disk_manager_read_page(bpm->dm, page_id, frame->data);
    page_table_insert(bpm, page_id, target_frame);

    return frame;
}

bool bpm_unpin_page(buffer_pool_manager_t* bpm, page_id_t page_id, bool is_dirty) {
    if (!bpm || page_id == INVALID_PAGE_ID) {
        return false;
    }

    frame_id_t frame_id = page_table_find(bpm, page_id);
    if (frame_id == INVALID_FRAME_ID) {
        return false;
    }

    frame_t* frame = &bpm->frames[frame_id];
    if (frame->pin_count <= 0) {
        return false;
    }

    if (is_dirty) {
        frame->is_dirty = true;
    }

    frame->pin_count--;
    if (frame->pin_count == 0) {
        lru_insert(bpm, frame_id);
    }

    return true;
}

frame_t* bpm_new_page(buffer_pool_manager_t* bpm, page_id_t* out_page_id) {
    if (!bpm || !out_page_id) {
        return NULL;
    }

    frame_id_t target_frame = INVALID_FRAME_ID;

    if (bpm->free_count > 0) {
        target_frame = bpm->free_list[--bpm->free_count];
    } else {
        if (!lru_victim(bpm, &target_frame)) {
            return NULL;
        }

        frame_t* victim = &bpm->frames[target_frame];
        if (victim->is_dirty) {
            disk_manager_write_page(bpm->dm, victim->page_id, victim->data);
            victim->is_dirty = false;
        }
        page_table_remove(bpm, victim->page_id);
    }

    page_id_t new_page_id = disk_manager_allocate_page(bpm->dm);
    if (new_page_id == INVALID_PAGE_ID) {
        /* Return frame back to free list or LRU */
        bpm->free_list[bpm->free_count++] = target_frame;
        return NULL;
    }

    frame_t* frame = &bpm->frames[target_frame];
    frame->page_id = new_page_id;
    frame->pin_count = 1;
    frame->is_dirty = true;
    memset(frame->data, 0, PAGE_SIZE);

    page_table_insert(bpm, new_page_id, target_frame);
    *out_page_id = new_page_id;

    return frame;
}

bool bpm_delete_page(buffer_pool_manager_t* bpm, page_id_t page_id) {
    if (!bpm || page_id == INVALID_PAGE_ID) {
        return false;
    }

    frame_id_t frame_id = page_table_find(bpm, page_id);
    if (frame_id == INVALID_FRAME_ID) {
        return true;
    }

    frame_t* frame = &bpm->frames[frame_id];
    if (frame->pin_count > 0) {
        return false;
    }

    lru_remove(bpm, frame_id);
    page_table_remove(bpm, page_id);

    frame->page_id = INVALID_PAGE_ID;
    frame->pin_count = 0;
    frame->is_dirty = false;
    memset(frame->data, 0, PAGE_SIZE);

    bpm->free_list[bpm->free_count++] = frame_id;
    return true;
}

bool bpm_flush_page(buffer_pool_manager_t* bpm, page_id_t page_id) {
    if (!bpm || page_id == INVALID_PAGE_ID) {
        return false;
    }

    frame_id_t frame_id = page_table_find(bpm, page_id);
    if (frame_id == INVALID_FRAME_ID) {
        return false;
    }

    frame_t* frame = &bpm->frames[frame_id];
    if (frame->is_dirty) {
        disk_manager_write_page(bpm->dm, frame->page_id, frame->data);
        frame->is_dirty = false;
    }

    return true;
}

void bpm_flush_all(buffer_pool_manager_t* bpm) {
    if (!bpm || !bpm->frames || !bpm->dm) return;

    for (size_t i = 0; i < bpm->pool_size; i++) {
        frame_t* frame = &bpm->frames[i];
        if (frame->page_id != INVALID_PAGE_ID && frame->is_dirty) {
            disk_manager_write_page(bpm->dm, frame->page_id, frame->data);
            frame->is_dirty = false;
        }
    }
}

double bpm_get_hit_rate(const buffer_pool_manager_t* bpm) {
    if (!bpm || bpm->num_requests == 0) {
        return 0.0;
    }
    return ((double)bpm->num_hits / (double)bpm->num_requests) * 100.0;
}

void bpm_print_stats(const buffer_pool_manager_t* bpm) {
    if (!bpm) return;

    printf("+---------------------------------------+\n");
    printf("|       Buffer Pool Manager Stats       |\n");
    printf("+---------------------------------------+\n");
    printf("| Pool Size    : %-22zu |\n", bpm->pool_size);
    printf("| Requests     : %-22llu |\n", (unsigned long long)bpm->num_requests);
    printf("| Cache Hits   : %-22llu |\n", (unsigned long long)bpm->num_hits);
    printf("| Cache Misses : %-22llu |\n", (unsigned long long)bpm->num_misses);
    printf("| Hit Rate     : %-20.2f %% |\n", bpm_get_hit_rate(bpm));
    printf("| Disk Reads   : %-22llu |\n", (unsigned long long)disk_manager_get_num_reads(bpm->dm));
    printf("| Disk Writes  : %-22llu |\n", (unsigned long long)disk_manager_get_num_writes(bpm->dm));
    printf("+---------------------------------------+\n");
}

void bpm_reset_stats(buffer_pool_manager_t* bpm) {
    if (bpm) {
        bpm->num_requests = 0;
        bpm->num_hits = 0;
        bpm->num_misses = 0;
        disk_manager_reset_stats(bpm->dm);
    }
}

