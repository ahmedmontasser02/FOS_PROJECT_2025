/*
 * dynamic_allocator.c
 *
 * Created on: Sep 21, 2023
 * Author: HP
 */
#include <inc/assert.h>
#include <inc/string.h>
#include "../inc/dynamic_allocator.h"
//#include "../kern/mem/kheap.h"

#define Phys_frames 65536 //(256*1024*1024)/PAGE_SIZE =256 mb / 4 kb
uint32 PA_2_VA[Phys_frames];
uint32* ptr_page_directory;
//uint32 *ptr_page_table = NULL;
//uint32 ret=get_page_table(ptr_page_directory, (uint32)0x00000000, &ptr_page_table);
//==================================================================================//
//============================== GIVEN FUNCTIONS ===================================//
//==================================================================================//

// [1] GET PAGE VA:
__inline__ uint32 to_page_va(struct PageInfoElement *ptrPageInfo)
{
    if (ptrPageInfo < &pageBlockInfoArr[0] || ptrPageInfo >= &pageBlockInfoArr[DYN_ALLOC_MAX_SIZE / PAGE_SIZE])
        panic("to_page_va called with invalid pageInfoPtr");

    int idxInPageInfoArr = (ptrPageInfo - pageBlockInfoArr);
    return dynAllocStart + (idxInPageInfoArr << PGSHIFT);
}

// [2] GET PAGE INFO OF PAGE VA:
__inline__ struct PageInfoElement *to_page_info(uint32 va)
{
    int idxInPageInfoArr = (va - dynAllocStart) >> PGSHIFT;
    if (idxInPageInfoArr < 0 || idxInPageInfoArr >= DYN_ALLOC_MAX_SIZE / PAGE_SIZE)
        panic("to_page_info called with invalid pa");
    return &pageBlockInfoArr[idxInPageInfoArr];
}

//==================================================================================//
//============================== HELPER FUNCTIONS ==================================//
//==================================================================================//

uint32 nearest_power_of_two(uint32 size)
{
    if (size == 0)
        return 0;
    if (size <= DYN_ALLOC_MIN_BLOCK_SIZE)
        return DYN_ALLOC_MIN_BLOCK_SIZE;

    uint32 p = DYN_ALLOC_MIN_BLOCK_SIZE;
    while (p < size)
    {
        p <<= 1;
    }
    if (p > DYN_ALLOC_MAX_BLOCK_SIZE)
        return DYN_ALLOC_MAX_BLOCK_SIZE + 1;

    return p;
}

int get_log2(uint32 n)
{
    if (n == 0)
        return -1;
    int log2_val = 0;
    while (n > 1)
    {
        n >>= 1;
        log2_val++;
    }
    return log2_val;
}

//==================================================================================//
//============================ REQUIRED FUNCTIONS ==================================//
//==================================================================================//

//==================================
// [1] INITIALIZE DYNAMIC ALLOCATOR:
//==================================
bool is_initialized = 0;
void initialize_dynamic_allocator(uint32 daStart, uint32 daEnd)
{
    {
        assert(daEnd <= daStart + DYN_ALLOC_MAX_SIZE);
        is_initialized = 1;
    }

    dynAllocStart = daStart;
    dynAllocEnd = daEnd;

    int num_of_lists = LOG2_MAX_SIZE - LOG2_MIN_SIZE + 1;
    for (int i = 0; i < num_of_lists; i++)
    {
        LIST_INIT(&freeBlockLists[i]);
    }

    LIST_INIT(&freePagesList);

    // Calculate the EXACT number of pages in the given range [daStart, daEnd]
    // We must NOT initialize all 32MB worth of pages if daEnd is smaller.
    int num_of_pages = (daEnd - daStart) / PAGE_SIZE;
    // Initialize only the required number of PageInfoElements
    for (int i = 0; i < num_of_pages; i++)
    {
        pageBlockInfoArr[i].block_size = 0;
        pageBlockInfoArr[i].num_of_free_blocks = 0;
        LIST_INSERT_TAIL(&freePagesList, &pageBlockInfoArr[i]);
    }
}

//===========================
// [2] GET BLOCK SIZE:
//===========================
__inline__ uint32 get_block_size(void *va)
{
    struct PageInfoElement *ptrPageInfo = to_page_info((uint32)va);
    return ptrPageInfo->block_size;
}

//===========================
// [3] ALLOCATE BLOCK:
//===========================
void *alloc_block(uint32 size)
{
    //==================================================================================
    // DON'T CHANGE THESE LINES==========================================================
    //==================================================================================
    {
        assert(size <= DYN_ALLOC_MAX_BLOCK_SIZE);
    }
    //==================================================================================
    //==================================================================================

    if (size == 0)
        return NULL;

    uint32 block_size = nearest_power_of_two(size);
    if (block_size > DYN_ALLOC_MAX_BLOCK_SIZE)
        return NULL;

    int list_index = get_log2(block_size) - LOG2_MIN_SIZE;
    struct BlockElement *ptrBlock = NULL;

    //=========================================================================
    // CASE 1: Exact Match in Free Block Lists
    //=========================================================================
    if (!LIST_EMPTY(&freeBlockLists[list_index]))
    {
        ptrBlock = LIST_FIRST(&freeBlockLists[list_index]);
        LIST_REMOVE(&freeBlockLists[list_index], ptrBlock);

        struct PageInfoElement *ptrPageInfo = to_page_info((uint32)ptrBlock);
        ptrPageInfo->num_of_free_blocks--;

        return (void *)ptrBlock;
    }

    //=========================================================================
    // CASE 2: Allocate New Page (if no exact match)
    //=========================================================================
    if (!LIST_EMPTY(&freePagesList))
    {
        struct PageInfoElement *ptrPageInfo = LIST_FIRST(&freePagesList);
        LIST_REMOVE(&freePagesList, ptrPageInfo);

        uint32 page_va = to_page_va(ptrPageInfo);

        if (get_page((void *)page_va) < 0)
        {
//        	/* ==== ÅÖÇÝÉ reverse mapping ÈÚÏ get_page ==== */
//        	{
//        	    uint32 *ptr_page_table = NULL;
//        	    /* ÇÌáÈ ÌÏæá ÇáÕÝÍÉ ááÜ page_va */
//
//        		get_page_table(ptr_page_directory, (uint32)page_va, &ptr_page_table);
//        	    /* ÇÞÑÃ entry */
//        	    uint32 page_entry = ptr_page_table[PTX((uint32)page_va)];
//        	    uint32 framenumber = page_entry >> 12;
//        	    if (framenumber < Phys_frames) {
//        	        PA_2_VA[framenumber] = (uint32)page_va;
//        	    }
//        	}
//        	/* ==== äåÇíÉ ÇáÅÖÇÝÉ ==== */
            LIST_INSERT_HEAD(&freePagesList, ptrPageInfo);
            return NULL;
        }

        ptrPageInfo->block_size = block_size;
        ptrPageInfo->num_of_free_blocks = PAGE_SIZE / block_size;

        // Split page into blocks and add to list.
        // Use LIST_INSERT_TAIL to maintain address order (Low VA -> High VA)
        // This ensures contiguous allocation logic preferred by tests
        uint32 page_end = page_va + PAGE_SIZE;
        for (uint32 va = page_va; va < page_end; va += block_size)
        {
            struct BlockElement *newBlock = (struct BlockElement *)va;
            LIST_INSERT_TAIL(&freeBlockLists[list_index], newBlock);
        }

        // Allocate the first block
        ptrBlock = LIST_FIRST(&freeBlockLists[list_index]);
        LIST_REMOVE(&freeBlockLists[list_index], ptrBlock);
        ptrPageInfo->num_of_free_blocks--;

        return (void *)ptrBlock;
    }

    //=========================================================================
    // CASE 3: Wasteful Allocation (Use larger block)
    //=========================================================================
    // We only reach here if Case 1 and Case 2 failed (i.e., lists empty AND no free pages)
    int num_of_lists = LOG2_MAX_SIZE - LOG2_MIN_SIZE + 1;
    for (int i = list_index + 1; i < num_of_lists; i++)
    {
        if (!LIST_EMPTY(&freeBlockLists[i]))
        {
            ptrBlock = LIST_FIRST(&freeBlockLists[i]);
            LIST_REMOVE(&freeBlockLists[i], ptrBlock);

            struct PageInfoElement *ptrPageInfo = to_page_info((uint32)ptrBlock);
            // Important: Do NOT change block_size of the PageInfo.
            // The page remains configured for the larger block size.
            ptrPageInfo->num_of_free_blocks--;

            return (void *)ptrBlock;
        }
    }

    return NULL;
}

//===========================
// [4] FREE BLOCK:
//===========================
void free_block(void *va)
{
    //==================================================================================
    // DON'T CHANGE THESE LINES==========================================================
    //==================================================================================
    {
        assert((uint32)va >= dynAllocStart && (uint32)va < dynAllocEnd);
    }
    //==================================================================================
    //==================================================================================
    if (va == NULL)
        return;
    if ((uint32)va < dynAllocStart || (uint32)va >= dynAllocEnd)
        return;

    struct PageInfoElement *ptrPageInfo = to_page_info((uint32)va);
    uint32 block_size = ptrPageInfo->block_size;

    // If block_size is 0, page is likely already free/uninitialized
    if (block_size == 0)
        return;

    int list_index = get_log2(block_size) - LOG2_MIN_SIZE;

    struct BlockElement *blk = (struct BlockElement *)va;
    // Insert at Head (standard free behavior)
    LIST_INSERT_HEAD(&freeBlockLists[list_index], blk);

    ptrPageInfo->num_of_free_blocks++;

    // Check if the entire page is now free
    uint32 max_blocks = PAGE_SIZE / block_size;
    if (ptrPageInfo->num_of_free_blocks == max_blocks)
    {
        uint32 page_start_va = to_page_va(ptrPageInfo);
        uint32 page_end_va = page_start_va + PAGE_SIZE;

        struct BlockElement *curr = LIST_FIRST(&freeBlockLists[list_index]);
        struct BlockElement *next_blk;

        // Remove all blocks belonging to this page from the free list
        while (curr != NULL)
        {
            next_blk = LIST_NEXT(curr);
            uint32 curr_va = (uint32)curr;

            if (curr_va >= page_start_va && curr_va < page_end_va)
            {
                LIST_REMOVE(&freeBlockLists[list_index], curr);
            }
            curr = next_blk;
        }


//        /* ==== ÅÒÇáÉ reverse mapping ÞÈá return_page ==== */
//        {
//            uint32 *ptr_page_table = NULL;
//            get_page_table(ptr_page_directory, (uint32)page_start_va, &ptr_page_table);
//            if (ptr_page_table != NULL) {
//                uint32 page_entry = ptr_page_table[PTX((uint32)page_start_va)];
//                uint32 framenumber = page_entry >> 12;
//                if (framenumber < Phys_frames) {
//                    PA_2_VA[framenumber] = 0;
//                }
//            }
//        }
//        /* ==== äåÇíÉ ÇáÅÖÇÝÉ ==== */

        // Return physical page
        return_page((void *)page_start_va);

        // Reset Info
        ptrPageInfo->block_size = 0;
        ptrPageInfo->num_of_free_blocks = 0;

        // Add PageInfo back to the free pages list
        LIST_INSERT_TAIL(&freePagesList, ptrPageInfo);
    }
}

//==================================================================================//
//============================== BONUS FUNCTIONS ===================================//
//==================================================================================//

//===========================
// [1] REALLOCATE BLOCK:
//===========================
void *realloc_block(void *va, uint32 new_size)
{
    if (va == NULL)
        return alloc_block(new_size);
    if (new_size == 0)
    {
        free_block(va);
        return NULL;
    }
    uint32 old_block_size = get_block_size(va);
    uint32 new_aligned_size = nearest_power_of_two(new_size);

    // Optimization: If new size fits in the existing block, return same pointer
    if (new_aligned_size <= old_block_size)
    {
        return va;
    }

    // Allocate new block
    void *new_va = alloc_block(new_size);
    if (new_va == NULL)
    {
        return NULL;
    }

    // Copy data
    memcpy(new_va, va, old_block_size);

    // Free old block
    free_block(va);

    return new_va;
}
