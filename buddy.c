#include "buddy.h"
#include <stdint.h>
#include <string.h>
#define NULL ((void *)0)

#define MAX_RANK 16
#define MIN_RANK 1
#define PAGE_SIZE 4096  // 4KB

// Data structures for buddy system
static void *memory_start = NULL;
static int total_pages = 0;
static int max_rank = 0;

// Binary tree for buddy system
// Tree nodes: 0 = free, 1 = allocated, 2 = split (partially allocated)
#define MAX_TREE_NODES (1 << 18)  // Enough for 2^17 pages
static unsigned char tree[MAX_TREE_NODES];

// Helper functions
static int get_block_size(int rank) {
    return PAGE_SIZE * (1 << (rank - 1));
}

static int get_pages_in_block(int rank) {
    return 1 << (rank - 1);
}

static int get_tree_index(int page_idx, int rank) {
    // Convert (page index, rank) to tree index
    // Tree is stored as heap: root at 1, children at 2i and 2i+1
    // Level 0: rank max_rank (1 node)
    // Level 1: rank max_rank-1 (2 nodes)
    // ...
    // Level max_rank-1: rank 1 (2^(max_rank-1) nodes)

    int level = max_rank - rank;  // 0 for max_rank, max_rank-1 for rank 1
    int nodes_at_level = 1 << level;
    int block_idx = page_idx / get_pages_in_block(rank);

    return nodes_at_level + block_idx;
}

static void mark_tree(int tree_idx, int status) {
    tree[tree_idx] = status;

    // Update parent nodes
    while (tree_idx > 1) {
        tree_idx >>= 1;
        int left = tree[tree_idx * 2];
        int right = tree[tree_idx * 2 + 1];

        if (left == 1 && right == 1) {
            tree[tree_idx] = 1;  // Both allocated
        } else if (left == 0 && right == 0) {
            tree[tree_idx] = 0;  // Both free
        } else {
            tree[tree_idx] = 2;  // Partially allocated
        }
    }
}

// Initialize the buddy system
int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0) return -EINVAL;

    memory_start = p;
    total_pages = pgcount;

    // Calculate maximum rank that can fit all pages
    max_rank = 0;
    int pages = pgcount;
    while (pages > 1) {
        pages >>= 1;
        max_rank++;
    }
    max_rank++;  // Convert to 1-based

    if (max_rank > MAX_RANK) {
        max_rank = MAX_RANK;
    }

    // Initialize tree (all free)
    memset(tree, 0, sizeof(tree));

    return OK;
}

// Recursive function to allocate block
static int allocate_from_node(int tree_idx, int target_level, int current_level, int *block_idx) {
    if (current_level == target_level) {
        // At target level
        if (tree[tree_idx] == 0) {  // Free
            *block_idx = tree_idx - (1 << current_level);
            return 1;  // Success
        }
        return 0;  // Not free
    }

    // Not at target level, need to go deeper
    if (tree[tree_idx] == 1) {  // Allocated
        return 0;  // Can't allocate here
    }

    // If free or split, try left child
    int result = allocate_from_node(tree_idx * 2, target_level, current_level + 1, block_idx);
    if (result) return 1;

    // Try right child
    return allocate_from_node(tree_idx * 2 + 1, target_level, current_level + 1, block_idx);
}

// Allocate pages of specified rank
void *alloc_pages(int rank) {
    if (rank < MIN_RANK || rank > MAX_RANK) {
        return ERR_PTR(-EINVAL);
    }

    if (!memory_start) {
        return ERR_PTR(-EINVAL);
    }

    int target_level = max_rank - rank;  // 0 for max_rank, max_rank-1 for rank 1
    int block_idx = -1;

    // Try to allocate starting from root
    if (allocate_from_node(1, target_level, 0, &block_idx)) {
        // Success, mark the node as allocated
        int tree_idx = (1 << target_level) + block_idx;
        mark_tree(tree_idx, 1);

        // Calculate address
        int pages_in_block = get_pages_in_block(rank);
        void *addr = (char *)memory_start + (block_idx * pages_in_block * PAGE_SIZE);
        return addr;
    }

    return ERR_PTR(-ENOSPC);
}

// Release pages back to buddy system
int return_pages(void *p) {
    if (!p || !memory_start) return -EINVAL;

    uintptr_t offset = (uintptr_t)p - (uintptr_t)memory_start;
    if (offset % PAGE_SIZE != 0) return -EINVAL;

    int page_idx = offset / PAGE_SIZE;
    if (page_idx < 0 || page_idx >= total_pages) return -EINVAL;

    // Find the block containing this address
    // Start from smallest rank (1) and go up
    for (int rank = 1; rank <= max_rank; rank++) {
        int pages_in_block = get_pages_in_block(rank);
        if (page_idx % pages_in_block != 0) continue;

        int tree_idx = get_tree_index(page_idx, rank);

        if (tree[tree_idx] == 1) {  // Allocated block
            // Mark as free
            mark_tree(tree_idx, 0);
            return OK;
        }
    }

    return -EINVAL;  // Not found or already free
}

// Query rank of a page
int query_ranks(void *p) {
    if (!p || !memory_start) return -EINVAL;

    uintptr_t offset = (uintptr_t)p - (uintptr_t)memory_start;
    if (offset % PAGE_SIZE != 0) return -EINVAL;

    int page_idx = offset / PAGE_SIZE;
    if (page_idx < 0 || page_idx >= total_pages) return -EINVAL;

    // Check each rank from 1 to max_rank
    for (int rank = 1; rank <= max_rank; rank++) {
        int pages_in_block = get_pages_in_block(rank);
        if (page_idx % pages_in_block != 0) continue;

        int tree_idx = get_tree_index(page_idx, rank);

        if (tree[tree_idx] == 1) {  // Allocated
            return rank;
        } else if (tree[tree_idx] == 0) {  // Free
            return rank;
        }
        // If 2 (split), continue to smaller rank
    }

    return -EINVAL;  // Should not reach here
}

// Query how many unallocated pages remain for specified rank
int query_page_counts(int rank) {
    if (rank < MIN_RANK || rank > MAX_RANK) return -EINVAL;

    if (!memory_start) return -EINVAL;

    int pages_in_block = get_pages_in_block(rank);
    int blocks_at_rank = total_pages / pages_in_block;
    int count = 0;

    for (int block_idx = 0; block_idx < blocks_at_rank; block_idx++) {
        int tree_idx = get_tree_index(block_idx * pages_in_block, rank);
        if (tree[tree_idx] == 0) {
            count++;
        }
    }

    return count;
}
