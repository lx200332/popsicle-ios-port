/*
 * pandora_mem.c -- read-only XNU memory-management observability for the
 *                  Pandora (sweet / SWEET, arm64) port.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * drivers/pandora/ carries sixteen drivers (clocks, thermal, UFS, modem, ADSP,
 * GPU, camera, display, audio, ...) but no memory-management driver at all:
 * nothing in the tree reports what the arm64 VM layer believes it owns, what
 * the pageout targets are, or whether the compressor is live.  pandora_files.c
 * looks related by name but is a "write the files out of the pinned pack into
 * the rootfs" installer; it never touches vm_page_*.
 *
 * This driver is an observer first.  Every value it prints is a plain
 * file-scope global of the XNU VM layer whose definition site is quoted in the
 * comments below, so there is no guessing about what a number means.
 *
 * ANDROID / LINUX CONCEPTS THAT DO NOT APPLY HERE
 * -----------------------------------------------
 * CMA/ION/DMA-BUF 是 Android/Linux 概念；XNU 用 pmap + IOKit
 * IOBufferMemoryDescriptor 承担同等职能；本项目不移植这些。
 *
 * Concretely:
 *   - There is no CMA (contiguous memory allocator) in XNU.  Large physically
 *     contiguous allocations come from pmap_steal_memory()/ml_static_alloc()
 *     at boot, or from IOBufferMemoryDescriptor with kIOMemoryPhysicallyContiguous
 *     behind it.  The "linux,cma" phandle in dts/canoe_base.dts belongs to the
 *     Android side and is not read by this kernel.
 *   - There is no ION allocator.  The two "ion" hits in pandora_adsp.c are the
 *     qcom,msm-audio-ion IOMMU stream ids, i.e. an SMMU translation context,
 *     not an Android graphics buffer allocator.
 *   - There is no DMA-BUF.  Buffer sharing between kernel clients is done with
 *     IOMemoryDescriptor / IOSurface, and the lifetime rules are IOKit's.
 * Porting any of the three would mean porting the Android userspace contract
 * around it, which this project does not have (no iOS/Android userspace here).
 *
 * GATING
 * ------
 * Nothing here writes VM state unless a boot-arg asks for it:
 *   pandora_mem=1                 print the snapshot from pandora_mem_init()
 *   pandora_mem_report=1          print compressor / jetsam state
 *   pandora_mem_target=<MB>       retune vm_page_free_target / _min (WARNING:
 *                                 see the note in pandora_mem_tune())
 * Absent boot-args, this file only defines functions; it prints nothing and
 * changes nothing.
 *
 * NOT REGISTERED IN bsd/conf/files -- deliberately.  This file is built and
 * warning-checked out of tree; wiring it into the real build (and calling
 * pandora_mem_init() from sweet_ufs.c) is a separate, reviewed change.
 */

#if defined(PANDORA)

#include <sys/param.h>
#include <sys/systm.h>

#include <mach/vm_param.h>          /* atop_64(), max_mem            */
#include <vm/vm_pageout.h>          /* vm_pageout_state, VM_PAGER_*  */
#include <pexpert/pexpert.h>        /* PE_parse_boot_argn()          */

/*
 * ============================================================================
 * KERNEL SYMBOLS THIS FILE READS
 * ============================================================================
 * Every symbol is a plain, non-static, file-scope global in the buildtree, so
 * "extern" here matches the definition's linkage and type exactly.  The
 * declarations are repeated rather than #included because the authoritative
 * headers are not on the bsd/dev include path:
 *
 *   osfmk/vm/vm_page.h is NOT exported to
 *   build/xnu.obj/EXPORT_HDRS/osfmk/vm/ (only vm_pageout.h, vm_protos.h,
 *   pmap.h, vm_map.h, ... are), and vm_pageout.h only pulls vm_page.h in
 *   under #ifdef MACH_KERNEL_PRIVATE (osfmk/vm/vm_pageout.h:87, :291), which
 *   this build does not define.  So the counters have to be declared here.
 *
 *   bsd/kern/kern_memorystatus_internal.h declares the memorystatus globals
 *   (lines 57-60) but is wrapped in #if BSD_KERNEL_PRIVATE *and* is not in
 *   EXPORT_HDRS, so it cannot be included from bsd/dev either.
 *
 * Types are `unsigned int` because CONFIG_JETSAM is on for this board
 * (kern_memorystatus.c:774-778 takes the unsigned-int branch; the uint64_t
 * branch at :808-810 is the !CONFIG_JETSAM one).
 */

/* osfmk/vm/vm_page.h:1297 / defined osfmk/vm/vm_resident.c:336 */
extern unsigned int vm_page_free_count;
/* osfmk/vm/vm_page.h:1299 / osfmk/vm/vm_resident.c:428 */
extern unsigned int vm_page_active_count;
/* osfmk/vm/vm_page.h:1301 / osfmk/vm/vm_resident.c:429 */
extern unsigned int vm_page_inactive_count;
/* osfmk/vm/vm_page.h:1344 / osfmk/vm/vm_resident.c:438 */
extern unsigned int vm_page_throttled_count;
/* osfmk/vm/vm_page.h:1346 / osfmk/vm/vm_resident.c:439 */
extern unsigned int vm_page_speculative_count;
/* osfmk/vm/vm_page.h:1352 / osfmk/vm/vm_resident.c:458 */
extern unsigned int vm_page_external_count;
/* osfmk/vm/vm_page.h:1354 / osfmk/vm/vm_resident.c:459 */
extern unsigned int vm_page_internal_count;
/* osfmk/vm/vm_page.h:1356 / osfmk/vm/vm_resident.c:441 */
extern unsigned int vm_page_wire_count;
/* osfmk/vm/vm_page.h:1391 / osfmk/vm/vm_resident.c:453 */
extern unsigned int vm_page_purgeable_count;

/* The three pageout limits pandora_mem_tune() may rewrite. */
/* osfmk/vm/vm_page.h:1362 / osfmk/vm/vm_resident.c:483 */
extern unsigned int vm_page_free_target;
/* osfmk/vm/vm_page.h:1364 / osfmk/vm/vm_resident.c:484 */
extern unsigned int vm_page_free_min;
/* osfmk/vm/vm_page.h:1376 / osfmk/vm/vm_resident.c:491 */
extern unsigned int vm_page_free_reserved;

/*
 * osfmk/vm/vm_page.h:1416 declares vm_compressor_mode; the definition this
 * build links is osfmk/vm/vm_compressor.c:163 (`= VM_PAGER_COMPRESSOR_WITH_SWAP`)
 * because XNU_TARGET_OS_OSX is set and CONFIG_FREEZE is not -- the other two
 * initialisers at :156 (CONFIG_FREEZE) and :159 (!XNU_TARGET_OS_OSX) are not
 * compiled in.
 */
extern int vm_compressor_mode;

/*
 * osfmk/vm/vm_compressor.c:168 and :169.  Neither is declared in any exported
 * header, so the types are copied from the definitions.
 */
extern int vm_compressor_is_active;
extern int vm_compressor_available;

/*
 * bsd/kern/kern_memorystatus_internal.h:57-59 / defined
 * bsd/kern/kern_memorystatus.c:774-776.  `-1` means "not yet computed", which
 * is what a freshly booted kernel shows before the first memorystatus sweep.
 */
extern unsigned int memorystatus_available_pages;
extern unsigned int memorystatus_available_pages_pressure;
extern unsigned int memorystatus_available_pages_critical;

/*
 * bsd/kern/kern_memorystatus_internal.h:60 / bsd/kern/kern_memorystatus.c:805
 */
extern uint32_t jetsam_kill_on_low_swap;

/* ------------------------------------------------------------------------ */

#define PANDORA_MEM_PREFIX "PANDORA MEM"

/* Forward declarations: this build runs -Wmissing-prototypes. */
void pandora_mem_init(void);
void pandora_mem_tune(void);
void pandora_mem_report(void);

static const char *pandora_mem_compressor_name(int mode);

/*
 * Human-readable name for the VM_PAGER_* mode word.  The constants are the
 * ones in osfmk/vm/vm_pageout.h:654-660; in this build the value should be
 * VM_PAGER_COMPRESSOR_WITH_SWAP (0x4).
 */
static const char *
pandora_mem_compressor_name(int mode)
{
	if (mode == VM_PAGER_NOT_CONFIGURED) {
		return "not-configured";
	}
	if (mode == VM_PAGER_DEFAULT) {
		return "default";
	}
	if (mode == VM_PAGER_COMPRESSOR_NO_SWAP) {
		return "compressor-no-swap";
	}
	if (mode == VM_PAGER_COMPRESSOR_WITH_SWAP) {
		return "compressor-with-swap";
	}
	if (mode == VM_PAGER_FREEZER_DEFAULT) {
		return "freezer-default";
	}
	if (mode == VM_PAGER_FREEZER_COMPRESSOR_NO_SWAP) {
		return "freezer-compressor-no-swap";
	}
	if (mode == VM_PAGER_COMPRESSOR_NO_SWAP_PLUS_FREEZER_COMPRESSOR_WITH_SWAP) {
		return "compressor-no-swap+freezer-with-swap";
	}
	return "unknown";
}

/*
 * pandora_mem_init -- one-shot snapshot of the arm64 VM counters.
 *
 * Gated by pandora_mem=1.  Read-only: no assignment happens here.
 *
 * The physical size comes from max_mem (osfmk/mach/vm_param.h:237, defined
 * osfmk/arm64/arm_vm_init.c:189 and assigned at :2253 from mem_size, which
 * arm_vm_init.c:1886 computes as
 *     gPhysSize = mem_size = ((gPhysBase + args->memSize) & ~PAGE_MASK) - gPhysBase;
 * i.e. the page-aligned part of the DRAM window pmap actually took).
 *
 * max_mem_actual (osfmk/mach/vm_param.h:273, defined arm_vm_init.c:191) is the
 * firmware-reported size before that window alignment, and arm_vm_init.c:1892
 * /:1894 assign it: when the `memory_size` boot constraint is present and
 * smaller, both are clamped to it; otherwise max_mem_actual keeps
 * args->memSizeActual (or mem_size when the firmware did not report one).
 * Printing both makes a mismatch between "what the firmware said" and "what
 * pmap took" visible instead of silently choosing one.
 */
void
pandora_mem_init(void)
{
	int enable = 0;
	uint64_t total_pages;

	if (!PE_parse_boot_argn("pandora_mem", &enable, sizeof(enable))) {
		return;
	}
	if (enable == 0) {
		return;
	}

	/* max_mem is in bytes; PAGE_SIZE is 4096 here (see report). */
	total_pages = (uint64_t)atop_64(max_mem);

	printf(PANDORA_MEM_PREFIX ": phys=%llu MB (%llu pages of %u B) "
	    "fw_reported=%llu MB\n",
	    (unsigned long long)(max_mem >> 20),
	    (unsigned long long)total_pages,
	    (unsigned)PAGE_SIZE,
	    (unsigned long long)(max_mem_actual >> 20));
	printf(PANDORA_MEM_PREFIX ": free=%u active=%u inactive=%u spec=%u\n",
	    vm_page_free_count,
	    vm_page_active_count,
	    vm_page_inactive_count,
	    vm_page_speculative_count);
	printf(PANDORA_MEM_PREFIX ": wire=%u throttled=%u purgeable=%u\n",
	    vm_page_wire_count,
	    vm_page_throttled_count,
	    vm_page_purgeable_count);
	printf(PANDORA_MEM_PREFIX ": internal=%u external=%u\n",
	    vm_page_internal_count,
	    vm_page_external_count);
	printf(PANDORA_MEM_PREFIX ": target=%u min=%u reserved=%u\n",
	    vm_page_free_target,
	    vm_page_free_min,
	    vm_page_free_reserved);
}

/*
 * pandora_mem_tune -- move vm_page_free_target / vm_page_free_min to a page
 *                     count derived from pandora_mem_target=<MB>.
 *
 * WHY THIS IS OPT-IN AND WHY IT PRINTS A WARNING
 * ----------------------------------------------
 * Both variables are writable globals, but osfmk/vm/vm_pageout.c:3831
 * vm_page_free_reserve() recomputes them from scratch out of
 * vm_pageout_state.vm_page_free_count_init and vm_page_free_reserved
 * (recompute block at :3849-3869), and it is called at runtime by
 * osfmk/kern/thread.c:2294 (+1 page), osfmk/kern/thread.c:2299 (-1 page),
 * osfmk/kern/thread_call.c:1955 (+2 pages) and osfmk/vm/vm_pageout.c:5233
 * (the per-CPU reservation delta).  So a value written here survives only
 * until the next thread is created or destroyed.  There is no upstream hook to
 * make a custom target stick, which is exactly why this driver reports the
 * overwrite risk instead of pretending the write is durable.
 *
 * The write is also kept inside the invariants vm_page_free_reserve() itself
 * enforces: free_target must exceed free_min + 5 (:3866), and both are
 * unsigned so a target above the number of free pages is refused.
 */
void
pandora_mem_tune(void)
{
	int want_mb = 0;
	uint64_t want_pages;
	unsigned int new_target;
	unsigned int new_min;
	unsigned int old_target;
	unsigned int old_min;

	if (!PE_parse_boot_argn("pandora_mem_target", &want_mb, sizeof(want_mb))) {
		return;
	}
	if (want_mb <= 0) {
		printf(PANDORA_MEM_PREFIX ": target=%d MB ignored (need > 0)\n",
		    want_mb);
		return;
	}

	want_pages = (uint64_t)atop_64((uint64_t)want_mb * 1024ULL * 1024ULL);

	/*
	 * Keep free_target > free_min + 5 as vm_pageout.c:3866 requires, and
	 * refuse values that exceed the pages we actually have.
	 */
	new_min = (unsigned int)(want_pages / 2);
	if (new_min < 8) {
		new_min = 8;
	}
	if (want_pages <= (uint64_t)new_min + 5ULL) {
		printf(PANDORA_MEM_PREFIX ": target=%d MB -> %llu pages is too "
		    "small for a target/min pair, ignored\n",
		    want_mb, (unsigned long long)want_pages);
		return;
	}
	if (want_pages > (uint64_t)vm_page_free_count) {
		printf(PANDORA_MEM_PREFIX ": target=%d MB -> %llu pages exceeds "
		    "free=%u, ignored\n",
		    want_mb, (unsigned long long)want_pages, vm_page_free_count);
		return;
	}
	new_target = (unsigned int)want_pages;

	old_target = vm_page_free_target;
	old_min = vm_page_free_min;

	vm_page_free_target = new_target;
	vm_page_free_min = new_min;

	printf(PANDORA_MEM_PREFIX ": tune target %u -> %u, min %u -> %u "
	    "(from pandora_mem_target=%d)\n",
	    old_target, vm_page_free_target, old_min, vm_page_free_min,
	    want_mb);
	printf(PANDORA_MEM_PREFIX ": tune WARNING vm_page_free_reserve() "
	    "recomputes both from vm_pageout_state on every thread create/exit "
	    "(osfmk/vm/vm_pageout.c:3849-3869), so this value is not durable; "
	    "persisting it needs an upstream change\n");
}

/*
 * pandora_mem_report -- compressor + jetsam state, one boot-arg away.
 *
 * Gated by pandora_mem_report=1.  Read-only.
 *
 * vm_memory_pressure lives in struct vm_pageout_state
 * (osfmk/vm/vm_pageout.h:705, the struct itself at :703-714, defined
 * osfmk/vm/vm_pageout.c:320).
 */
void
pandora_mem_report(void)
{
	int enable = 0;

	if (!PE_parse_boot_argn("pandora_mem_report", &enable, sizeof(enable))) {
		return;
	}
	if (enable == 0) {
		return;
	}

	printf(PANDORA_MEM_PREFIX ": compressor mode=%d (%s) active=%d "
	    "available=%d\n",
	    vm_compressor_mode,
	    pandora_mem_compressor_name(vm_compressor_mode),
	    vm_compressor_is_active,
	    vm_compressor_available);
	printf(PANDORA_MEM_PREFIX ": pressure=%u count_init=%d\n",
	    vm_pageout_state.vm_memory_pressure,
	    vm_pageout_state.vm_page_free_count_init);
	printf(PANDORA_MEM_PREFIX ": jetsam avail=%u pressure=%u critical=%u "
	    "low_swap=%u\n",
	    memorystatus_available_pages,
	    memorystatus_available_pages_pressure,
	    memorystatus_available_pages_critical,
	    (unsigned)jetsam_kill_on_low_swap);
	printf(PANDORA_MEM_PREFIX ": page size=%u B shift=%d\n",
	    (unsigned)PAGE_SIZE,
	    (int)PAGE_SHIFT);
}

#else /* !PANDORA */

/*
 * Non-Pandora configs build the same TU elsewhere in the tree only if the file
 * is added to bsd/conf/files; keep the translation unit non-empty so an
 * accidental inclusion still compiles.
 */
typedef int pandora_mem_unused_t;

#endif /* PANDORA */
