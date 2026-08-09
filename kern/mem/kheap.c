#include "kheap.h"

#include <inc/memlayout.h>
#include <inc/dynamic_allocator.h>
#include <kern/conc/sleeplock.h>
#include <kern/proc/user_environment.h>
#include <kern/mem/memory_manager.h>
#include "../conc/kspinlock.h"
//***********************************
// struct AllocatedPagesList allocated_pages;
#define Phys_frames 65536 //(256*1024*1024)/PAGE_SIZE =256 mb / 4 kb
uint32 PA_2_VA[Phys_frames];
#define MAX_KMALLOC_ALLOCS 4096

struct kspinlock k;
typedef struct {
    uint32 addr;
    uint32 size;
} kmalloc_entry_t;

static kmalloc_entry_t kmalloc_table[MAX_KMALLOC_ALLOCS];

//==================================================================================//
//============================== HELPER FUNCTIONS ===================================//
//==================================================================================//
static void kmalloc_save(uint32 addr, uint32 size)
{
    for (int i = 0; i < MAX_KMALLOC_ALLOCS; i++) {
        if (kmalloc_table[i].addr == 0) {
            kmalloc_table[i].addr = addr;
            kmalloc_table[i].size = size;
            return;
        }
    }
}

void try_shrink_break(void)
{
    uint32 *ptr_pt = NULL;

    while (kheapPageAllocBreak > kheapPageAllocStart) {
        uint32 lastPageVA = kheapPageAllocBreak - PAGE_SIZE;

        struct FrameInfo *fi = get_frame_info(ptr_page_directory, lastPageVA, &ptr_pt);
        if (fi != NULL)
            break;

        kheapPageAllocBreak -= PAGE_SIZE;
    }
}

//==================================================================================//
//============================== GIVEN FUNCTIONS ===================================//
//==================================================================================//

//==============================================
// [1] INITIALIZE KERNEL HEAP:
//==============================================
//TODO: [PROJECT'25.GM#2] KERNEL HEAP - #0 kheap_init [GIVEN]
//Remember to initialize locks (if any)
void kheap_init()
{
#if USE_KHEAP
	//==================================================================================
	//DON'T CHANGE THESE LINES==========================================================
	//==================================================================================
	{
		initialize_dynamic_allocator(KERNEL_HEAP_START, KERNEL_HEAP_START + DYN_ALLOC_MAX_SIZE);
		set_kheap_strategy(KHP_PLACE_CUSTOMFIT);
		kheapPageAllocStart = dynAllocEnd + PAGE_SIZE;
		kheapPageAllocBreak = kheapPageAllocStart;

	}
	//==================================================================================
	//==================================================================================
	// LIST_INIT(&allocated_pages);
	init_kspinlock(&k,"kheap lock");

#endif
}

//==============================================
// [2] GET A PAGE FROM THE KERNEL FOR DA:
//==============================================
int get_page(void* va)
{
	int ret = alloc_page(ptr_page_directory, ROUNDDOWN((uint32)va, PAGE_SIZE), PERM_WRITEABLE, 1);
	if (ret < 0)
		panic("get_page() in kern: failed to allocate page from the kernel");
	//		mapping the PA_2_VA list
				uint32* ptr_page_table = NULL;
				    get_page_table(ptr_page_directory, (uint32)va, &ptr_page_table);
				    uint32 page_entry = ptr_page_table[PTX((uint32)va)];
				    uint32 framenumber = page_entry >> 12;
				    PA_2_VA[framenumber] = (uint32)va;
	return 0;
}

//==============================================
// [3] RETURN A PAGE FROM THE DA TO KERNEL:
//==============================================
void return_page(void* va)
{
	//				unmapping the PA_2_VA list
				 uint32* ptr_page_table = NULL;
				    get_page_table(ptr_page_directory, (uint32)va, &ptr_page_table);
				    uint32 page_entry = ptr_page_table[PTX((uint32)va)];
				    uint32 framenumber = page_entry >> 12;
				    PA_2_VA[framenumber] = 0;
	unmap_frame(ptr_page_directory, ROUNDDOWN((uint32)va, PAGE_SIZE));
}

//==================================================================================//
//============================ REQUIRED FUNCTIONS ==================================//
//==================================================================================//
//===================================
// [1] ALLOCATE SPACE IN KERNEL HEAP:
//===================================
void* kmalloc(unsigned int size)
{
#if USE_KHEAP

	acquire_kspinlock(&k);
	//TODO: [PROJECT'25.GM#2] KERNEL HEAP - #1 kmalloc
	//Your code is here
	//Comment the following line
	//kpanic_into_prompt("kmalloc() is not implemented yet...!!");

	if (size <= DYN_ALLOC_MAX_BLOCK_SIZE) {
		release_kspinlock(&k);
		return alloc_block(size);
	}

	uint32 bytes = ROUNDUP(size, PAGE_SIZE);
	uint32 numPages = bytes / PAGE_SIZE;
	uint32 hStart = kheapPageAllocStart;
	uint32 hEnd = kheapPageAllocBreak;

	uint32 currentStartAddr = 0;
	uint32 currentNumPages = 0;

	uint32 exactStartAddr = 0;
	uint32 worstStartAddr = 0;
	uint32 worstNumPages = 0;


	uint32 *ptr_pt = NULL;
	for(uint32 va = hStart; va < hEnd; va += PAGE_SIZE) {
		struct FrameInfo *fi = get_frame_info(ptr_page_directory, va, &ptr_pt);

		if(fi == NULL) {
			if(currentNumPages == 0) {
				currentStartAddr = va;
			}
			currentNumPages++;

			// if(currentNumPages == numPages) {
			// 	break;
			// }
			//TODO: when free found add to a free pages list for fast allocation
		} else {
			if(currentNumPages >= numPages) {

				if(currentNumPages == numPages && exactStartAddr == 0){
					exactStartAddr = currentStartAddr;
				}

				if(currentNumPages > worstNumPages){
					worstNumPages = currentNumPages;
					worstStartAddr = currentStartAddr;
				}
			}
			currentNumPages = 0;
		}
	}

	if (currentNumPages >= numPages) {
		if (currentNumPages == numPages && exactStartAddr == 0) {
			exactStartAddr = currentStartAddr;
		}
		if (currentNumPages > worstNumPages) {
			worstNumPages   = currentNumPages;
			worstStartAddr = currentStartAddr;
		}
	}

	uint32 allocva = 0;
	// struct AllocatedPage *allocated_page;

	if(exactStartAddr != 0) {
		allocva = exactStartAddr;

	} else if (worstStartAddr != 0) {
		allocva = worstStartAddr;

	} else {
		if(bytes > KERNEL_HEAP_MAX - kheapPageAllocBreak) {
			release_kspinlock(&k);
			return NULL;
		}
		allocva = kheapPageAllocBreak;
		kheapPageAllocBreak += bytes;
		// cprintf("numPages = %u\n", numPages);
	}

	kmalloc_save(allocva, bytes);

	for(uint32 i = 0; i < numPages; i++) {
			void *pageva = (void*)(allocva + (i * PAGE_SIZE));
			get_page(pageva);

		}


	//TODO: [PROJECT'25.BONUS#3] FAST PAGE ALLOCATOR
	release_kspinlock(&k);
	return (void*)allocva;
	#else
	panic("not allowed");
#endif
}
//=================================
// [2] FREE SPACE FROM KERNEL HEAP:
//=================================
void kfree(void* virtual_address)
{
	#if USE_KHEAP

	acquire_kspinlock(&k);
	//TODO: [PROJECT'25.GM#2] KERNEL HEAP - #2 kfree
	//Your code is here
	//Comment the following line
	// for(uint32)
	// panic("kfree() is not implemented yet...!!");
	uint32 va = (uint32)virtual_address; // i should've put void* address instead in the struct but im lazy tbh

	if( va >= dynAllocStart && va < dynAllocEnd) {
		free_block(virtual_address);
		release_kspinlock(&k);
		return;
	}

	for(int i = 0; i < MAX_KMALLOC_ALLOCS; i++) {
		if(kmalloc_table[i].addr == va) {

            uint32 bytes = kmalloc_table[i].size;
            uint32 nPages = bytes / PAGE_SIZE;


			for(uint32 j = 0; j < nPages ; j++) {
				void *pageva = (void*)(va + (j * PAGE_SIZE));
				return_page(pageva);
			}

            kmalloc_table[i].addr = 0;
            kmalloc_table[i].size = 0;

            try_shrink_break();
            release_kspinlock(&k);
			return;
		}
	}
	#else
	panic("not allowed");
#endif
}

//=================================
// [3] FIND VA OF GIVEN PA:
//=================================
unsigned int kheap_virtual_address(unsigned int physical_address)
{
	#if USE_KHEAP

	//TODO: [PROJECT'25.GM#2] KERNEL HEAP - #3 kheap_virtual_address
	//Your code is here
	acquire_kspinlock(&k);
	uint32 VA;
	uint32 frame= physical_address>>12;
	if(frame>=Phys_frames){
		release_kspinlock(&k);
		return 0;}
	uint32 Offset= physical_address&0x00000FFF;
	if(PA_2_VA[frame]==0){
		release_kspinlock(&k);
		return 0;}
	uint32 Page_address=PA_2_VA[frame];

	VA=Page_address+Offset;
	release_kspinlock(&k);
	return VA;
	//Comment the following line
	//panic("kheap_virtual_address() is not implemented yet...!!");
#else
	panic("not allowed");
#endif
	/*EFFICIENT IMPLEMENTATION ~O(1) IS REQUIRED */
}

//=================================
// [4] FIND PA OF GIVEN VA:
//=================================
unsigned int kheap_physical_address(unsigned int virtual_address)
{
#if USE_KHEAP

	//TODO: [PROJECT'25.GM#2] KERNEL HEAP - #4 kheap_physical_address
	//Your code is here
	acquire_kspinlock(&k);
	if(virtual_address==0){
		release_kspinlock(&k);
		return 0;}
	uint32 PA;
	uint32 Offset=virtual_address&0x00000FFF;
	uint32* ptr_page_table=NULL;

	get_page_table(ptr_page_directory,virtual_address,&ptr_page_table);

	uint32 page_entry=ptr_page_table[PTX(virtual_address)];

	if(!(page_entry&PERM_PRESENT)){
		release_kspinlock(&k);
		return 0; // no mapping
	}

	PA=((page_entry>>12)*PAGE_SIZE)+Offset;
	release_kspinlock(&k);
	return PA;
	//Comment the following line
	//panic("kheap_physical_address() is not implemented yet...!!");
#else
	panic("not allowed");
#endif
	/*EFFICIENT IMPLEMENTATION ~O(1) IS REQUIRED */
}

//=================================================================================//
//============================== BONUS FUNCTION ===================================//
//=================================================================================//
// krealloc():

//	Attempts to resize the allocated space at "virtual_address" to "new_size" bytes,
//	possibly moving it in the heap.
//	If successful, returns the new virtual_address, in which case the old virtual_address must no longer be accessed.
//	On failure, returns a null pointer, and the old virtual_address remains valid.

//	A call with virtual_address = null is equivalent to kmalloc().
//	A call with new_size = zero is equivalent to kfree().

extern __inline__ uint32 get_block_size(void *va);

void *krealloc(void *virtual_address, uint32 new_size)
{
	//TODO: [PROJECT'25.BONUS#2] KERNEL REALLOC - krealloc
	//Your code is here
	//Comment the following line
	panic("krealloc() is not implemented yet...!!");

}
