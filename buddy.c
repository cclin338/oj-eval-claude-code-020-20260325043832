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

// Free lists for each rank
typedef struct FreeBlock {
    struct FreeBlock *next;
} FreeBlock;

static FreeBlock *free_lists[MAX_RANK + 1];  // Index 1..MAX_RANK

// Track allocated blocks to validate returns
#define MAX_ALLOCATIONS 100000
static struct {
    void *addr;
    int rank;
} allocations[MAX_ALLOCATIONS];
static int alloc_count = 0;

// Helper functions
static int get_block_size(int rank) {
    return PAGE_SIZE * (1 << (rank - 1));
}

static int get_pages_in_block(int rank) {
    return 1 << (rank - 1);
}

static void add_to_free_list(int rank, void *block) {
    FreeBlock *fb = (FreeBlock *)block;
    fb->next = free_lists[rank];
    free_lists[rank] = fb;
}

static void *remove_from_free_list(int rank) {
    if (!free_lists[rank]) return NULL;
    FreeBlock *fb = free_lists[rank];
    free_lists[rank] = fb->next;
    return (void *)fb;
}

static int get_page_index(void *addr) {
    if (!memory_start || addr < memory_start) return -1;
    uintptr_t offset = (uintptr_t)addr - (uintptr_t)memory_start;
    if (offset % PAGE_SIZE != 0) return -1;
    return offset / PAGE_SIZE;
}

static void *split_and_allocate(int rank) {
    // Find smallest free block with rank >= target
    for (int r = rank; r <= max_rank; r++) {
        if (free_lists[r]) {
            // Found block to split
            void *block = remove_from_free_list(r);

            // Split down to target rank
            while (r > rank) {
                // Split into two buddies
                int block_size = get_block_size(r);
                void *buddy = (char *)block + block_size / 2;

                // Add buddy to free list at rank-1
                add_to_free_list(r - 1, buddy);

                // Continue with first half
                r--;
            }

            return block;
        }
    }

    return NULL;
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

    // Initialize free lists
    for (int i = 1; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
    }

    // Initialize allocations
    alloc_count = 0;

    // Add entire memory to free list at max rank
    add_to_free_list(max_rank, p);

    return OK;
}

// Allocate pages of specified rank
void *alloc_pages(int rank) {
    if (rank < MIN_RANK || rank > MAX_RANK) {
        return ERR_PTR(-EINVAL);
    }

    if (!memory_start) {
        return ERR_PTR(-EINVAL);
    }

    void *block = NULL;

    // Try exact match first
    if (free_lists[rank]) {
        block = remove_from_free_list(rank);
    } else {
        // Try to split larger block
        block = split_and_allocate(rank);
    }

    if (!block) {
        return ERR_PTR(-ENOSPC);
    }

    // Record allocation
    if (alloc_count < MAX_ALLOCATIONS) {
        allocations[alloc_count].addr = block;
        allocations[alloc_count].rank = rank;
        alloc_count++;
    }

    return block;
}

// Release pages back to buddy system
int return_pages(void *p) {
    if (!p || !memory_start) return -EINVAL;

    // Find this allocation
    int idx = -1;
    for (int i = 0; i < alloc_count; i++) {
        if (allocations[i].addr == p) {
            idx = i;
            break;
        }
    }

    if (idx == -1) return -EINVAL;  // Not found

    int rank = allocations[idx].rank;

    // Remove from allocations array
    allocations[idx] = allocations[alloc_count - 1];
    alloc_count--;

    // Add to free list
    add_to_free_list(rank, p);

    // Try to merge with buddy
    while (rank < max_rank) {
        // Calculate buddy address
        int block_size = get_block_size(rank);
        uintptr_t block_offset = (uintptr_t)p - (uintptr_t)memory_start;
        uintptr_t buddy_offset = block_offset ^ block_size;
        void *buddy = (char *)memory_start + buddy_offset;

        // Check if buddy is in free list at same rank
        int buddy_found = 0;
        FreeBlock *prev = NULL;
        FreeBlock *curr = free_lists[rank];

        while (curr) {
            if ((void *)curr == buddy) {
                buddy_found = 1;
                // Remove buddy from free list
                if (prev) {
                    prev->next = curr->next;
                } else {
                    free_lists[rank] = curr->next;
                }
                break;
            }
            prev = curr;
            curr = curr->next;
        }

        if (!buddy_found) {
            break;  // Buddy not free, stop merging
        }

        // Remove current block from free list
        prev = NULL;
        curr = free_lists[rank];
        while (curr) {
            if ((void *)curr == p) {
                if (prev) {
                    prev->next = curr->next;
                } else {
                    free_lists[rank] = curr->next;
                }
                break;
            }
            prev = curr;
            curr = curr->next;
        }

        // Merge blocks: take the lower address
        p = (block_offset < buddy_offset) ? p : buddy;
        rank++;

        // Add merged block to higher rank free list
        add_to_free_list(rank, p);
    }

    return OK;
}

// Query rank of a page
int query_ranks(void *p) {
    if (!p || !memory_start) return -EINVAL;

    uintptr_t offset = (uintptr_t)p - (uintptr_t)memory_start;
    if (offset % PAGE_SIZE != 0) return -EINVAL;

    int page_idx = offset / PAGE_SIZE;
    if (page_idx < 0 || page_idx >= total_pages) return -EINVAL;

    // Check if it's allocated
    for (int i = 0; i < alloc_count; i++) {
        if (allocations[i].addr == p) {
            return allocations[i].rank;
        }
    }

    // Not allocated, find the largest free block containing this address
    // Check each rank from max down to 1
    for (int rank = max_rank; rank >= 1; rank--) {
        int block_size = get_block_size(rank);
        int pages_in_block = get_pages_in_block(rank);

        // Check if address is aligned to block boundary
        if (offset % block_size != 0) continue;

        // Check if block starting at this address is free
        FreeBlock *curr = free_lists[rank];
        while (curr) {
            if ((void *)curr == p) {
                return rank;
            }
            curr = curr->next;
        }
    }

    // Address is inside a larger free block
    // Find which free block contains it
    for (int rank = max_rank; rank >= 1; rank--) {
        int block_size = get_block_size(rank);

        FreeBlock *curr = free_lists[rank];
        while (curr) {
            uintptr_t block_start = (uintptr_t)curr;
            uintptr_t block_end = block_start + block_size;

            if ((uintptr_t)p >= block_start && (uintptr_t)p < block_end) {
                // Address is inside this free block
                // The actual rank is the largest power of 2 block size
                // that fits within this block and contains the address
                // For a free block, any address within it has the same rank
                // as the block itself (the block is not split)
                return rank;
            }
            curr = curr->next;
        }
    }

    return -EINVAL;
}

// Query how many unallocated pages remain for specified rank
int query_page_counts(int rank) {
    if (rank < MIN_RANK || rank > MAX_RANK) return -EINVAL;

    if (!memory_start) return -EINVAL;

    int count = 0;
    FreeBlock *curr = free_lists[rank];
    while (curr) {
        count++;
        curr = curr->next;
    }

    return count;
}
