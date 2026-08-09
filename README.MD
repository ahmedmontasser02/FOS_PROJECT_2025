# FOS — Educational Operating System (Course Project)

Course: Operating Systems (OS) — College Project

Semester: Semester 1 - 2025

Team: [ahmedmontasser02](https://github.com/ahmedmontasser02) • [abdelrahman-hridy](https://github.com/abdelrahman-hridy) • [Abdullah-Yossry](https://github.com/Abdullah-Yossry) • [xmope](https://github.com/xmope) • [khaledMubarak](https://github.com/khaledMubarak) • [AhmedWalaa36](https://github.com/AhmedWalaa36)

Instructor: Ahmed Salah

Overview
--------

This repository contains an educational, small teaching operating system (kernel + user programs) implemented in C and x86 assembly. The project demonstrates classic OS concepts such as:

- process scheduling and priorities
- concurrency and synchronization (spinlocks, semaphores, sleeplocks, channels)
- memory management (boot memory manager, paging, working sets, page replacement tests)
- kernel heap and dynamic allocator
- user-space programs and a small runtime library (libfos)
- boot and run under the Bochs x86 emulator (configuration included)

The codebase is organized so that the kernel lives in `kern/`, user-space programs and test-cases in `user/`, shared runtime/library code in `lib/`, and headers in `inc/`.

Notable learning objectives
- Understand and modify a simple kernel's scheduler and memory manager
- Implement and validate synchronization primitives and inter-process communication
- Build and boot the system in an emulator and exercise user-level test programs

What's in this repository
-------------------------

- [kern/](A:/11/FOS_PROJECT_2025_TEMPLATE/kern/): kernel source (C and .S), kernel linker script, and kernel-side tests
- [user/](A:/11/FOS_PROJECT_2025_TEMPLATE/user/): user programs and many small test programs (sorting, fib, page-replacement, allocator tests, concurrency tests, etc.)
- [lib/](A:/11/FOS_PROJECT_2025_TEMPLATE/lib/): runtime library used by user programs (libfos)
- [inc/](A:/11/FOS_PROJECT_2025_TEMPLATE/inc/): public headers and architecture definitions
- [boot/](A:/11/FOS_PROJECT_2025_TEMPLATE/boot/): boot loader sources
- .bochsrc: Bochs configuration (included and preconfigured for this image)
- GNUmakefile and sub-Makefrag fragments: full build system (cross-compiler-friendly)

Important components and highlights
- Scheduler: kern/cpu/sched.c + helpers in kern/cpu/sched_helpers.c
- Memory: kern/mem/* (memory_manager, paging_helpers, working_set_manager, kheap)
- Concurrency: kern/conc/* (kspinlock, sleeplock, channel, ksemaphore)
- Disk / pagefile manager: kern/disk/pagefile_manager.c
- Tests: many test files in kern/tests/ and user/ (prefix `tst_` or `tst_`/`test_`)

Build prerequisites
-------------------

Minimum tools (tested workflow):
- GNU make
- i386-elf cross toolchain (i386-elf-gcc, i386-elf-ld, i386-elf-as, objcopy, objdump) OR a host toolchain capable of producing 32-bit i386 ELF outputs (see notes below)
- Bochs (to run the emulated image)
- standard Unix tools: dd, rm, mkdir

Notes on toolchain options:
- Preferred: install an i386-elf cross-compiler (packaged as `i386-elf-gcc` and related binutils). This repository's GNUmakefile defaults to `i386-elf-*` tools.
- Alternative: if your host GCC produces 32-bit ELF (common on many Linux boxes with multilib), you can configure conf/env.mk and set GCCPREFIX to an empty string (or adapt CC/AS/LD variables). See conf/env.mk for hints.
- On Windows, use WSL (recommended) or MSYS2/MinGW with multilib support. The included .bochsrc and batch scripts may help but WSL gives the most straightforward Linux-like environment.

Quick build & run
-----------------

1. Open a terminal and change to the repository root:

   cd A:/11/FOS_PROJECT_2025_TEMPLATE

2. (Optional) Edit conf/env.mk if you need to change GCCPREFIX or other variables. By default the makefiles try to use `i386-elf-*` toolchain.

3. Build the entire system (creates the Bochs disk image):

   make

   The top-level make will build the bootloader, kernel, user binaries, and then assemble a disk image at `obj/kern/bochs.img`.

4. Run the image in Bochs (the Makefile provides a convenience target):

   make bochs

   By default the `bochs` target runs Bochs with `display_library: nogui`. To run interactively, run `bochs -q` yourself or edit `.bochsrc` to change display options.

Useful make targets
- make          — build everything (image produced as obj/kern/bochs.img)
- make bochs    — run Bochs (uses the image built above)
- make clean    — remove build artifacts (obj/)
- make realclean — deeper clean (also removes bochs logs and lab tarballs)

Running / testing individual programs
-------------------------------------

- Many user-level programs (sorting, fib, allocator tests, page replacement tests, concurrency tests, etc.) live in `user/` and are compiled and linked into the disk image when you run `make`.
- To run a particular user test, boot the image in Bochs and use the kernel's command prompt to start the available user program. The kernel includes a small command/console implementation in `kern/cmd/` (`command_prompt.c`) that can be used to run tests. See `kern/cmd/commands.c` for which commands are exposed by the kernel.
- The build system contains commented helper targets (see the `run-%` example in `GNUmakefile`) for building alternative test runs; advanced users can modify make variables or the kernel's DEFS to enable single-test images.

Selected user programs and tests (examples)
- Sorting & algorithms: user/arrayOperations_*, mergesort_*, quicksort_*
- Scheduling / priority examples: priRR_fib* and related
- Memory / paging tests: many `tst_page_replacement_*` and `tst_placement.c`
- Concurrency & synchronization: `tst_ksemaphore_*`, `tst_sharing_*`, `tst_sleeplock_*`
- Utility demos: fos_helloWorld.c, fos_input.c, game.c

For a complete list of user programs, see the [user/](A:/11/FOS_PROJECT_2025_TEMPLATE/user/) directory.

Project structure (top-level)
-----------------------------
- [boot/](A:/11/FOS_PROJECT_2025_TEMPLATE/boot/)      — bootloader sources
- [kern/](A:/11/FOS_PROJECT_2025_TEMPLATE/kern/)      — kernel sources, kernel linker script
- [lib/](A:/11/FOS_PROJECT_2025_TEMPLATE/lib/)       — user runtime library sources
- [user/](A:/11/FOS_PROJECT_2025_TEMPLATE/user/)     — user programs and tests
- [inc/](A:/11/FOS_PROJECT_2025_TEMPLATE/inc/)       — common headers and definitions
- [obj/](A:/11/FOS_PROJECT_2025_TEMPLATE/obj/)       — build artifacts (created by make)
- [.bochsrc] — emulator configuration used by `make bochs`
- [GNUmakefile] — top-level makefile (includes sub-Makefrag fragments)

Design notes & mapping to code (quick)
- Scheduling: `kern/cpu/sched.c` + `kern/cpu/sched_helpers.c` (priority manager in `proc/priority_manager.c`)
- Memory management: `kern/mem/*` (boot memory manager, kheap, paging helpers, working set manager)
- Synchronization primitives: `kern/conc/*` (kspinlock, sleeplock, ksemaphore, channel)
- Kernel-to-user boundary: `trap/syscall.c`, `trap/trapentry.S`, and the user runtime in `lib/`

Testing & grading notes
-----------------------
- The repository includes many small tests (kernel-side and user-side). Most tests are executed by starting the corresponding user program from the kernel console.
- For automated grading or scripted tests, the Makefile has convenience hooks (see `grade` and lab/handin related targets in `GNUmakefile` and `conf/`).

Known issues / portability notes
--------------------------------
- The build system expects an x86 32-bit toolchain by default (`i386-elf-*`). If your host toolchain differs, adapt `conf/env.mk` or set `GCCPREFIX` in the environment.
- Many Makefrag fragments show commented TODOs about incremental builds — a full `make clean` before experimenting with toolchain changes is recommended if you see stale builds.

Credits & contributors
----------------------
This project and repository structure are based on the conventional teaching OS layout (kernel + lib + user), with tests and examples included for course labs.

Contributors (GitHub handles):
- https://github.com/ahmedmontasser02
- https://github.com/abdelrahman-hridy
- https://github.com/Abdullah-Yossry
- https://github.com/xmope
- https://github.com/khaledMubarak
- https://github.com/AhmedWalaa36

Licensing & copyright
---------------------
- The repository contains COPYRIGHT files (see `kern/COPYRIGHT` and the top-level files). Check those files for licensing terms.

Further reading
---------------
- Course lecture notes and standard textbooks (Silberschatz, Tanenbaum) are good references for the algorithms and data structures implemented here.
- The included `Readme_Project_Port_To_ModernGCC.txt` contains notes about GCC/inline/porting tips.

Support / contact
-----------------
- For project-specific questions contact the team members via their GitHub accounts above.
