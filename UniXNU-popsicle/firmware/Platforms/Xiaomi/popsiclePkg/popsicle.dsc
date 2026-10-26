##
#
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = popsicle
  PLATFORM_GUID                  = 7B4E1C62-9A3D-4F18-B0E7-5C2A8D41F963
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/popsiclePkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = popsiclePkg/popsicle.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

  #
  # 0 = SM8850-AC
  # 1 = SM8850-5-AC
  # 2 = SM8850-1-AD
  #
  SOC_TYPE                       = 0

!include KaanapaliPkg/KaanapaliPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0xC6E0D000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"17 Pro Max"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"popsicle"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"2509FPN0BC"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"popsicle"

  #
  # Simple Frame Buffer
  # Panel: nt37801 wqhd_plus (main), 976x596 secondary not supported yet
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1200
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2608
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|popsiclePkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|popsiclePkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

  #
  # UniXNU
  #
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf

[Components]
  #
  # UniXNU: the boot manager application, plus the driver that registers it as a
  # Boot#### option so BDS starts it. Both are packed into FVMAIN by popsicle.fdf.
  # Without them the firmware comes up to the plain UEFI boot manager and the
  # payload volume (EFI/UniXNU/unixnu.toml + kernel.macho) is never read, so the
  # [payload] and ADT steps would produce a volume nothing consumes.
  #
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf

  #
  # DarwinBoot is listed so it keeps compiling against the same libraries, but
  # popsicle.fdf does not pack it. The reference platform does the same: sheng.dsc
  # names it under [Components] and sheng.fdf never includes it in a volume. Packing
  # it would put a second kernel-entry path in the firmware with nothing selecting
  # between them.
  #
  DarwinBootPkg/Driver/DarwinBoot.inf

  #
  # ACPI Tables
  #
  #popsicle/AcpiTables.inf
