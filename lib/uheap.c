#include <inc/lib.h>

struct uspinlock *slock;
#define MAX_MALLOC_ALLOCS 4096

typedef struct
{
	uint32 addr;
	uint32 size;
} umalloc_alloc;

static umalloc_alloc umallocs[MAX_MALLOC_ALLOCS];

/* Save an allocation record into the table (first free slot). */
static void umalloc_save(uint32 addr, uint32 size)
{
	for (int i = 0; i < MAX_MALLOC_ALLOCS; i++)
	{
		if (umallocs[i].addr == 0)
		{
			umallocs[i].addr = addr;
			umallocs[i].size = size;
			return;
		}
	}
	/* if table is full we silently drop (or you can panic/log) */
}

/* Find an allocation record by addr; returns index or -1 if not found */
// static int umalloc_find(uint32 addr)
//{
//     for (int i = 0; i < MAX_MALLOC_ALLOCS; i++) {
//         if (umallocs[i].addr == addr) return i;
//     }
//     return -1;
// }

/* Remove allocation record at index */
static void umalloc_remove_index(int i)
{
	if (i < 0 || i >= MAX_MALLOC_ALLOCS)
		return;
	umallocs[i].addr = 0;
	umallocs[i].size = 0;
}

/* Try to shrink the page-allocator break if trailing pages are free */
static void shrink_break(void)
{
	/* We assume uheapPageAllocBreak and uheapPageAllocStart are globals visible here */
	uint32 *ptr_pt = NULL;

	while (uheapPageAllocBreak > uheapPageAllocStart)
	{
		uint32 lastPageVA = uheapPageAllocBreak - PAGE_SIZE;

		/* If the page table/frame exists for lastPageVA then it's reserved/allocated -> stop */
		/* We cannot call kernel get_frame_info here from user; instead we check our umalloc_table to see if any allocation covers lastPageVA */
		int isAlloc = 0;
		for (int i = 0; i < MAX_MALLOC_ALLOCS; i++)
		{
			if (umallocs[i].addr != 0)
			{
				uint32 a = umallocs[i].addr;
				uint32 s = umallocs[i].size;
				if (lastPageVA >= a && lastPageVA < (a + s))
				{
					isAlloc = 1;
					break;
				}
			}
		}
		if (isAlloc)
			break;

		/* no allocation covers last page -> move break down by one page */
		uheapPageAllocBreak -= PAGE_SIZE;
	}
}

//==================================================================================//
//============================== GIVEN FUNCTIONS ===================================//
//==================================================================================//

//==============================================
// [1] INITIALIZE USER HEAP:
//==============================================
int __firstTimeFlag = 1;
void uheap_init()
{
	if (__firstTimeFlag)
	{

		initialize_dynamic_allocator(USER_HEAP_START, USER_HEAP_START + DYN_ALLOC_MAX_SIZE);
		uheapPlaceStrategy = sys_get_uheap_strategy();
		uheapPageAllocStart = dynAllocEnd + PAGE_SIZE;
		uheapPageAllocBreak = uheapPageAllocStart;

		__firstTimeFlag = 0;
	}
}

//==============================================
// [2] GET A PAGE FROM THE KERNEL FOR DA:
//==============================================
int get_page(void *va)
{
	int ret = __sys_allocate_page(ROUNDDOWN(va, PAGE_SIZE), PERM_USER | PERM_WRITEABLE | PERM_UHPAGE);
	if (ret < 0)
		panic("get_page() in user: failed to allocate page from the kernel");
	return 0;
}

//==============================================
// [3] RETURN A PAGE FROM THE DA TO KERNEL:
//==============================================
void return_page(void *va)
{
	int ret = __sys_unmap_frame(ROUNDDOWN((uint32)va, PAGE_SIZE));
	if (ret < 0)
		panic("return_page() in user: failed to return a page to the kernel");
}

//==================================================================================//
//============================ REQUIRED FUNCTIONS ==================================//
//==================================================================================//

//=================================
// [1] ALLOCATE SPACE IN USER HEAP:
//=================================
void *malloc(uint32 size)
{
#if USE_KHEAP


	/* PAGE ALLOCATOR CASE (size > block limit)
	 * follow CUSTOM-FIT:
	 * 1) search for exact fit
	 * 2) if not found, choose worst fit (largest free contiguous chunk)
	 * 3) if not found, extend break if available
	 * 4) else return NULL
	 *
	 * The page allocator area: [uheapPageAllocStart, USER_HEAP_MAX)
	 * currently used: [uheapPageAllocStart, uheapPageAllocBreak)
	 * unused: [uheapPageAllocBreak, USER_HEAP_MAX)
	 *
	 * We do not actually allocate physical pages here  we mark the virtual range by
	 * calling sys_allocate_user_mem(va, bytes) which asks the kernel to mark the region
	 * with PERM_UHPAGE. Actual frames/pagefile pages will be created on page faults.
	 */

	//==============================================================
	// DON'T CHANGE THIS CODE========================================
	uheap_init();
	if (size == 0)
		return NULL;
	//==============================================================
	// TODO: [PROJECT'25.IM#2] USER HEAP - #1 malloc
	// Your code is here

	//	block allocator
	if (size <= DYN_ALLOC_MAX_BLOCK_SIZE)
	{

		return alloc_block(size);
	}

	uint32 bytes = ROUNDUP(size, PAGE_SIZE);
	uint32 numPages = bytes / PAGE_SIZE;

	uint32 hStart = uheapPageAllocStart;
	uint32 hEnd = uheapPageAllocBreak;

	uint32 currentStartAddr = 0;
	uint32 currentNumPages = 0;

	uint32 exactStartAddr = 0;
	uint32 worstStartAddr = 0;
	uint32 worstNumPages = 0;

	/* scan the used area for free runs (holes) by consulting our umalloc_table.
	   We treat any ranges recorded in umalloc_table as reserved. Everything else inside
	   [hStart, hEnd) is considered free. This mirrors kernel approach where get_frame_info
	   is used to detect used pages. */
	uint32 va = hStart;
	while (va < hEnd)
	{
		/* check whether this page is inside any allocation entry */
		int alloc = 0;

		for (int i = 0; i < MAX_MALLOC_ALLOCS; i++)
		{
			if (umallocs[i].addr == 0)
				continue;
			uint32 a = umallocs[i].addr;
			uint32 s = umallocs[i].size;
			if (va >= a && va < a + s)
			{
				alloc = 1;
				break;
			}
		}
		/* also consult page-tables like smalloc/sget do: if the page is marked
		 * with PERM_UHPAGE in our page table then it's in-use/reserved */
		if (!alloc)
		{
			uint32 *page_table = NULL;
			sys_get_frame_info(myEnv->env_page_directory, (uint32)va, &page_table);
			if (page_table != NULL && (page_table[PTX(va)] & PERM_UHPAGE))
				alloc = 1;
		}

		if (!alloc)
		{

			if (currentNumPages == 0)
				currentStartAddr = va;
			currentNumPages++;
		}
		else
		{
			if (currentNumPages >= numPages)
			{
				if (currentNumPages == numPages && exactStartAddr == 0)
				{
					//	                	if(currentStartAddr<USER_HEAP_START||currentStartAddr+bytes<USER_HEAP_START)return NULL;
					exactStartAddr = currentStartAddr;
				}
				if (currentNumPages > worstNumPages)
				{
					//	                	if(currentStartAddr<USER_HEAP_START||currentStartAddr+bytes<USER_HEAP_START)return NULL;
					worstNumPages = currentNumPages;
					worstStartAddr = currentStartAddr;
				}
			}
			currentNumPages = 0;
		}

		va += PAGE_SIZE;
	}

	/* tail check */
	if (currentNumPages >= numPages)
	{

		if (currentNumPages == numPages && exactStartAddr == 0)
		{
			exactStartAddr = currentStartAddr;
		}

		if (currentNumPages > worstNumPages)
		{
			worstNumPages = currentNumPages;
			worstStartAddr = currentStartAddr;
		}
	}

	uint32 allocva = 0;

	if (exactStartAddr != 0)
	{
		allocva = exactStartAddr;
	}
	else if (worstStartAddr != 0)
	{
		allocva = worstStartAddr;
	}
	else if (uheapPageAllocBreak + bytes <= USER_HEAP_MAX)
	{

		allocva = uheapPageAllocBreak;
		uheapPageAllocBreak += bytes;

		if (allocva < USER_HEAP_START || allocva >= USER_HEAP_MAX || (allocva + bytes) < USER_HEAP_START || (allocva + bytes) >= USER_HEAP_MAX)
		{
			uheapPageAllocBreak -= bytes;
			allocva = 0;
		}
	}
	else
		return NULL;

	// does not fit in any strategy
	if (allocva == 0)
		return NULL;

	umalloc_save(allocva, bytes);
	sys_allocate_user_mem(allocva, bytes);
	return (void *)allocva;
	#else
	panic("not allowed");
#endif
	// release_uspinlock(slock);
	// Comment the following line
	// panic("malloc() is not implemented yet...!!");
}

//=================================
// [2] FREE SPACE FROM USER HEAP:
//=================================
void free(void *virtual_address)
{
#if USE_KHEAP

	// TODO: [PROJECT'25.IM#2] USER HEAP - #3 free
	// Your code is here
	if (virtual_address == NULL)
		return;

	uint32 va = (uint32)virtual_address;

	if (va >= dynAllocStart && va < dynAllocEnd)
	{
		free_block(virtual_address);

		return;
	}

	if (va >= (uheapPageAllocStart) && va < USER_HEAP_MAX)
	{

		int index = -1;
		for (int i = 0; i < MAX_MALLOC_ALLOCS; i++)
		{
			if (umallocs[i].addr == va)
			{
				index = i;
			}
		}
		if (index < 0)
		{
			// invalid
			//	            panic("free(): invalid page-allocation address %x\n", va);
			return;
		}

		uint32 bytes = umallocs[index].size;

		sys_free_user_mem(va, bytes);

		umalloc_remove_index(index);

		// decrease the uheap break
		shrink_break();

		return;

	}
#else
	panic("not allowed");
#endif
	// out of range
	//	    panic("free(): invalid address %x (out of range the user heap)\n", va);
	//	    return;

	// Comment the following line
	//	panic("free() is not implemented yet...!!");
}

//=================================
// [3] ALLOCATE SHARED VARIABLE:
//=================================
void *smalloc(char *sharedVarName, uint32 size, uint8 isWritable)
{
#if USE_KHEAP

	//==============================================================
	// DON'T CHANGE THIS CODE========================================
	uheap_init();
	if (size == 0)
		return NULL;
	//==============================================================

	// TODO: [PROJECT'25.IM#3] SHARED MEMORY - #2 smalloc
	// panic("smalloc() is not implemented yet...!!");

	// 1. Round up size to page boundary
	uint32 bytes = ROUNDUP(size, PAGE_SIZE);
	uint32 numPages = bytes / PAGE_SIZE;

	// 2. Apply CUSTOM FIT strategy
	uint32 hStart = uheapPageAllocStart;
	uint32 hEnd = uheapPageAllocBreak;

	uint32 currentStartAddr = 0;
	uint32 currentNumPages = 0;

	uint32 exactStartAddr = 0;
	uint32 worstStartAddr = 0;
	uint32 worstNumPages = 0;

	uint32 va = hStart;
	while (va < hEnd)
	{
		// check whether this page is used
		int alloc = 0;

		// Check umallocs first
		for (int i = 0; i < MAX_MALLOC_ALLOCS; i++)
		{
			if (umallocs[i].addr == 0)
				continue;
			uint32 a = umallocs[i].addr;
			uint32 s = umallocs[i].size;
			if (va >= a && va < a + s)
			{
				alloc = 1;
				break;
			}
		}
		// Check Page Table
		if (!alloc)
		{
			uint32 *page_table = NULL;
			sys_get_frame_info(myEnv->env_page_directory, (uint32)va, &page_table);
			if (page_table != NULL && (page_table[PTX(va)] & PERM_UHPAGE))
				alloc = 1;
		}

		if (!alloc)
		{
			if (currentNumPages == 0)
				currentStartAddr = va;
			currentNumPages++;
		}
		else
		{
			if (currentNumPages >= numPages)
			{
				if (currentNumPages == numPages && exactStartAddr == 0)
				{
					exactStartAddr = currentStartAddr;
				}
				if (currentNumPages > worstNumPages)
				{
					worstNumPages = currentNumPages;
					worstStartAddr = currentStartAddr;
				}
			}
			currentNumPages = 0;
		}
		va += PAGE_SIZE;
	}

	/* tail check */
	if (currentNumPages >= numPages)
	{
		if (currentNumPages == numPages && exactStartAddr == 0)
		{
			exactStartAddr = currentStartAddr;
		}
		if (currentNumPages > worstNumPages)
		{
			worstNumPages = currentNumPages;
			worstStartAddr = currentStartAddr;
		}
	}

	uint32 allocva = 0;

	if (exactStartAddr != 0)
	{
		allocva = exactStartAddr;
	}
	else if (worstStartAddr != 0)
	{
		allocva = worstStartAddr;
	}
	else if (uheapPageAllocBreak + bytes <= USER_HEAP_MAX)
	{
		allocva = uheapPageAllocBreak;
		uheapPageAllocBreak += bytes;
		if (allocva < USER_HEAP_START || allocva >= USER_HEAP_MAX || (allocva + bytes) < USER_HEAP_START || (allocva + bytes) >= USER_HEAP_MAX)
		{
			uheapPageAllocBreak -= bytes;
			allocva = 0;
		}
	}
	else
		return NULL;

	if (allocva == 0)
		return NULL;

	// 3. Call sys_create_shared_object to create and allocate in kernel
	int result = sys_create_shared_object(sharedVarName, size, isWritable, (void *)allocva);

	if (result < 0)
	{
		// Rollback if extension happened and failed?
		// But result < 0 usually means shared object issues, not memory issues if address is valid.
		// However, if we extended break, we might want to revert, but complex to revert safely if logic below fails.
		// For now, return NULL.
		return NULL;
	}
	/* record the allocation in our user malloc table */
	umalloc_save((uint32)allocva, bytes);
	return (void *)allocva;
	#else
	panic("not allowed");
#endif
}

//========================================
// [4] SHARE ON ALLOCATED SHARED VARIABLE:
//========================================
/*
 * Steps:
 * 1) Get the size of the shared object from the owner environment using sys_size_of
 * 2) Round up size to page boundary
 * 3) Apply CUSTOM FIT strategy
 */
void *sget(int32 ownerEnvID, char *sharedVarName)
{
#if USE_KHEAP

	uheap_init();

	// 1. Get the size of the shared object
	int size = sys_size_of_shared_object(ownerEnvID, sharedVarName);
	if (size < 0)
	{
		return NULL; // Shared object doesn't exist
	}

	// 2. Round up size to page boundary
	uint32 bytes = ROUNDUP(size, PAGE_SIZE);
	uint32 numPages = bytes / PAGE_SIZE;

	// 3. Apply CUSTOM FIT strategy
	uint32 hStart = uheapPageAllocStart;
	uint32 hEnd = uheapPageAllocBreak;
	// scan the used area for free runs (holes) by consulting our umalloc_table.
	// We treat any ranges recorded in umalloc_table as reserved. Everything else inside
	// [hStart, hEnd) is considered free. This mirrors kernel approach where get_frame_info
	// is used to detect used pages.
	uint32 currentStartAddr = 0;
	// track current number of pages
	uint32 currentNumPages = 0;
	// variables to track best fits
	uint32 exactStartAddr = 0;
	// variables to track worst fit
	uint32 worstStartAddr = 0; // track worst number of pages
	uint32 worstNumPages = 0;
	// scan the heap area
	uint32 va = hStart;
	while (va < hEnd)
	{
		int alloc = 0;
		// Check umallocs
		for (int i = 0; i < MAX_MALLOC_ALLOCS; i++)
		{
			if (umallocs[i].addr == 0)
				continue;
			uint32 a = umallocs[i].addr;
			uint32 s = umallocs[i].size;
			if (va >= a && va < a + s)
			{
				alloc = 1;
				break;
			}
		}
		// Check Page Table
		if (!alloc)
		{
			uint32 *page_table = NULL;
			sys_get_frame_info(myEnv->env_page_directory, (uint32)va, &page_table);
			if (page_table != NULL && (page_table[PTX(va)] & PERM_UHPAGE))
				alloc = 1;
		}

		if (!alloc)
		{
			if (currentNumPages == 0)
				currentStartAddr = va;
			currentNumPages++;
		}
		else
		{
			if (currentNumPages >= numPages)
			{
				if (currentNumPages == numPages && exactStartAddr == 0)
				{
					exactStartAddr = currentStartAddr;
				}
				if (currentNumPages > worstNumPages)
				{
					worstNumPages = currentNumPages;
					worstStartAddr = currentStartAddr;
				}
			}
			currentNumPages = 0;
		}
		va += PAGE_SIZE;
	}

	/* tail check */
	if (currentNumPages >= numPages)
	{
		if (currentNumPages == numPages && exactStartAddr == 0)
		{
			exactStartAddr = currentStartAddr;
		}
		if (currentNumPages > worstNumPages)
		{
			worstNumPages = currentNumPages;
			worstStartAddr = currentStartAddr;
		}
	}

	uint32 allocva = 0;

	if (exactStartAddr != 0)
	{
		allocva = exactStartAddr;
	}
	else if (worstStartAddr != 0)
	{
		allocva = worstStartAddr;
	}
	else if (uheapPageAllocBreak + bytes <= USER_HEAP_MAX)
	{
		allocva = uheapPageAllocBreak;
		uheapPageAllocBreak += bytes;
		if (allocva < USER_HEAP_START || allocva >= USER_HEAP_MAX || (allocva + bytes) < USER_HEAP_START || (allocva + bytes) >= USER_HEAP_MAX)
		{
			uheapPageAllocBreak -= bytes;
			allocva = 0;
		}
	}
	else
		return NULL;

	if (allocva == 0)
		return NULL;

	// 4. Call sys_get_shared_object to share it in current process
	int result = sys_get_shared_object(ownerEnvID, sharedVarName, (void *)allocva);

	if (result < 0)
	{
		return NULL; // Failed to get
	}
	/* record mapping */
	umalloc_save((uint32)allocva, bytes);
	return (void *)allocva;
	#else
	panic("not allowed");
#endif
}

//==================================================================================//
//============================== BONUS FUNCTIONS ===================================//
//==================================================================================//

//=================================
// REALLOC USER SPACE:
//=================================
//	Attempts to resize the allocated space at "virtual_address" to "new_size" bytes,
//	possibly moving it in the heap.
//	If successful, returns the new virtual_address, in which case the old virtual_address must no longer be accessed.
//	On failure, returns a null pointer, and the old virtual_address remains valid.

//	A call with virtual_address = null is equivalent to malloc().
//	A call with new_size = zero is equivalent to free().

//  Hint: you may need to use the sys_move_user_mem(...)
//		which switches to the kernel mode, calls move_user_mem(...)
//		in "kern/mem/chunk_operations.c", then switch back to the user mode here
//	the move_user_mem() function is empty, make sure to implement it.
void *realloc(void *virtual_address, uint32 new_size)
{
#if USE_KHEAP
	//==============================================================
	// DON'T CHANGE THIS CODE========================================
	uheap_init();
	//==============================================================

	if (virtual_address == NULL)
		return malloc(new_size);

	if (new_size == 0)
	{
		free(virtual_address);
		return NULL;
	}

	uint32 va = (uint32)virtual_address;

	// [1] Block Allocator Range
	// Check if address is within the block allocator range
	if (va >= USER_HEAP_START && va < dynAllocStart + DYN_ALLOC_MAX_SIZE)
	{
		return realloc_block(virtual_address, new_size);
	}

	// [2] Page Allocator Range
	if (va >= uheapPageAllocStart && va < USER_HEAP_MAX)
	{
		// Find allocation record
		int idx = -1;
		for (int i = 0; i < MAX_MALLOC_ALLOCS; i++)
		{
			if (umallocs[i].addr == va)
			{
				idx = i;
				break;
			}
		}

		if (idx == -1)
			return NULL; // Pointer not found

		uint32 old_size = umallocs[idx].size;
		uint32 new_aligned_size = ROUNDUP(new_size, PAGE_SIZE);
		// old_size in umallocs is already page-aligned from malloc/umalloc_save

		if (new_aligned_size == old_size)
			return virtual_address;

		if (new_aligned_size < old_size)
		{
			// SHRINK: Free the tail pages
			uint32 diff = old_size - new_aligned_size;
			sys_free_user_mem(va + new_aligned_size, diff);
			umallocs[idx].size = new_aligned_size;

			// If we freed pages at the break, try to shrink it
			shrink_break();
			return virtual_address;
		}
		else
		{
			// EXPAND
			uint32 diff = new_aligned_size - old_size;
			uint32 next_va = va + old_size;
			uint32 end_va = va + new_aligned_size;

			if (end_va > USER_HEAP_MAX)
				return NULL;

			// Check if the adjacent space is free
			int can_extend = 1;

			// 1. Check umallocs collision
			for (int i = 0; i < MAX_MALLOC_ALLOCS; i++)
			{
				if (umallocs[i].addr != 0)
				{
					uint32 a = umallocs[i].addr;
					uint32 s = umallocs[i].size;
					// Check intersection: [a, a+s) overlaps [next_va, end_va)
					if (MAX(a, next_va) < MIN(a + s, end_va))
					{
						can_extend = 0;
						break;
					}
				}
			}

			// 2. Check Page Table (Physical usage)
			if (can_extend)
			{
				for (uint32 p = next_va; p < end_va; p += PAGE_SIZE)
				{
					uint32 *pt = NULL;
					sys_get_frame_info(myEnv->env_page_directory, p, &pt);
					if (pt != NULL && (pt[PTX(p)] & PERM_UHPAGE))
					{
						can_extend = 0;
						break;
					}
				}
			}

			if (can_extend)
			{
				// Extend in place
				if (end_va > uheapPageAllocBreak)
				{
					uheapPageAllocBreak = end_va;
				}
				sys_allocate_user_mem(next_va, diff);
				umallocs[idx].size = new_aligned_size;
				return virtual_address;
			}
			else
			{
				// Cannot extend, malloc new space
				void *new_ptr = malloc(new_size);
				if (new_ptr == NULL)
					return NULL;

				// Copy data
				memcpy(new_ptr, virtual_address, old_size); // Copy only valid old data

				// Free old space
				free(virtual_address);
				return new_ptr;
			}
		}
	}

	return NULL;

#else
	panic("not allowed");
#endif
}

//=================================
// FREE SHARED VARIABLE:
//=================================
//	This function frees the shared variable at the given virtual_address
//	To do this, we need to switch to the kernel, free the pages AND "EMPTY" PAGE TABLES
//	from main memory then switch back to the user again.
//
//	use sys_delete_shared_object(...); which switches to the kernel mode,
//	calls delete_shared_object(...) in "shared_memory_manager.c", then switch back to the user mode here
//	the delete_shared_object() function is empty, make sure to implement it.
void sfree(void *virtual_address)
{
	// TODO: [PROJECT'25.BONUS#5] EXIT #2 - sfree
	// Your code is here
	if (virtual_address == NULL)
		return;

	uint32 va = (uint32)virtual_address;

	// 1) Find the ID of the shared variable
	// ID = VA masked (clearing MSB)
	int32 id = (int32)va & 0x7FFFFFFF;

	// 2) Call syscall to free shared object
//	sys_freeSharedObject(id, virtual_address);
	sys_delete_shared_object(id,virtual_address);

	// 3) Update User Heap Bookkeeping (umallocs)
	// Find and remove the entry to mark space as free
	int idx = -1;
	for (int i = 0; i < MAX_MALLOC_ALLOCS; i++)
	{
		if (umallocs[i].addr == va)
		{
			idx = i;
			break;
		}
	}

	if (idx != -1)
	{
		umalloc_remove_index(idx);
		shrink_break();
	}
}
