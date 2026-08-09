#include <inc/memlayout.h>
#include "shared_memory_manager.h"

#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/queue.h>
#include <inc/environment_definitions.h>

#include <kern/proc/user_environment.h>
#include <kern/trap/syscall.h>
#include "kheap.h"
#include "memory_manager.h"

//==================================================================================//
//============================== GIVEN FUNCTIONS ===================================//
//==================================================================================//

//===========================
// [1] INITIALIZE SHARES:
//===========================
// Initialize the list and the corresponding lock
void sharing_init()
{
#if USE_KHEAP
	LIST_INIT(&AllShares.shares_list);
	init_kspinlock(&AllShares.shareslock, "shares lock");
#else
	panic("not handled when KERN HEAP is disabled");
#endif
}

//=========================
// [2] Find Share Object:
//=========================
// Search for the given shared object in the "shares_list"
// Return:
//	a) if found: ptr to Share object
//	b) else: NULL
struct Share *find_share(int32 ownerID, char *name)
{
#if USE_KHEAP
	struct Share *ret = NULL;
	bool wasHeld = holding_kspinlock(&(AllShares.shareslock));
	if (!wasHeld)
	{
		acquire_kspinlock(&(AllShares.shareslock));
	}
	{
		struct Share *shr;
		LIST_FOREACH(shr, &(AllShares.shares_list))
		{
			if (shr->ownerID == ownerID && strcmp(name, shr->name) == 0)
			{
				ret = shr;
				break;
			}
		}
	}
	if (!wasHeld)
	{
		release_kspinlock(&(AllShares.shareslock));
	}
	return ret;
#else
	panic("not handled when KERN HEAP is disabled");
#endif
}

//==============================
// [3] Get Size of Share Object:
//==============================
int size_of_shared_object(int32 ownerID, char *shareName)
{
	struct Share *ptr_share = find_share(ownerID, shareName);
	if (ptr_share == NULL)
		return E_SHARED_MEM_NOT_EXISTS;
	else
		return ptr_share->size;

	return 0;
}

//==================================================================================//
//============================ REQUIRED FUNCTIONS ==================================//
//==================================================================================//

//=====================================
// [1] Alloc & Initialize Share Object:
//=====================================
struct Share *alloc_share(int32 ownerID, char *shareName, uint32 size, uint8 isWritable)
{
#if USE_KHEAP
	struct Share *newShare = (struct Share *)kmalloc(sizeof(struct Share));
	if (newShare == NULL)
	{
		return NULL;
	}

	newShare->ownerID = ownerID;
	strncpy(newShare->name, shareName, 63);
	newShare->name[63] = '\0';
	newShare->size = size;
	newShare->isWritable = isWritable;
	newShare->references = 1;
	newShare->ID = 0;

	uint32 numFrames = ROUNDUP(size, PAGE_SIZE) / PAGE_SIZE;
	newShare->framesStorage = (struct FrameInfo **)kmalloc(numFrames * sizeof(struct FrameInfo *));
	if (newShare->framesStorage == NULL)
	{
		kfree(newShare);
		return NULL;
	}

	for (uint32 i = 0; i < numFrames; i++)
	{
		newShare->framesStorage[i] = NULL;
	}
	return newShare;
#else
	panic("not allowed");
#endif
}

//=========================
// [4] Create Share Object:
//=========================
int create_shared_object(int32 ownerID, char *shareName, uint32 size, uint8 isWritable, void *virtual_address)
{
#if USE_KHEAP
	struct Env *myenv = get_cpu_proc();

	struct Share *existingShare = find_share(ownerID, shareName);
	if (existingShare != NULL)
	{
		return E_SHARED_MEM_EXISTS;
	}

	struct Share *newShare = alloc_share(ownerID, shareName, size, isWritable);
	if (newShare == NULL)
	{
		return E_NO_SHARE;
	}

	newShare->ID = ((uint32)virtual_address) & 0x7FFFFFFF;

	uint32 numFrames = ROUNDUP(size, PAGE_SIZE) / PAGE_SIZE;
	uint32 va = (uint32)virtual_address;

	for (uint32 i = 0; i < numFrames; i++)
	{
		struct FrameInfo *frame = NULL;
		int ret = allocate_frame(&frame);
		if (ret != 0)
		{
			for (uint32 j = 0; j < i; j++)
			{
				unmap_frame(myenv->env_page_directory, (uint32)virtual_address + j * PAGE_SIZE);
			}
			kfree(newShare->framesStorage);
			kfree(newShare);
			return E_NO_SHARE;
		}

		newShare->framesStorage[i] = frame;

		int map_ret = map_frame(myenv->env_page_directory, frame,
								(uint32)va, PERM_USER | PERM_WRITEABLE | PERM_UHPAGE);

		if (map_ret != 0)
		{
			free_frame(frame);
			for (uint32 j = 0; j < i; j++)
			{
				unmap_frame(myenv->env_page_directory, (uint32)virtual_address + j * PAGE_SIZE);
			}
			kfree(newShare->framesStorage);
			kfree(newShare);
			return E_NO_SHARE;
		}
		va += PAGE_SIZE;
	}

	// 5. Add the share object to shares_list (CRITICAL SECTION)
	acquire_kspinlock(&AllShares.shareslock);

	// 5.1 Race Condition Check
	existingShare = NULL;
	struct Share *shr;
	LIST_FOREACH(shr, &(AllShares.shares_list))
	{
		if (shr->ownerID == ownerID && strcmp(shareName, shr->name) == 0)
		{
			existingShare = shr;
			break;
		}
	}

	if (existingShare != NULL)
	{
		release_kspinlock(&AllShares.shareslock);
		for (uint32 i = 0; i < numFrames; i++)
		{
			unmap_frame(myenv->env_page_directory, (uint32)virtual_address + i * PAGE_SIZE);
		}
		kfree(newShare->framesStorage);
		kfree(newShare);
		return E_SHARED_MEM_EXISTS;
	}

	LIST_INSERT_HEAD(&AllShares.shares_list, newShare);
	release_kspinlock(&AllShares.shareslock);

	return newShare->ID;
#else
	panic("not allowed");
#endif
}

//======================
// [5] Get Share Object:
//======================
int get_shared_object(int32 ownerID, char *shareName, void *virtual_address)
{
#if USE_KHEAP
	struct Env *myenv = get_cpu_proc();

	// [FIX]: Acquire lock once
	acquire_kspinlock(&AllShares.shareslock);
	struct Share *share = find_share(ownerID, shareName);

	if (share == NULL)
	{
		// [FIX]: Release lock before returning failure
		release_kspinlock(&AllShares.shareslock);
		return E_SHARED_MEM_NOT_EXISTS;
	}

	// [FIX]: Increment reference count WHILE holding the lock
	// This ensures the object isn't deleted by another process before we map it
	share->references++;

	// [FIX]: Release lock before entering the mapping loop
	// Mapping pages involves memory allocation and should not be done while holding a spinlock
	release_kspinlock(&AllShares.shareslock);

	uint32 numFrames = (share->size + PAGE_SIZE - 1) / PAGE_SIZE;
	uint32 va = (uint32)virtual_address;

	uint32 permissions = PERM_USER | PERM_AVAILABLE;
	if (share->isWritable)
	{
		permissions |= PERM_WRITEABLE;
	}

	for (uint32 i = 0; i < numFrames; i++)
	{
		struct FrameInfo *frame = share->framesStorage[i];

		int ret = map_frame(myenv->env_page_directory, frame,
							(uint32)va, permissions);
		if (ret != 0)
		{
			// Cleanup: unmap already mapped frames
			for (uint32 j = 0; j < i; j++)
			{
				unmap_frame(myenv->env_page_directory,
							(uint32)virtual_address + j * PAGE_SIZE);
			}

			// [FIX]: If mapping fails, roll back the reference increment
			acquire_kspinlock(&AllShares.shareslock);
			share->references--;
			if (share->references == 0)
			{
				free_share(share);
			}
			release_kspinlock(&AllShares.shareslock);

			return E_NO_SHARE;
		}

		va += PAGE_SIZE;
	}

	return share->ID;
#else
	panic("not allowed");
#endif
}

//==================================================================================//
//============================== BONUS FUNCTIONS ===================================//
//==================================================================================//

//=========================
// [1] Delete Share Object:
//=========================
// delete the given shared object from the "shares_list"
// it should free its framesStorage and the share object itself
void free_share(struct Share *ptrShare)
{
#if USE_KHEAP

	LIST_REMOVE(&AllShares.shares_list, ptrShare);


	if (ptrShare->framesStorage != NULL)
	{
		kfree(ptrShare->framesStorage);
	}


	kfree(ptrShare);
#endif
}

//=========================
// [2] Free Share Object:
//=========================
int delete_shared_object(int32 sharedObjectID, void *startVA)
{
#if USE_KHEAP
	struct Env *myenv = get_cpu_proc();


	acquire_kspinlock(&AllShares.shareslock);

	struct Share *shr = NULL;
	struct Share *it = NULL;

	LIST_FOREACH(it, &AllShares.shares_list)
	{
		if (it->ID == sharedObjectID)
		{
			shr = it;
			break;
		}
	}


	if (shr == NULL)
	{
		release_kspinlock(&AllShares.shareslock);
		return E_SHARED_MEM_NOT_EXISTS;
	}


	uint32 numFrames = (shr->size + PAGE_SIZE - 1) / PAGE_SIZE;
	uint32 va = (uint32)startVA;

	for (uint32 i = 0; i < numFrames; i++)
	{
		unmap_frame(myenv->env_page_directory, va);
		va += PAGE_SIZE;
	}


	shr->references--;


	if (shr->references == 0)
	{
		free_share(shr);
	}


	release_kspinlock(&AllShares.shareslock);


	tlbflush();

	return 0;
#else
	panic("not allowed");
#endif
}
