#include "../include/bplus_tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bplus_node_t* bplus_node_create(bool is_leaf) {
    bplus_node_t* node = (bplus_node_t*)calloc(1, sizeof(bplus_node_t));
    if (!node) return NULL;
    node->is_leaf = is_leaf;
    node->num_keys = 0;
    node->parent = NULL;
    if (is_leaf) {
        node->leaf.next = NULL;
        node->leaf.prev = NULL;
    }
    return node;
}

static void bplus_node_destroy_recursive(bplus_node_t* node) {
    if (!node) return;
    if (!node->is_leaf) {
        for (int i = 0; i <= node->num_keys; i++) {
            bplus_node_destroy_recursive(node->internal.children[i]);
        }
    }
    free(node);
}

bplus_tree_t* bplus_tree_create(void) {
    bplus_tree_t* tree = (bplus_tree_t*)calloc(1, sizeof(bplus_tree_t));
    if (!tree) return NULL;
    tree->root = NULL; 
    tree->size = 0;
    tree->depth = 0;
    return tree;
}

void bplus_tree_destroy(bplus_tree_t* tree) {
    if (!tree) return;
    bplus_node_destroy_recursive(tree->root);
    free(tree);
}

static bplus_node_t* find_first_leaf(bplus_node_t* root, int64_t key) {
    if (!root) return NULL;
    bplus_node_t* curr = root;
    while (!curr->is_leaf) {
        int idx = 0;
        while (idx < curr->num_keys && key > curr->keys[idx]) {
            idx++;
        }
        curr = curr->internal.children[idx];
        if (!curr) return NULL;
    }
    return curr;
}

static bplus_node_t* find_leaf(bplus_node_t* root, int64_t key) {
    if (!root) return NULL;
    bplus_node_t* curr = root;
    while (!curr->is_leaf) {
        int idx = 0;
        while (idx < curr->num_keys && key >= curr->keys[idx]) {
            idx++;
        }
        curr = curr->internal.children[idx];
        if (!curr) return NULL;
    }
    return curr;
}

bool bplus_tree_search(const bplus_tree_t* tree, int64_t key, rid_t* out_rid) {
    if (!tree || !tree->root || !out_rid) {
        return false;
    }

    bplus_node_t* leaf = find_first_leaf(tree->root, key);
    if (!leaf) return false;

    bplus_node_t* curr = leaf;
    while (curr) {
        for (int i = 0; i < curr->num_keys; i++) {
            if (curr->keys[i] == key) {
                *out_rid = curr->leaf.rids[i];
                return true;
            }
            if (curr->keys[i] > key) {
                return false;
            }
        }
        curr = curr->leaf.next;
    }

    return false;
}

size_t bplus_tree_find_all(const bplus_tree_t* tree, int64_t key, rid_t** out_rids) {
    if (!tree || !tree->root || !out_rids) {
        if (out_rids) *out_rids = NULL;
        return 0;
    }

    bplus_node_t* leaf = find_first_leaf(tree->root, key);
    if (!leaf) {
        *out_rids = NULL;
        return 0;
    }

    size_t capacity = 16;
    size_t count = 0;
    rid_t* results = (rid_t*)malloc(capacity * sizeof(rid_t));
    if (!results) {
        *out_rids = NULL;
        return 0;
    }

    bplus_node_t* curr = leaf;
    while (curr) {
        for (int i = 0; i < curr->num_keys; i++) {
            if (curr->keys[i] == key) {
                if (count >= capacity) {
                    capacity *= 2;
                    rid_t* new_res = (rid_t*)realloc(results, capacity * sizeof(rid_t));
                    if (!new_res) {
                        free(results);
                        *out_rids = NULL;
                        return 0;
                    }
                    results = new_res;
                }
                results[count++] = curr->leaf.rids[i];
            } else if (curr->keys[i] > key) {
                goto done;
            }
        }
        curr = curr->leaf.next;
    }

done:
    if (count == 0) {
        free(results);
        *out_rids = NULL;
        return 0;
    }

    *out_rids = results;
    return count;
}

size_t bplus_tree_range_scan(const bplus_tree_t* tree, int64_t min_key, int64_t max_key, rid_t* out_rids, size_t max_results) {
    if (!tree || !tree->root || !out_rids || max_results == 0) {
        return 0;
    }

    bplus_node_t* leaf = find_first_leaf(tree->root, min_key);
    if (!leaf) return 0;

    size_t count = 0;
    bplus_node_t* curr = leaf;

    while (curr && count < max_results) {
        for (int i = 0; i < curr->num_keys; i++) {
            if (curr->keys[i] > max_key) {
                return count;
            }
            if (curr->keys[i] >= min_key) {
                out_rids[count++] = curr->leaf.rids[i];
                if (count >= max_results) {
                    return count;
                }
            }
        }
        curr = curr->leaf.next;
    }

    return count;
}

static void insert_into_parent(bplus_tree_t* tree, bplus_node_t* left, int64_t key, bplus_node_t* right);

static void split_leaf(bplus_tree_t* tree, bplus_node_t* leaf) {
    bplus_node_t* new_leaf = bplus_node_create(true);
    int mid = leaf->num_keys / 2;
    int move_count = leaf->num_keys - mid;

    for (int i = 0; i < move_count; i++) {
        new_leaf->keys[i] = leaf->keys[mid + i];
        new_leaf->leaf.rids[i] = leaf->leaf.rids[mid + i];
    }
    new_leaf->num_keys = move_count;
    leaf->num_keys = mid;

    new_leaf->leaf.next = leaf->leaf.next;
    new_leaf->leaf.prev = leaf;
    if (leaf->leaf.next) {
        leaf->leaf.next->leaf.prev = new_leaf;
    }
    leaf->leaf.next = new_leaf;
    new_leaf->parent = leaf->parent;

    insert_into_parent(tree, leaf, new_leaf->keys[0], new_leaf);
}

static void split_internal(bplus_tree_t* tree, bplus_node_t* node) {
    bplus_node_t* new_node = bplus_node_create(false);
    int mid = node->num_keys / 2;
    int64_t up_key = node->keys[mid];

    int move_keys = node->num_keys - (mid + 1);
    for (int i = 0; i < move_keys; i++) {
        new_node->keys[i] = node->keys[mid + 1 + i];
    }

    int move_children = move_keys + 1;
    for (int i = 0; i < move_children; i++) {
        new_node->internal.children[i] = node->internal.children[mid + 1 + i];
        if (new_node->internal.children[i]) {
            new_node->internal.children[i]->parent = new_node;
        }
    }

    new_node->num_keys = move_keys;
    node->num_keys = mid;
    new_node->parent = node->parent;

    insert_into_parent(tree, node, up_key, new_node);
}

static void insert_into_parent(bplus_tree_t* tree, bplus_node_t* left, int64_t key, bplus_node_t* right) {
    bplus_node_t* parent = left->parent;

    if (!parent) {
        /* Create new root */
        bplus_node_t* new_root = bplus_node_create(false);
        new_root->keys[0] = key;
        new_root->internal.children[0] = left;
        new_root->internal.children[1] = right;
        new_root->num_keys = 1;
        left->parent = new_root;
        right->parent = new_root;
        tree->root = new_root;
        tree->depth++;
        return;
    }

    /* Find left child's index in parent */
    int idx = 0;
    while (idx <= parent->num_keys && parent->internal.children[idx] != left) {
        idx++;
    }

    for (int i = parent->num_keys; i > idx; i--) {
        parent->keys[i] = parent->keys[i - 1];
        parent->internal.children[i + 1] = parent->internal.children[i];
    }

    parent->keys[idx] = key;
    parent->internal.children[idx + 1] = right;
    right->parent = parent;
    parent->num_keys++;

    if (parent->num_keys > BPLUS_MAX_KEYS) {
        split_internal(tree, parent);
    }
}

bool bplus_tree_insert(bplus_tree_t* tree, int64_t key, rid_t rid) {
    if (!tree) return false;

    if (!tree->root) {
        bplus_node_t* root = bplus_node_create(true);
        root->keys[0] = key;
        root->leaf.rids[0] = rid;
        root->num_keys = 1;
        tree->root = root;
        tree->size = 1;
        tree->depth = 1;
        return true;
    }

    bplus_node_t* leaf = find_leaf(tree->root, key);

    /* Insert (key, rid) into leaf in sorted order */
    int idx = 0;
    while (idx < leaf->num_keys && leaf->keys[idx] <= key) {
        idx++;
    }

    for (int i = leaf->num_keys; i > idx; i--) {
        leaf->keys[i] = leaf->keys[i - 1];
        leaf->leaf.rids[i] = leaf->leaf.rids[i - 1];
    }

    leaf->keys[idx] = key;
    leaf->leaf.rids[idx] = rid;
    leaf->num_keys++;
    tree->size++;

    if (leaf->num_keys > BPLUS_MAX_KEYS) {
        split_leaf(tree, leaf);
    }

    return true;
}

static void fix_internal_underflow(bplus_tree_t* tree, bplus_node_t* node);

static void fix_leaf_underflow(bplus_tree_t* tree, bplus_node_t* leaf) {
    bplus_node_t* parent = leaf->parent;
    if (!parent) {
        return;
    }

    int child_idx = 0;
    while (child_idx <= parent->num_keys && parent->internal.children[child_idx] != leaf) {
        child_idx++;
    }

    /* Try borrowing from left sibling */
    if (child_idx > 0) {
        bplus_node_t* left_sib = parent->internal.children[child_idx - 1];
        if (left_sib->num_keys > BPLUS_MIN_KEYS) {
            for (int i = leaf->num_keys; i > 0; i--) {
                leaf->keys[i] = leaf->keys[i - 1];
                leaf->leaf.rids[i] = leaf->leaf.rids[i - 1];
            }
            leaf->keys[0] = left_sib->keys[left_sib->num_keys - 1];
            leaf->leaf.rids[0] = left_sib->leaf.rids[left_sib->num_keys - 1];
            leaf->num_keys++;
            left_sib->num_keys--;
            parent->keys[child_idx - 1] = leaf->keys[0];
            return;
        }
    }

    /* Try borrowing from right sibling */
    if (child_idx < parent->num_keys) {
        bplus_node_t* right_sib = parent->internal.children[child_idx + 1];
        if (right_sib->num_keys > BPLUS_MIN_KEYS) {
            leaf->keys[leaf->num_keys] = right_sib->keys[0];
            leaf->leaf.rids[leaf->num_keys] = right_sib->leaf.rids[0];
            leaf->num_keys++;

            for (int i = 0; i < right_sib->num_keys - 1; i++) {
                right_sib->keys[i] = right_sib->keys[i + 1];
                right_sib->leaf.rids[i] = right_sib->leaf.rids[i + 1];
            }
            right_sib->num_keys--;
            parent->keys[child_idx] = right_sib->keys[0];
            return;
        }
    }

    /* Merging with left or right sibling */
    if (child_idx > 0) {
        /* Merge leaf into left_sib */
        bplus_node_t* left_sib = parent->internal.children[child_idx - 1];
        for (int i = 0; i < leaf->num_keys; i++) {
            left_sib->keys[left_sib->num_keys + i] = leaf->keys[i];
            left_sib->leaf.rids[left_sib->num_keys + i] = leaf->leaf.rids[i];
        }
        left_sib->num_keys += leaf->num_keys;

        left_sib->leaf.next = leaf->leaf.next;
        if (leaf->leaf.next) {
            leaf->leaf.next->leaf.prev = left_sib;
        }

        /* Remove separator key in parent */
        for (int i = child_idx - 1; i < parent->num_keys - 1; i++) {
            parent->keys[i] = parent->keys[i + 1];
            parent->internal.children[i + 1] = parent->internal.children[i + 2];
        }
        parent->num_keys--;
        free(leaf);

        if (parent->num_keys < BPLUS_MIN_KEYS) {
            fix_internal_underflow(tree, parent);
        }
    } else {
        /* Merge right_sib into leaf */
        bplus_node_t* right_sib = parent->internal.children[child_idx + 1];
        for (int i = 0; i < right_sib->num_keys; i++) {
            leaf->keys[leaf->num_keys + i] = right_sib->keys[i];
            leaf->leaf.rids[leaf->num_keys + i] = right_sib->leaf.rids[i];
        }
        leaf->num_keys += right_sib->num_keys;

        leaf->leaf.next = right_sib->leaf.next;
        if (right_sib->leaf.next) {
            right_sib->leaf.next->leaf.prev = leaf;
        }

        for (int i = child_idx; i < parent->num_keys - 1; i++) {
            parent->keys[i] = parent->keys[i + 1];
            parent->internal.children[i + 1] = parent->internal.children[i + 2];
        }
        parent->num_keys--;
        free(right_sib);

        if (parent->num_keys < BPLUS_MIN_KEYS) {
            fix_internal_underflow(tree, parent);
        }
    }
}

static void fix_internal_underflow(bplus_tree_t* tree, bplus_node_t* node) {
    if (node == tree->root) {
        if (node->num_keys == 0) {
            tree->root = node->internal.children[0];
            if (tree->root) {
                tree->root->parent = NULL;
            }
            free(node);
            tree->depth--;
        }
        return;
    }

    bplus_node_t* parent = node->parent;
    int child_idx = 0;
    while (child_idx <= parent->num_keys && parent->internal.children[child_idx] != node) {
        child_idx++;
    }

    /* Try borrowing from left sibling */
    if (child_idx > 0) {
        bplus_node_t* left_sib = parent->internal.children[child_idx - 1];
        if (left_sib->num_keys > BPLUS_MIN_KEYS) {
            for (int i = node->num_keys; i > 0; i--) {
                node->keys[i] = node->keys[i - 1];
                node->internal.children[i + 1] = node->internal.children[i];
            }
            node->internal.children[1] = node->internal.children[0];

            node->keys[0] = parent->keys[child_idx - 1];
            node->internal.children[0] = left_sib->internal.children[left_sib->num_keys];
            if (node->internal.children[0]) {
                node->internal.children[0]->parent = node;
            }

            parent->keys[child_idx - 1] = left_sib->keys[left_sib->num_keys - 1];
            node->num_keys++;
            left_sib->num_keys--;
            return;
        }
    }

    /* Try borrowing from right sibling */
    if (child_idx < parent->num_keys) {
        bplus_node_t* right_sib = parent->internal.children[child_idx + 1];
        if (right_sib->num_keys > BPLUS_MIN_KEYS) {
            node->keys[node->num_keys] = parent->keys[child_idx];
            node->internal.children[node->num_keys + 1] = right_sib->internal.children[0];
            if (node->internal.children[node->num_keys + 1]) {
                node->internal.children[node->num_keys + 1]->parent = node;
            }

            parent->keys[child_idx] = right_sib->keys[0];
            node->num_keys++;

            for (int i = 0; i < right_sib->num_keys - 1; i++) {
                right_sib->keys[i] = right_sib->keys[i + 1];
                right_sib->internal.children[i] = right_sib->internal.children[i + 1];
            }
            right_sib->internal.children[right_sib->num_keys - 1] = right_sib->internal.children[right_sib->num_keys];
            right_sib->num_keys--;
            return;
        }
    }

    /* Merge internal nodes */
    if (child_idx > 0) {
        bplus_node_t* left_sib = parent->internal.children[child_idx - 1];
        left_sib->keys[left_sib->num_keys] = parent->keys[child_idx - 1];
        int start = left_sib->num_keys + 1;

        for (int i = 0; i < node->num_keys; i++) {
            left_sib->keys[start + i] = node->keys[i];
            left_sib->internal.children[start + i] = node->internal.children[i];
            if (left_sib->internal.children[start + i]) {
                left_sib->internal.children[start + i]->parent = left_sib;
            }
        }
        left_sib->internal.children[start + node->num_keys] = node->internal.children[node->num_keys];
        if (left_sib->internal.children[start + node->num_keys]) {
            left_sib->internal.children[start + node->num_keys]->parent = left_sib;
        }

        left_sib->num_keys += 1 + node->num_keys;

        for (int i = child_idx - 1; i < parent->num_keys - 1; i++) {
            parent->keys[i] = parent->keys[i + 1];
            parent->internal.children[i + 1] = parent->internal.children[i + 2];
        }
        parent->num_keys--;
        free(node);

        if (parent->num_keys < BPLUS_MIN_KEYS) {
            fix_internal_underflow(tree, parent);
        }
    } else {
        bplus_node_t* right_sib = parent->internal.children[child_idx + 1];
        node->keys[node->num_keys] = parent->keys[child_idx];
        int start = node->num_keys + 1;

        for (int i = 0; i < right_sib->num_keys; i++) {
            node->keys[start + i] = right_sib->keys[i];
            node->internal.children[start + i] = right_sib->internal.children[i];
            if (node->internal.children[start + i]) {
                node->internal.children[start + i]->parent = node;
            }
        }
        node->internal.children[start + right_sib->num_keys] = right_sib->internal.children[right_sib->num_keys];
        if (node->internal.children[start + right_sib->num_keys]) {
            node->internal.children[start + right_sib->num_keys]->parent = node;
        }

        node->num_keys += 1 + right_sib->num_keys;

        for (int i = child_idx; i < parent->num_keys - 1; i++) {
            parent->keys[i] = parent->keys[i + 1];
            parent->internal.children[i + 1] = parent->internal.children[i + 2];
        }
        parent->num_keys--;
        free(right_sib);

        if (parent->num_keys < BPLUS_MIN_KEYS) {
            fix_internal_underflow(tree, parent);
        }
    }
}

bool bplus_tree_delete(bplus_tree_t* tree, int64_t key, rid_t rid) {
    if (!tree || !tree->root) return false;

    bplus_node_t* leaf = find_first_leaf(tree->root, key);
    if (!leaf) return false;

    int idx = -1;
    bplus_node_t* target_leaf = NULL;
    bplus_node_t* curr = leaf;
    while (curr) {
        for (int i = 0; i < curr->num_keys; i++) {
            if (curr->keys[i] == key &&
                curr->leaf.rids[i].page_id == rid.page_id &&
                curr->leaf.rids[i].slot_num == rid.slot_num) {
                idx = i;
                target_leaf = curr;
                goto found;
            } else if (curr->keys[i] > key) {
                goto found;
            }
        }
        curr = curr->leaf.next;
    }
found:
    if (idx == -1 || !target_leaf) return false;

    for (int i = idx; i < target_leaf->num_keys - 1; i++) {
        target_leaf->keys[i] = target_leaf->keys[i + 1];
        target_leaf->leaf.rids[i] = target_leaf->leaf.rids[i + 1];
    }
    target_leaf->num_keys--;
    tree->size--;

    if (target_leaf == tree->root) {
        if (target_leaf->num_keys == 0) {
            free(target_leaf);
            tree->root = NULL;
            tree->size = 0;
            tree->depth = 0;
        }
        return true;
    }

    if (target_leaf->num_keys < BPLUS_MIN_KEYS) {
        fix_leaf_underflow(tree, target_leaf);
    }

    return true;
}

static void print_node_recursive(const bplus_node_t* node, int indent) {
    if (!node) return;
    for (int i = 0; i < indent; i++) printf("  ");

    if (node->is_leaf) {
        printf("[Leaf (%d keys)]: ", node->num_keys);
        for (int i = 0; i < node->num_keys; i++) {
            printf("%lld->(%u,%u) ", (long long)node->keys[i], node->leaf.rids[i].page_id, node->leaf.rids[i].slot_num);
        }
        printf("\n");
    } else {
        printf("[Internal (%d keys)]: ", node->num_keys);
        for (int i = 0; i < node->num_keys; i++) {
            printf("%lld ", (long long)node->keys[i]);
        }
        printf("\n");
        for (int i = 0; i <= node->num_keys; i++) {
            print_node_recursive(node->internal.children[i], indent + 1);
        }
    }
}

void bplus_tree_print(const bplus_tree_t* tree) {
    if (!tree || !tree->root) {
        printf("(B+ Tree is empty)\n");
        return;
    }
    printf("B+ Tree (Depth: %d, Entries: %zu):\n", tree->depth, tree->size);
    print_node_recursive(tree->root, 0);
}

size_t bplus_tree_get_size(const bplus_tree_t* tree) {
    return tree ? tree->size : 0;
}
