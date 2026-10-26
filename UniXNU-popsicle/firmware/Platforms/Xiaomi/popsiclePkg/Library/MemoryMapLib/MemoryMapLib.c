#include <Library/MemoryMapLib.h>

//
// Xiaomi 17 Pro Max (popsicle) - SM8850 / canoe / kaanapali
//
// DDR layout taken from the stock firmware:
//   xbl_config.img  -> CFGL / post-ddr.dtbs.bin -> /soc/memorymap
// 28 regions, verified contiguous from 0xC4800000 through 0xD7800000.
//
// Differences against OnePlus 15 (infiniti), same SoC:
//   * low DDR window is described as NOMAP/RSRV0/XBL_DT/AOP/UEFI_Log
//     instead of HYP/RSRV0/PDP/RSRV1/TME
//   * UEFI_FD occupies 0xC6800000 + 0x400000 (no leading FD_Reserved)
//     which matches the entry point of the stock uefi.img ELF
//   * CPU_Vectors is called out explicitly at 0xC6E00000
// Everything from 0xC6E01000 to 0xFC800000+0x02B00000 is identical.
//
// Regions marked [dtb] were added on top of the infiniti-derived set because the vendor device tree
// (/reserved-memory in kaanapali-xiaomi-popsicle.dtb) reserves them and the inherited map did not
// describe them. Without an entry here they would reach DXE as ordinary free memory and the allocator
// would eventually hand out a page belonging to a DSP image, the hypervisor or TrustZone.
// scripts/check-popsicle.py recomputes this comparison: every non-zero /reserved-memory node has to
// fall inside a descriptor that is not Conventional memory.
//
STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceAttribute, ArmAttributes, ResourceType, MemoryType

  // DDR Regions
  {"NOMAP",              0x80000000, 0x01960000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"RSRV0",              0x81960000, 0x000A0000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"XBL_DT",             0x81A00000, 0x00060000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"AOP",                0x81C00000, 0x000A0000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"AOP_TME_UEFI_MERGED", 0x81CA0000, 0x00044000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN}, // [dtb]
  {"UEFI_Log",           0x81CE4000, 0x00010000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
  {"SMEM",               0x81D00000, 0x00200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"PDP_NS",             0x81F00000, 0x00100000, AddMem, MEM_RES, UNCACHEABLE, Reserv, NS_DEVICE},
  {"CPUCP_SCANDUMP",     0x82000000, 0x00380000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN}, // [dtb]
  {"ADSP_MHI",           0x82380000, 0x00020000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN}, // [dtb]
  {"SOCP_SDI",           0x823A0000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN}, // [dtb]
  {"PMIC_MINI_DUMP",     0x823E0000, 0x00080000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN}, // [dtb]
  {"QUP_FW",             0x82460000, 0x00018000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN}, // [dtb]
  {"PvmFw",              0x824A0000, 0x00100000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"TZ_STAT",            0x82700000, 0x00100000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN}, // [dtb]
  {"QDSS_APPS",          0x82800000, 0x02000000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN}, // [dtb]
  {"DSM_PARTITION_1",    0x84A00000, 0x05500000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN}, // [dtb]
  {"DSM_PARTITION_2",    0x89F00000, 0x00A80000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN}, // [dtb]
  {"PIL_Reserved",       0x8AA00000, 0x1D300000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Display_Demura",     0xA7D00000, 0x02C80000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"DBI_Dump",           0xAA980000, 0x05770000, NoHob,  MMAP_IO, INITIALIZED, Conv,   UNCACHED_UNBUFFERED_XN},
  {"DXE_Heap",           0xB00F0000, 0x131A0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"HYPERVISOR_EXT_HEAP", 0xC3DD0000, 0x00A30000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN}, // [dtb]
  {"HLOS1",              0xC4800000, 0x02000000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"UEFI_FD",            0xC6800000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI_FD_Reserved",   0xC6C00000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"CPU_Vectors",        0xC6E00000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Info_Blk",           0xC6E01000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
  {"MMU_PageTables",     0xC6E02000, 0x00003000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Log_Buffer",         0xC6E05000, 0x00008000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
  {"UEFI_Stack",         0xC6E0D000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"SEC_Heap",           0xC6E4D000, 0x0008C000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Sched_Heap",         0xC6ED9000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"FV_Region",          0xC72D9000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"UEFI_RESV",          0xC76D9000, 0x00127000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Kernel",             0xC7800000, 0x10000000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"XBL_Ramdump",        0xD7800000, 0x00200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Global_SYNC",        0xD7A00000, 0x00200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"TZ_MERGED",          0xD8000000, 0x00800000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN}, // [dtb]
  {"TRUST_UI_VM",        0xF3800000, 0x05000000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN}, // [dtb]
  {"OEM_VM",             0xF8800000, 0x03800000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN}, // [dtb]
  {"SHM",                0xFC200000, 0x00400000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN}, // [dtb]
  {"Display_Reserved",   0xFC800000, 0x02B00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"DUMP_DISPLAY",       0xFF3FF000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN}, // [dtb] rounded down to a page
  {"RAMOOPS",            0xFF400000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN}, // [dtb]
  {"LLCC_LPI",           0xFF800000, 0x00800000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN}, // [dtb]
  // 34 GiB. The device tree marks lcp_dare_region reusable, which a 16 GiB handset should not have
  // at that address, so whether it is DRAM or an aperture is unresolved. Reserving it is correct
  // either way: nothing is allocated there, and if it is real the kernel is not told to use it.
  {"LCP_DARE",          0x880000000, 0x06000000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN}, // [dtb] above the 4 GiB boundary

  // Other Memory Regions
  {"IMEM_Cookie_Base",   0x14680000, 0x00001000, AddDev, MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},

  // Register Regions
  {"IPC_ROUTER_TOP",     0x01100000, 0x00400000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SECURITY_CONTROL",   0x00780000, 0x00007000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"QUP",                0x00800000, 0x00300000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"QUP1",               0x01900000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PRNG_CFG_PRNG",      0x010C0000, 0x0000C000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"CRYPTO0_CRYPTO",     0x01DC0000, 0x00040000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"TCSR_TCSR_REGS",     0x01F00000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PERIPH_SS",          0x08800000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"USB",                0x0A600000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"AOSS",               0x0B000000, 0x04000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"TLMM",               0x0F000000, 0x01000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SMMU",               0x15000000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"APSS_HM",            0x17000000, 0x08000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  // Pass Data
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
