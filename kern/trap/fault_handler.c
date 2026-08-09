
/*
 * fault_handler.c
 *
 *  Created on: Oct 12, 2022
 *      Author: HP
 */

#include "trap.h"
#include <kern/proc/user_environment.h>
#include <kern/cpu/sched.h>
#include <kern/cpu/cpu.h>
#include <kern/disk/pagefile_manager.h>
#include <kern/mem/memory_manager.h>
#include <kern/mem/kheap.h>

//2014 Test Free(): Set it to bypass the PAGE FAULT on an instruction with this length and continue executing the next one
// 0 means don't bypass the PAGE FAULT
uint8 bypassInstrLength = 0;

//===============================
// REPLACEMENT STRATEGIES
//===============================
//2020
void setPageReplacmentAlgorithmLRU(int LRU_TYPE)
{
	assert(LRU_TYPE == PG_REP_LRU_TIME_APPROX || LRU_TYPE == PG_REP_LRU_LISTS_APPROX);
	_PageRepAlgoType = LRU_TYPE ;
}
void setPageReplacmentAlgorithmCLOCK(){_PageRepAlgoType = PG_REP_CLOCK;}
void setPageReplacmentAlgorithmFIFO(){_PageRepAlgoType = PG_REP_FIFO;}
void setPageReplacmentAlgorithmModifiedCLOCK(){_PageRepAlgoType = PG_REP_MODIFIEDCLOCK;}
//2018/
void setPageReplacmentAlgorithmDynamicLocal(){_PageRepAlgoType = PG_REP_DYNAMIC_LOCAL;}
//2021/
void setPageReplacmentAlgorithmNchanceCLOCK(int PageWSMaxSweeps){_PageRepAlgoType = PG_REP_NchanceCLOCK;  page_WS_max_sweeps = PageWSMaxSweeps;}
//2024/
void setFASTNchanceCLOCK(bool fast){ FASTNchanceCLOCK = fast; };
//2025/
void setPageReplacmentAlgorithmOPTIMAL(){ _PageRepAlgoType = PG_REP_OPTIMAL; };

//2020
uint32 isPageReplacmentAlgorithmLRU(int LRU_TYPE){return _PageRepAlgoType == LRU_TYPE ? 1 : 0;}
uint32 isPageReplacmentAlgorithmCLOCK(){if(_PageRepAlgoType == PG_REP_CLOCK) return 1; return 0;}
uint32 isPageReplacmentAlgorithmFIFO(){if(_PageRepAlgoType == PG_REP_FIFO) return 1; return 0;}
uint32 isPageReplacmentAlgorithmModifiedCLOCK(){if(_PageRepAlgoType == PG_REP_MODIFIEDCLOCK) return 1; return 0;}
//2018/
uint32 isPageReplacmentAlgorithmDynamicLocal(){if(_PageRepAlgoType == PG_REP_DYNAMIC_LOCAL) return 1; return 0;}
//2021/
uint32 isPageReplacmentAlgorithmNchanceCLOCK(){if(_PageRepAlgoType == PG_REP_NchanceCLOCK) return 1; return 0;}
//2021/
uint32 isPageReplacmentAlgorithmOPTIMAL(){if(_PageRepAlgoType == PG_REP_OPTIMAL) return 1; return 0;}

//===============================
// PAGE BUFFERING
//===============================
void enableModifiedBuffer(uint32 enableIt){_EnableModifiedBuffer = enableIt;}
uint8 isModifiedBufferEnabled(){  return _EnableModifiedBuffer ; }

void enableBuffering(uint32 enableIt){_EnableBuffering = enableIt;}
uint8 isBufferingEnabled(){  return _EnableBuffering ; }

void setModifiedBufferLength(uint32 length) { _ModifiedBufferLength = length;}
uint32 getModifiedBufferLength() { return _ModifiedBufferLength;}

//===============================
// FAULT HANDLERS
//===============================

//==================
// [0] INIT HANDLER:
//==================
void fault_handler_init()
{
	//setPageReplacmentAlgorithmLRU(PG_REP_LRU_TIME_APPROX);
	//setPageReplacmentAlgorithmOPTIMAL();
	setPageReplacmentAlgorithmCLOCK();
	//setPageReplacmentAlgorithmModifiedCLOCK();
	enableBuffering(0);
	enableModifiedBuffer(0) ;
	setModifiedBufferLength(1000);
}
//==================
// [1] MAIN HANDLER:
//==================
//2022/
uint32 last_eip = 0;
uint32 before_last_eip = 0;
uint32 last_fault_va = 0;
uint32 before_last_fault_va = 0;
int8 num_repeated_fault  = 0;
extern uint32 sys_calculate_free_frames() ;

struct Env* last_faulted_env = NULL;
void fault_handler(struct Trapframe *tf)
{

	/******************/
	// Read processor's CR2 register to find the faulting address
	uint32 fault_va = rcr2();
	//cprintf("*****Faulted VA = %x*****\n", fault_va);
	//	print_trapframe(tf);
	/******************/

	//If same fault va for 3 times, then panic
	//UPDATE: 3 FAULTS MUST come from the same environment (or the kernel)
	struct Env* cur_env = get_cpu_proc();
	if (last_fault_va == fault_va && last_faulted_env == cur_env)
	{
		num_repeated_fault++ ;
		if (num_repeated_fault == 3)
		{
			print_trapframe(tf);
			panic("Failed to handle fault! fault @ at va = %x from eip = %x causes va (%x) to be faulted for 3 successive times\n", before_last_fault_va, before_last_eip, fault_va);
		}
	}
	else
	{
		before_last_fault_va = last_fault_va;
		before_last_eip = last_eip;
		num_repeated_fault = 0;
	}
	last_eip = (uint32)tf->tf_eip;
	last_fault_va = fault_va ;
	last_faulted_env = cur_env;
	/******************/
	//2017: Check stack overflow for Kernel
	int userTrap = 0;
	if ((tf->tf_cs & 3) == 3) {
		userTrap = 1;
	}
	if (!userTrap)
	{
		struct cpu* c = mycpu();
		//cprintf("trap from KERNEL\n");
		if (cur_env && fault_va >= (uint32)cur_env->kstack && fault_va < (uint32)cur_env->kstack + PAGE_SIZE)
			panic("User Kernel Stack: overflow exception!");
		else if (fault_va >= (uint32)c->stack && fault_va < (uint32)c->stack + PAGE_SIZE)
			panic("Sched Kernel Stack of CPU #%d: overflow exception!", c - CPUS);
#if USE_KHEAP
		if (fault_va >= KERNEL_HEAP_MAX)
			panic("Kernel: heap overflow exception!");
#endif
	}
	//2017: Check stack underflow for User
	else
	{
		//cprintf("trap from USER\n");
		if (fault_va >= USTACKTOP && fault_va < USER_TOP)
			panic("User: stack underflow exception!");
	}

	//get a pointer to the environment that caused the fault at runtime
	//cprintf("curenv = %x\n", curenv);
	struct Env* faulted_env = cur_env;
	if (faulted_env == NULL)
	{
		cprintf("\nFaulted VA = %x\n", fault_va);
		print_trapframe(tf);
		panic("faulted env == NULL!");
	}
	//check the faulted address, is it a table or not ?
	//If the directory entry of the faulted address is NOT PRESENT then
	if ( (faulted_env->env_page_directory[PDX(fault_va)] & PERM_PRESENT) != PERM_PRESENT)
	{
		faulted_env->tableFaultsCounter ++ ;
		table_fault_handler(faulted_env, fault_va);
	}
	else
	{
		if (userTrap)
		{
			//============================================================================================/
			//TODO: [PROJECT'25.GM#3] FAULT HANDLER I - #2 Check for invalid pointers
			//(e.g. pointing to unmarked user heap page, kernel or wrong access rights),
			//your code is here
#if USE_KHEAP
			uint32 value = pt_get_page_permissions(faulted_env->env_page_directory,fault_va);

			if(fault_va>=USER_LIMIT){

				env_exit();
			}
			if((value&PERM_PRESENT)&&(value&PERM_WRITEABLE)==0){
				env_exit();
			}
			if(fault_va >= USER_HEAP_START && fault_va < USER_HEAP_MAX){

				if((value & PERM_UHPAGE)==0){

					env_exit();
				}
			}
#endif



			//============================================================================================/
		}

		/*2022: Check if fault due to Access Rights */
		int perms = pt_get_page_permissions(faulted_env->env_page_directory, fault_va);
		if (perms & PERM_PRESENT)
			panic("Page @va=%x is exist! page fault due to violation of ACCESS RIGHTS\n", fault_va) ;
		//============================================================================================/


		// we have normal page fault =============================================================
		faulted_env->pageFaultsCounter ++ ;

//				cprintf("[%08s] user PAGE fault va %08x\n", faulted_env->prog_name, fault_va);
//				cprintf("\nPage working set BEFORE fault handler...\n");
//				env_page_ws_print(faulted_env);
		//int ffb = sys_calculate_free_frames();

		if(isBufferingEnabled())
		{
			__page_fault_handler_with_buffering(faulted_env, fault_va);
		}
		else
		{
			page_fault_handler(faulted_env, fault_va);
		}

		//		cprintf("\nPage working set AFTER fault handler...\n");
		//		env_page_ws_print(faulted_env);
		//		int ffa = sys_calculate_free_frames();
		//		cprintf("fault handling @%x: difference in free frames (after - before = %d)\n", fault_va, ffa - ffb);
	}

	/*********************/
	//Refresh the TLB cache
	tlbflush();
	/*********************/
}


//=========================
// [2] TABLE FAULT HANDLER:
//=========================
void table_fault_handler(struct Env * curenv, uint32 fault_va)
{
	//panic("table_fault_handler() is not implemented yet...!!");
	//Check if it's a stack page
	uint32* ptr_table;
#if USE_KHEAP
	{
		ptr_table = create_page_table(curenv->env_page_directory, (uint32)fault_va);
	}
#else
	{
		__static_cpt(curenv->env_page_directory, (uint32)fault_va, &ptr_table);
	}
#endif
}

//=========================
// [3] PAGE FAULT HANDLER:
//=========================
/* Calculate the number of page faults according th the OPTIMAL replacement strategy
 * Given:
 * 	1. Initial Working Set List (that the process started with)
 * 	2. Max Working Set Size
 * 	3. Page References List (contains the stream of referenced VAs till the process finished)
 *
 * 	IMPORTANT: This function SHOULD NOT change any of the given lists
 */
int get_optimal_num_faults(struct WS_List *initWorkingSet, int maxWSSize, struct PageRef_List *pageReferences)
{
	//TODO: [PROJECT'25.IM#1] FAULT HANDLER II - #2 get_optimal_num_faults
	//Your code is here
	//Comment the following line
#if USE_KHEAP
	int faults = 0;

	uint32 copy_initWorkingSet[maxWSSize];
	struct WorkingSetElement *wse = NULL;
	int count_ws = 0;
	LIST_FOREACH(wse,initWorkingSet){
		copy_initWorkingSet[count_ws++]=wse->virtual_address;
		//count_ws++;
	}
	int reff_size = LIST_SIZE(pageReferences);
	uint32 copy_pageReferences[reff_size];
	struct PageRefElement *reffElmnt;
	int count_reff = 0;
	LIST_FOREACH(reffElmnt,pageReferences){
		copy_pageReferences[count_reff++]=reffElmnt->virtual_address;
		//count_reff++;
	}

	for(int j=0;j<reff_size;j++){
		uint32 cur_ref = copy_pageReferences[j];
		int found = 0;
		for(int k=0;k<count_ws;k++){
			if(copy_initWorkingSet[j] == cur_ref){
				found=1;
				break;
			}
		}
		if(found){continue;}
		faults++;

		if (count_ws < maxWSSize)
		{
			copy_initWorkingSet[count_ws++] = cur_ref;
		}
		else{
			int farNext = -1;
			int victim = -1;
			uint32 curr_ws_pg;
			for(int l = 0;l<count_ws;l++){
				curr_ws_pg = copy_initWorkingSet[l];
				int next = -1;
				for(int m=j+1;m<count_reff;m++){
					if(copy_pageReferences[m]==curr_ws_pg){
						next=m;
						break;
					}
				}
				if(next==-1){
					victim=l;
					break;
				}
				if(next>farNext){
					farNext=next;
					victim=l;
				}
			}
			copy_initWorkingSet[victim] = copy_pageReferences[j];
		}

	}

	return faults;
#else
	panic("not allowed");
#endif
	//panic("get_optimal_num_faults() is not implemented yet...!!");
}


void page_fault_handler(struct Env * faulted_env, uint32 fault_va)
{
#if USE_KHEAP
	if (isPageReplacmentAlgorithmOPTIMAL())
	{
		//TODO: [PROJECT'25.IM#1] FAULT HANDLER II - #1 Optimal Reference Stream
		//Your code is here
		//Comment the following line
		if(faulted_env->cur_loaded==1){
			struct WorkingSetElement *new_wse;
			LIST_FOREACH(new_wse,&faulted_env->page_WS_list){
				struct WorkingSetElement *new_elmnt = kmalloc(sizeof(struct WorkingSetElement));
				new_elmnt->virtual_address=new_wse->virtual_address;
				LIST_INSERT_TAIL(&faulted_env->cur_WS_list,new_elmnt);
				struct PageRefElement *ref_Element=kmalloc(sizeof(struct PageRefElement));
				ref_Element->virtual_address=fault_va;
				LIST_INSERT_TAIL(&faulted_env->referenceStreamList, ref_Element);
			}
			faulted_env->cur_loaded=1;
		}
		//struct PageRefElement *ref_Element=kmalloc(sizeof(struct PageRefElement));
		//ref_Element->virtual_address=fault_va;
		//LIST_INSERT_TAIL(&faulted_env->referenceStreamList, ref_Element);

		uint32 *new_table;
		struct FrameInfo *ptr_frame_info = get_frame_info(faulted_env->env_page_directory, fault_va, &new_table);
		if(ptr_frame_info==NULL){
			allocate_frame(&ptr_frame_info);
			map_frame(faulted_env->env_page_directory, ptr_frame_info, fault_va, PERM_USER | PERM_WRITEABLE | PERM_PRESENT);
			uint32 r_fault_va = ROUNDDOWN(fault_va,PAGE_SIZE);
			pf_read_env_page(faulted_env,(void*)r_fault_va);
		}
		else{
			pt_set_page_permissions(faulted_env->env_page_directory, fault_va, PERM_PRESENT, 0);
		}
		if(LIST_SIZE(&faulted_env->cur_WS_list)==faulted_env->page_WS_max_size){
			struct WorkingSetElement *wse;
				LIST_FOREACH(wse,&faulted_env->page_WS_list){
				pt_set_page_permissions(faulted_env->env_page_directory,wse->virtual_address,0,PERM_PRESENT);
				LIST_REMOVE(&faulted_env->cur_WS_list,wse);
				kfree(wse);
			}
				struct WorkingSetElement *nwse =kmalloc(sizeof(struct WorkingSetElement));
				nwse->virtual_address=ROUNDDOWN(fault_va,PAGE_SIZE);
				LIST_INSERT_TAIL(&faulted_env->cur_WS_list,nwse);
		}
		else{
			struct WorkingSetElement *nwse =kmalloc(sizeof(struct WorkingSetElement));
			nwse->virtual_address=ROUNDDOWN(fault_va,PAGE_SIZE);
			LIST_INSERT_TAIL(&faulted_env->cur_WS_list,nwse);
		}
		struct PageRefElement *ref_Element=kmalloc(sizeof(struct PageRefElement));
		ref_Element->virtual_address=fault_va;
		LIST_INSERT_TAIL(&faulted_env->referenceStreamList, ref_Element);
		/*uint32 fault_va_pg = ROUNDDOWN(fault_va,PAGE_SIZE);
		struct WorkingSetElement *it = NULL;
		uint32 inAct = 0;
		LIST_FOREACH(it,&faulted_env->ActiveList){
			if(it->virtual_address==fault_va_pg){
				inAct=1;
				break;
			}
		}

		if(!inAct){
			if(LIST_SIZE(&faulted_env->ActiveList) == faulted_env->page_WS_max_size){
				struct WorkingSetElement *wse = NULL;
				LIST_FOREACH(wse,&faulted_env->ActiveList){
					pt_set_page_permissions(faulted_env->env_page_directory,wse->virtual_address,0,PERM_PRESENT);
				}
			}
			LIST_INIT(&faulted_env->ActiveList);
			struct WorkingSetElement *ws_elmnt = env_page_ws_list_create_element(faulted_env,fault_va_pg);
			LIST_INSERT_TAIL(&faulted_env->ActiveList,ws_elmnt);
		}

		uint32 perms = pt_get_page_permissions(faulted_env->env_page_directory,fault_va_pg);
		if(perms&PERM_PRESENT){

		}

		else if(perms!=0 && (perms&PERM_PRESENT)==0){
			pt_set_page_permissions(faulted_env->env_page_directory,fault_va_pg,PERM_PRESENT,0);
		}

		else{
			int val = pf_read_env_page(faulted_env,(void*)fault_va_pg);

			if(val == 0){

			}
			else{
				struct FrameInfo *ptr_new_frame = NULL;
				allocate_frame(&ptr_new_frame);
				map_frame(faulted_env->env_page_directory,ptr_new_frame,fault_va_pg,PERM_PRESENT|PERM_WRITEABLE|PERM_USER);
				pf_add_empty_env_page(faulted_env,fault_va_pg,0);
			}
		}

		struct PageRefElement *reffElmnt = kmalloc(sizeof(struct PageRefElement));
		reffElmnt->virtual_address = fault_va_pg;
		LIST_INSERT_TAIL(&faulted_env->referenceStreamList,reffElmnt);
		*/
		//panic("page_fault_handler().REPLACEMENT is not implemented yet...!!");
	}
	else
	{
		struct WorkingSetElement *victimWSElement = NULL;
		uint32 wsSize = LIST_SIZE(&(faulted_env->page_WS_list));
		if(wsSize < (faulted_env->page_WS_max_size))
		{
			//TODO: [PROJECT'25.GM#3] FAULT HANDLER I - #3 placement
			//Your code is here
			//Comment the following line
			//placement_logic(faulted_env,fault_va);

			struct FrameInfo *ptr_new_frame = NULL;
			int frame_alloc_ret = allocate_frame(&ptr_new_frame);
			map_frame(faulted_env->env_page_directory,ptr_new_frame,fault_va,PERM_USER | PERM_WRITEABLE|PERM_PRESENT);

			int page_r = pf_read_env_page(faulted_env,(void*)fault_va);

			if(page_r == E_PAGE_NOT_EXIST_IN_PF){

					if(!((fault_va>=USER_HEAP_START &&fault_va<USER_HEAP_MAX) ||(fault_va>=USTACKBOTTOM&&fault_va<USTACKTOP)))
					{
						env_exit();
					}
			}
			struct WorkingSetElement *nwse = env_page_ws_list_create_element(faulted_env,fault_va);
			LIST_INSERT_TAIL(&faulted_env->page_WS_list,nwse);
			if(LIST_SIZE(&faulted_env->page_WS_list)==faulted_env->page_WS_max_size)
				faulted_env->page_last_WS_element = LIST_FIRST(&faulted_env->page_WS_list);
				else
				faulted_env->page_last_WS_element=NULL;


			/*if(LIST_SIZE(&faulted_env->page_WS_list)==faulted_env->page_WS_max_size)
				faulted_env->page_last_WS_element = LIST_FIRST(&faulted_env->page_WS_list);
			else
				faulted_env->page_last_WS_element=NULL;*/

			//panic("page_fault_handler().PLACEMENT is not implemented yet...!!");
		}
		else
		{
			if (isPageReplacmentAlgorithmCLOCK())
			{
				//TODO: [PROJECT'25.IM#1] FAULT HANDLER II - #3 Clock Replacement
				//Your code is here
				//Comment the following line
				//env_page_ws_print(faulted_env);

				if(faulted_env->page_last_WS_element==NULL){
					faulted_env->page_last_WS_element=LIST_FIRST(&faulted_env->page_WS_list);
				}
				struct WorkingSetElement *cur_wse = NULL;
				struct WorkingSetElement *vic_wse = NULL;
				struct WorkingSetElement *before_vic_wse = NULL;
				struct WorkingSetElement *after_vic_wse = NULL;
				uint32 victim_va = 0;
				//uint32 perms = pt_get_page_permissions(faulted_env->env_page_directory,victim_va);
					cur_wse = faulted_env->page_last_WS_element;
				while(1){
					victim_va = cur_wse->virtual_address;// faulted_env->page_last_WS_element->virtual_address;

					//faulted_env->page_last_WS_element = LIST_NEXT(cur_wse);
//					if(faulted_env->page_last_WS_element == NULL){
//						faulted_env->page_last_WS_element = LIST_FIRST(&faulted_env->page_WS_list);
//					}

					uint32 perms = pt_get_page_permissions(faulted_env->env_page_directory,victim_va);
					if(perms&PERM_USED){
						pt_set_page_permissions(faulted_env->env_page_directory,victim_va,0,PERM_USED);
						cur_wse=LIST_NEXT(cur_wse);
						if(cur_wse == NULL){
							cur_wse = LIST_FIRST(&faulted_env->page_WS_list);
						}
					}
					else{
						vic_wse=cur_wse;
						before_vic_wse =LIST_PREV(vic_wse);
						after_vic_wse=LIST_NEXT(vic_wse);
						break;
					}
				}
				uint32* ptr_page_table = NULL;
				struct FrameInfo* vict_ptr = get_frame_info(faulted_env->env_page_directory, victim_va, &ptr_page_table);
				uint32 perms = pt_get_page_permissions(faulted_env->env_page_directory,victim_va);
				//vict_ptr =get_frame_info();
				if(perms&PERM_MODIFIED){
					pf_update_env_page(faulted_env,victim_va,vict_ptr);
				}
				unmap_frame(faulted_env->env_page_directory,victim_va);
				LIST_REMOVE(&faulted_env->page_WS_list,vic_wse);

				//kfree(vic_wse);
				struct FrameInfo *ptr_new_frame = NULL;
				int frame_alloc_ret = allocate_frame(&ptr_new_frame);
				map_frame(faulted_env->env_page_directory,ptr_new_frame,fault_va,PERM_USER | PERM_WRITEABLE|PERM_PRESENT);

				int page_r = pf_read_env_page(faulted_env,(void*)fault_va);

				if(page_r == E_PAGE_NOT_EXIST_IN_PF){

						if(!((fault_va>=USER_HEAP_START &&fault_va<USER_HEAP_MAX) ||(fault_va>=USTACKBOTTOM&&fault_va<USTACKTOP)))
						{
							env_exit();
						}
				}
				struct WorkingSetElement *nwse = env_page_ws_list_create_element(faulted_env,fault_va);
				if(before_vic_wse==NULL){
					LIST_INSERT_HEAD(&faulted_env->page_WS_list,nwse);

					faulted_env->page_last_WS_element = after_vic_wse;
				}
				else if(after_vic_wse==NULL){
					LIST_INSERT_TAIL(&faulted_env->page_WS_list,nwse);
					faulted_env->page_last_WS_element = LIST_FIRST(&faulted_env->page_WS_list);
				}
				else{
					LIST_INSERT_AFTER(&faulted_env->page_WS_list,before_vic_wse,nwse);
					faulted_env->page_last_WS_element = after_vic_wse;
				}

				/*if(LIST_SIZE(&faulted_env->page_WS_list)==faulted_env->page_WS_max_size)
					faulted_env->page_last_WS_element = LIST_FIRST(&faulted_env->page_WS_list);
				else
					faulted_env->page_last_WS_element=NULL;*/

							//env_page_ws_print(faulted_env);
				/*placement_logic(faulted_env,fault_va);
				uint32 new_va = ROUNDDOWN(fault_va,PAGE_SIZE);
				int val = pf_read_env_page(faulted_env,(void*)new_va);
				if(val == E_PAGE_NOT_EXIST_IN_PF){
					struct FrameInfo* new_fr_ptr = NULL;
					allocate_frame(&new_fr_ptr);
					if(new_fr_ptr==NULL){panic("failure: out of mem");}
					map_frame()
				}
				*/
				//panic("page_fault_handler().REPLACEMENT is not implemented yet...!!");
			}
			else if (isPageReplacmentAlgorithmLRU(PG_REP_LRU_TIME_APPROX))
			{
				// TODO: [PROJECT'25.IM#6] FAULT HANDLER II - #2 LRU Aging Replacement
				// Your code is here
				// Comment the following line
				// panic("page_fault_handler().REPLACEMENT is not implemented yet...!!");


				struct WorkingSetElement* victim_wse = NULL;
				struct WorkingSetElement* cur_wse = NULL;
				uint32 min_timestamp = 0xFFFFFFFF;   // Start with maximum possible

				LIST_FOREACH(cur_wse, &(faulted_env->page_WS_list))
				{
					// Select the page with the minimum timestamp ---- least recently used
					if (cur_wse->time_stamp < min_timestamp)
					{
						min_timestamp = cur_wse->time_stamp;
						victim_wse = cur_wse;
					}
				}

				// Victim virtual address
				uint32 victim_va = victim_wse->virtual_address;


				uint32* ptr_page_table = NULL;
				struct FrameInfo* victim_frame =
				get_frame_info(faulted_env->env_page_directory, victim_va, &ptr_page_table);

				uint32 perms = pt_get_page_permissions(faulted_env->env_page_directory, victim_va);

				// If modified --- save to page file before eviction
				if (perms & PERM_MODIFIED)
				{
					pf_update_env_page(faulted_env, victim_va, victim_frame);
				}


				unmap_frame(faulted_env->env_page_directory, victim_va);


				LIST_REMOVE(&(faulted_env->page_WS_list), victim_wse);
				kfree(victim_wse);   // Free the WS element structure


				uint32 fault_va_pg = ROUNDDOWN(fault_va, PAGE_SIZE);


				struct FrameInfo* new_frame = NULL;
				int alloc_ret = allocate_frame(&new_frame);
				if (alloc_ret != 0)
				{
					panic("page_fault_handler: failed to allocate frame for LRU replacement");
				}


				map_frame(faulted_env->env_page_directory, new_frame, fault_va_pg,
					PERM_USER | PERM_WRITEABLE | PERM_PRESENT);


				int read_ret = pf_read_env_page(faulted_env, (void*)fault_va_pg);

				// If page does not exist in page file ---- ensure it belongs to heap or stack
				if (read_ret == E_PAGE_NOT_EXIST_IN_PF)
				{
					if (!((fault_va_pg >= USER_HEAP_START && fault_va_pg < USER_HEAP_MAX) ||
						(fault_va_pg >= USTACKBOTTOM && fault_va_pg < USTACKTOP)))
					{
						env_exit();   // Invalid access --- kill environment
					}
				}


				struct WorkingSetElement* new_wse =
					env_page_ws_list_create_element(faulted_env, fault_va_pg);

				// Newly inserted page gets timestamp = 0
				// (it will increase in update_WS_time_stamps())
				new_wse->time_stamp = 0;

				// Add it to tail of WS
				LIST_INSERT_TAIL(&(faulted_env->page_WS_list), new_wse);
			}
			else if (isPageReplacmentAlgorithmModifiedCLOCK())
			{
				// TODO: [PROJECT'25.IM#6] FAULT HANDLER II - #3 Modified Clock Replacement
				// Your code is here
				// Comment the following line
				//panic("page_fault_handler().REPLACEMENT is not implemented yet...!!");


				if (faulted_env->page_last_WS_element == NULL)
					faulted_env->page_last_WS_element = LIST_FIRST(&faulted_env->page_WS_list);

				struct WorkingSetElement* victim_wse = NULL;
				struct WorkingSetElement* start = faulted_env->page_last_WS_element;
				struct WorkingSetElement* cur = start;
				uint32 victim_va = 0;
				int found = 0;


				do {
					uint32 va = cur->virtual_address;
					uint32 perms = pt_get_page_permissions(faulted_env->env_page_directory, va);

					// (0,0) --- perfect victim
					if (!(perms & PERM_USED) && !(perms & PERM_MODIFIED)) {
						victim_wse = cur;
						found = 1;
						break;
					}

					// If (1,0) --- clear USED bit
					if ((perms & PERM_USED) && !(perms & PERM_MODIFIED)) {
						pt_set_page_permissions(faulted_env->env_page_directory, va, 0, PERM_USED);
					}

					// Move hand
					cur = LIST_NEXT(cur);
					if (cur == NULL) cur = LIST_FIRST(&faulted_env->page_WS_list);

				} while (cur != start);


				if (!found)
				{
					cur = start;
					do {
						uint32 va = cur->virtual_address;
						uint32 perms = pt_get_page_permissions(faulted_env->env_page_directory, va);

						// (0,1) --- second-best victim
						if (!(perms & PERM_USED) && (perms & PERM_MODIFIED)) {
							victim_wse = cur;
							found = 1;
							break;
						}

						// (1,1) --- clear USED so it may become victim in next pass
						if ((perms & PERM_USED) && (perms & PERM_MODIFIED)) {
							pt_set_page_permissions(faulted_env->env_page_directory, va, 0, PERM_USED);
						}

						// Move hand
						cur = LIST_NEXT(cur);
						if (cur == NULL) cur = LIST_FIRST(&faulted_env->page_WS_list);

					} while (cur != start);
				}


				if (!found)
				{
					cur = start;
					do {
						uint32 va = cur->virtual_address;
						uint32 perms = pt_get_page_permissions(faulted_env->env_page_directory, va);

						if (!(perms & PERM_USED) && !(perms & PERM_MODIFIED)) {
							victim_wse = cur;
							found = 1;
							break;
						}

						cur = LIST_NEXT(cur);
						if (cur == NULL) cur = LIST_FIRST(&faulted_env->page_WS_list);

					} while (cur != start);
				}


				if (!found)
				{
					cur = start;
					do {
						uint32 va = cur->virtual_address;
						uint32 perms = pt_get_page_permissions(faulted_env->env_page_directory, va);

						if (!(perms & PERM_USED)) {
							victim_wse = cur;
							found = 1;
							break;
						}

						cur = LIST_NEXT(cur);
						if (cur == NULL) cur = LIST_FIRST(&faulted_env->page_WS_list);

					} while (cur != start);
				}


				if (!found || victim_wse == NULL)
					panic("Modified CLOCK: No victim found!");

				victim_va = victim_wse->virtual_address;


				faulted_env->page_last_WS_element = LIST_NEXT(victim_wse);
				if (faulted_env->page_last_WS_element == NULL)
					faulted_env->page_last_WS_element = LIST_FIRST(&faulted_env->page_WS_list);


				uint32* ptr_table = NULL;
				struct FrameInfo* victim_frame =
					get_frame_info(faulted_env->env_page_directory, victim_va, &ptr_table);

				uint32 perms = pt_get_page_permissions(faulted_env->env_page_directory, victim_va);
				if (perms & PERM_MODIFIED)
					pf_update_env_page(faulted_env, victim_va, victim_frame);


				unmap_frame(faulted_env->env_page_directory, victim_va);
				LIST_REMOVE(&faulted_env->page_WS_list, victim_wse);
				kfree(victim_wse);


				uint32 new_va = ROUNDDOWN(fault_va, PAGE_SIZE);

				struct FrameInfo* new_frame = NULL;
				allocate_frame(&new_frame);

				map_frame(faulted_env->env_page_directory, new_frame, new_va,
					PERM_USER | PERM_WRITEABLE | PERM_PRESENT);

				// Read content from page file if exists
				int ret = pf_read_env_page(faulted_env, (void*)new_va);
				if (ret == E_PAGE_NOT_EXIST_IN_PF)
				{
					if (!((new_va >= USER_HEAP_START && new_va < USER_HEAP_MAX) ||
						(new_va >= USTACKBOTTOM && new_va < USTACKTOP)))
						env_exit();
				}

				struct WorkingSetElement* new_wse =
					env_page_ws_list_create_element(faulted_env, new_va);

				LIST_INSERT_TAIL(&(faulted_env->page_WS_list), new_wse);

			}

		}
	}
#endif
}


void __page_fault_handler_with_buffering(struct Env * curenv, uint32 fault_va)
{
	panic("this function is not required...!!");
}
