#ifndef MINI_SQL_BPLUS_TREE_H
#define MINI_SQL_BPLUS_TREE_H

#include "types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define BPLUS_MAX_KEYS 8
#define BPLUS_MIN_KEYS (BPLUS_MAX_KEYS / 2)

typedef struct bplus_node_t {
    bool is_leaf;
    int num_keys;
    int64_t keys[BPLUS_MAX_KEYS + 1]; /* +1 extra slot for temporary overflow before split */
    struct bplus_node_t* parent;

    union {
        /* Leaf node fields */
        struct {
            rid_t rids[BPLUS_MAX_KEYS + 1];
            struct bplus_node_t* next;
            struct bplus_node_t* prev;
        } leaf;

        /* Internal node fields */
        struct {
            struct bplus_node_t* children[BPLUS_MAX_KEYS + 2];
        } internal;
    };
} bplus_node_t;

typedef struct {
    bplus_node_t* root;
    size_t size;
    int depth;
} bplus_tree_t;

/* B+ Tree Lifecycle */
bplus_tree_t* bplus_tree_create(void);
void bplus_tree_destroy(bplus_tree_t* tree);

/* B+ Tree Search */
bool bplus_tree_search(const bplus_tree_t* tree, int64_t key, rid_t* out_rid);
size_t bplus_tree_range_scan(const bplus_tree_t* tree, int64_t min_key, int64_t max_key, rid_t* out_rids, size_t max_results);

/* B+ Tree Mutation */
bool bplus_tree_insert(bplus_tree_t* tree, int64_t key, rid_t rid);
bool bplus_tree_delete(bplus_tree_t* tree, int64_t key, rid_t rid);

/* Inspection and Diagnostics */
void bplus_tree_print(const bplus_tree_t* tree);
size_t bplus_tree_get_size(const bplus_tree_t* tree);

#endif /* MINI_SQL_BPLUS_TREE_H */

