# UniXNU-popsicle

Apple-silicon-free boot of Darwin on the **Xiaomi 17 Pro Max** (`popsicle`, model 2509FPN0BC),
Qualcomm **SM8850** / platform codename **canoe** (Snapdragon 8 Elite Gen 5).

This is a device repository for [UniXNU](https://github.com/UniXNU/UniXNU): it supplies the board
description, the kernel defconfig, the Apple-format device tree generator and the memory map that the
Mu-Silicium firmware volume needs. The framework itself (XNU sources, drivers, kits, the `unixnu`
driver) lives in the main repository.

Status: **first bring-up**. Firmware builds, the boot image packs and verifies, nothing has run on
hardware yet (`boot.json` carries `"hardware_tested": false`).

---

## Hardware

| | |
|---|---|
| SoC | SM8850, platform `canoe` |
| CPU | 8 cores — 6 × 3.63 GHz (MPIDR `0x000`–`0x500`), 2 × 4.61 GHz (MPIDR `0x10000`, `0x10100`) |
| GPU | Adreno 840 |
| RAM | 16 GB LPDDR5X |
| Storage | 512 GB UFS, A/B partitions |
| Display | 1200 × 2608 primary (`p2_42_0d_0b_mp_prim_dsc_cmd`), 976 × 596 secondary |
| GIC | distributor `0x17000000`, redistributors `0x17080000` + 2 MiB |
| UART | `0xa9c000` @ 115200n8 (GENI SE1) |
| UFS host controller | `0x1d84000` + 0x3000, PHY `0x1d80000` + 0x2000 |
| Timer | 19.2 MHz |

Bootloader is unlocked (`ro.boot.flash.locked=0`, `verifiedbootstate=orange`), so `fastboot boot`
works without flashing anything.

## Layout

```
board.toml                  board description: IPSW identity, kernel archives, firmware, payload
configs/popsicle_defconfig  kernel feature selection
assets/vendor-memorymap.json    the 28-region memorymap XBL uses, decoded from xbl_config.img
scripts/make-popsicle-adt.py    Apple-format device tree generator (ADT) for DarwinBoot
scripts/make-popsicle-trustcache.py  the two-module trust cache bundle DarwinBoot loads, from the IPSW
scripts/check-popsicle.py       preflight: memory map vs. device tree vs. vendor memorymap vs. FV
scripts/check-build-guards.py   will the build script refuse this configuration?
scripts/check-defconfig-symbols.py  is every configured symbol read at all, or is one a typo?
scripts/stage-popsicle-patches.py  puts patches/ where firmware.py looks, refusing unusable ones
scripts/check-patches-at-pin.py  proves patches/ applies to the edk2 revision unixnu.lock pins
scripts/check-references.py     does every path board.toml names exist in the filesystem it is used from?
scripts/explain-firmware-parity.py  why the two firmware build drivers produce the same volume
scripts/verify-bootimg.py       walks boot.img down to the FFS files and proves UniXNU is inside
scripts/verify-prelink.py       checks [kernel.prelink] against the real kernelcache: sha, address, roots
scripts/verify-adt.py           decodes the generated ADT back and checks its structure
scripts/supply-firmware.py      assembles the Mu-Silicium checkout this board builds against
scripts/patch-unixnu-kernelcache.py  lets ipsw.py use lzfse where compression_tool is missing
scripts/build-arm-driver.py     generic driver compiler against the real source-kernel ABI
scripts/patch-xnu-entry-probe.py  boot-entry probe / panic-screen patcher
unixnu.lock                 pinned upstream revisions
firmware/                   the additions Mu-Silicium needs for this board, laid out as its tree
```

`drivers/`, `kits/`, `package/` are empty placeholders. `package/` is where board-specific glue goes
(sheng keeps its GMU, touch and GPU code there); popsicle has none yet. `patches/` holds six EDK II
patches the `edk2` pin needs — see `KERNEL-BUILD.md` for why all six and not just `Gic-Spurious`.

The firmware side of this port is not upstream Mu-Silicium — it is `firmware/`, a partial tree whose
paths mirror a checkout (`Platforms/Xiaomi/popsiclePkg/`, `Resources/Configs/popsicle.toml`,
`BootShim/BootShim.S`, and the two panel descriptions under `Binaries/popsicle/RawFiles/`).
`scripts/supply-firmware.py` assembles a complete checkout from the pinned upstream tarball plus this
overlay, which is how both the workflow and a fresh local build get one. See [Build](#build).

## Build

The firmware side starts from a Mu-Silicium checkout that carries `Platforms/Xiaomi/popsiclePkg`,
`Resources/Configs/popsicle.toml`, the cache-flushing `BootShim/BootShim.S` and
`Binaries/popsicle`. None of that is upstream, so `scripts/supply-firmware.py` assembles it from the
pinned tarball plus `firmware/`:

```sh
python3 scripts/supply-firmware.py                 # -> ./.cache/firmware, prints its SOURCE_DATE_EPOCH
```

It downloads the Mu-Silicium tarball at `[firmware.supply] commit`, its submodules at the refs
`.gitmodules` names, `Binaries/popsicle` from Device-Binaries at the pinned commit, then copies
`firmware/` over the result and derives `Resources/MuPatches/*.patch` and
`Resources/DTBs/popsicle.dtb` from `patches/` and `FdtBlob/`. It commits once, dated
`commit_time`, which is what makes the volume byte-reproducible; tarballs are cached under
`.cache/tarballs/`, so a second run is quick and a cold one takes about four minutes.
`--source-tarball PATH` skips the download entirely.

Then build the volume and the boot image:

```sh
UE=./.cache/firmware
UNIXNU_UEFI=$UE ./unixnu build firmware -D . --uefi $UE --target RELEASE   # -> $UE/Build/.../FV/SILICIUM_UEFI.fd
./unixnu bootimg  -D . --uefi $UE --target RELEASE \
                  --fd  $UE/Build/popsiclePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd \
                  --dtb $UE/Resources/DTBs/popsicle.dtb
```

Both `--target RELEASE` and the explicit `--fd`/`--dtb` are required and none is a default:
`unixnu` builds and looks for `DEBUG_CLANGPDB` unless told otherwise, and `bootimg` would otherwise
read `build/<board>/popsicle.dtb`, which only exists after `unixnu tree` — a step that fetches every
feed and belongs on the macOS side.

`bootimg` recompiles `BootShim/BootShim.S` itself and verifies the ARM64 image header, so the shim in
the checkout must be the one that reports `image_size = FD_SIZE + (_Payload - _Head)` and flushes the
cache before jumping. Stock Mu-Silicium writes `FD_SIZE` alone and skips the cache maintenance; the
overlay carries the fixed one.

Output: `out/popsicle/boot/boot.img` plus `boot.json` (sizes, sha256 of image/FD/DTB, shim size).
`scripts/verify-bootimg.py` walks that image back down to the FFS files and proves UniXNU is inside.

`build_uefi.py` on its own produces a firmware that boots to the plain UEFI boot manager, because
`popsicle.dsc` now names three UniXNU modules and they live outside the Mu-Silicium tree:

| module | in the volume? | what it does |
|---|---|---|
| `UniXNUPkg/Application/UniXNU/UniXNU.inf` | yes | the boot manager: reads `EFI/UniXNU/unixnu.toml` off the payload volume |
| `UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf` | yes | registers the application as a `Boot####` option and moves it to the front of `BootOrder` |
| `DarwinBootPkg/Driver/DarwinBoot.inf` | no | compiled, not packed — the reference platform leaves it out too |

`unixnu firmware` finds both packages through `board.toml [firmware] packages`, which lists
`{tree}/package` (holds `UniXNUPkg/`) and `{tree}/drivers/DarwinBoot` (holds `DarwinBootPkg/`).
`unixnu tree` populates both from the main checkout. A plain `build_uefi.py` run instead needs them
under `UniXNU/` in the Mu-Silicium checkout, which is what `DeviceBuild.py`'s `UniXNU` entry on
`PackagesPath` expects:

```sh
cp -a <unixnu>/package/UniXNUPkg      <mu-silicium>/UniXNU/UniXNUPkg
cp -a <unixnu>/drivers/DarwinBoot/DarwinBootPkg <mu-silicium>/UniXNU/DarwinBootPkg
```

Without that, the build stops at `UniXNUPkg.dec` not being found. `check-popsicle.py` guards the other
half of this: it fails if `popsicle.dsc` stops building either module or `popsicle.fdf` stops packing
the two that belong in the volume, so a firmware that silently reverts to the plain boot manager
cannot pass the preflight.

`build_uefi.py` also carries two latent upstream bugs worth knowing about, which this port fixes in
the overlay and in `tools/patch-builduefi.py` rather than by hand:

- `[boot_image]` pass-through drops falsy values (`if value:`) and has no `pagesize` key, so
  `base = 0` never reaches `mkbootimg` (it falls back to `0x10000000`, below this SoC's DRAM base) and
  the header page size stays at mkbootimg's 2048 instead of 4096.
- `BootShim/BootShim.S` in stock Mu-Silicium omits the `dc cvac` / `ic iallu` sequence before
  `br x5`, so the jump can land on stale instructions. The overlay carries the fixed shim.

Neither bug reaches `unixnu build firmware`, which handles both correctly.

`board.toml` names files in four different filesystems, and a path in the wrong one surfaces as an
`ENOENT` from inside a build step rather than as a bad path: this repository (`patches/`,
`configs/`, `scripts/`), the `firmware/` overlay that `supply-firmware.py` copies into the checkout,
the tree `unixnu tree` assembles (`kits/`, `drivers/`, `package/`), and a build directory
(`Build/`, `build/`, `out/`). `scripts/check-references.py` resolves all 28 against the namespace
each is used from and exits 1 with the offending path otherwise.

The overlay ones are the ones worth knowing about, because the names look committed and are not
usable where they sit: `Resources/MuPatches/*.patch` and `Resources/DTBs/<config>.dtb` are read by
`firmware.py` as `uefi / path`, so the six committed patches in `patches/` and the device tree in
`firmware/Platforms/*/<config>Pkg/FdtBlob/` have to be *traceable* to those paths, not merely present
here. Inside the assembled tree the script finds no `board.toml` and says so instead of failing, since
that is where preflight runs; and a `kits/` path it cannot trace (no main checkout nearby, or an
unfetched submodule) is reported as accepted-unverified rather than counted as proven.

The kernel steps (`build kernel`, `build payload`) need macOS with Xcode — that is where the XNU
sources get compiled. Everything up to and including the firmware volume and the boot image works on
Linux.

## Display

`popsicle.dsc` sets `USE_CUSTOM_DISPLAY_DRIVER = 0`, and that decides the whole display story:

| | `USE_CUSTOM_DISPLAY_DRIVER = 1` | `= 0` (this board) |
|---|---|---|
| Driver | `Binaries/popsicle/QcomPkg/Drivers/DisplayDxe/DisplayDxe.efi` (prebuilt, 438272 bytes) | `SiliciumPkg/Drivers/SimpleFbDxe` (source) |
| Panel data | 80 panel XMLs from `Binaries/popsicle/RawFiles` embedded as FV RAW sections | not used, not embedded |
| Works on popsicle? | no — the prebuilt driver's panel table is the OnePlus 15's (`mdss_dsi_panel_AD296_…`, `dsi_panel_AA601_…`) | yes |

So the panel XMLs generated for `p2_42_0d_0b_mp_prim_dsc_cmd` and `p2_42_06_0a_sec_dsc_cmd` are
**not** in the volume and are not needed for the UEFI stage. They are kept in the port workspace
(`panels/`) for a future DisplayDxe built from source, which this tree does not have — only
`DisplayReEnablerDxe` and `IortDtDxe` exist as source drivers under `Silicon/Qualcomm/QcomPkg`.

What SimpleFbDxe does instead is locate a memory region by name and draw at the PCD resolution:

- `GetFrameBufferInfos()` calls `LocateMemoryRegionByName("Display Reserved")` then
  `LocateMemoryRegionByName("Display_Reserved")`, compared with `AsciiStriCmp`, so the underscore
  form in `MemoryMapLib.c` matches.
- The descriptor is `{"Display_Reserved", 0xFC800000, 0x02B00000, …}` — base `0xFC800000` is exactly
  the vendor tree's `/reserved-memory/splash_region`, the buffer ABL leaves the splash in.
- Resolution comes from `PcdFrameBufferWidth|1200`, `PcdFrameBufferHeight|2608`,
  `PcdFrameBufferColorDepth|32`; 1200 × 2608 × 4 = 11.9 MiB fits the 43 MiB region.

`check-popsicle.py` now asserts that the descriptor and the device tree agree and that the
framebuffer fits, so a drift between the two fails the preflight instead of producing a black screen.
`check.json` records both sides (`display_reserved`, `splash_region`).

## Boot

```sh
adb reboot bootloader
fastboot boot out/popsicle/boot/boot.img
```

Nothing is written to the device. A cold boot back to Android is `fastboot reboot` or holding power.

Serial console is on the GENI SE1 UART at `0xa9c000`, 115200n8 — the device tree already points
`/chosen/stdout-path` there. This is the only channel that shows anything before the display driver
exists, so keep it attached for the first attempt.

## Filling in the one value that needs the handset

The absent `[userdata]` section is the only number in this repository that was not derived from
something this device produced. It needs the handset and is one command away once it is attached —
`tools/probe-device.sh` in the port workspace collects it:

```sh
tools/probe-device.sh          # prints the values
```

It reads the primary GPT with `dd` off `/dev/block/sda` and parses it locally (no `sgdisk`, no
`parted` — neither is reliably present in an Android userland), and reads `/proc/iomem` for the
physical RAM windows. Both need root, which this handset has. The parser is exercised by
`tools/probe-device.py selftest`, which builds a synthetic GPT and checks the byte offsets against
hand-computed answers — worth re-running after any change, because a silent offset error here would
point `[userdata]` at the wrong partition.

What to do with the output:

- `first_lba` / `last_lba` go into a new `[userdata]` section, with `lba_bytes = 4096`.
- `/proc/iomem` also prints the System RAM windows, which is the cross-check for `dram_size` — see
  the next section for why it is already very likely right and what a larger window would mean.

## What is known-good, and what is guessed

Verified against the vendor firmware dumped from this exact device:

- Memory map: 61 descriptors, cross-checked region by region against the 64 `reserved-memory` children
  of the merged vendor device tree. `scripts/check-popsicle.py` reports zero errors and zero warnings.
  The map inherited from the OnePlus 15 (`infiniti`, same SoC) was missing 19 regions — DSP images,
  the hypervisor heap, TrustZone, the shared-memory window — and would have handed them to DXE as
  free memory.
- Memory map, second opinion: the same 61 descriptors are cross-checked against the 28-region
  memorymap blob XBL itself uses, decoded from `xbl_config.img` and checked in as
  `assets/vendor-memorymap.json`. Every vendor region has a covering descriptor. Two of them
  (`DXE_Heap`, `DBI_Dump`) are covered by `Conv` descriptors, which is correct — the heap and a
  reclaimable dump area are meant to be allocatable.
- Firmware volume: builds to exactly 4,194,304 bytes at base `0xC6800000`, 52 images verified.
- The volume is reproducible from a named edk2 revision **and a named build epoch**. Building the
  `unixnu.lock` pin (`cfb93724`) with the six patches in `patches/` — through `unixnu build firmware`
  and through `build_uefi.py` (which manages five of them itself and needs only `Gic-Spurious`
  staged) — gives the identical volume for the same `SOURCE_DATE_EPOCH`:

  ```
  epoch 1792564011  (2026-10-21T06:26:51Z, the upstream commit time)
      unixnu build firmware, RELEASE  -> 9a4e0f5aef964302e99970e05091cd76b7ab38e0ab665e5970fe3ad4e17f8681
  epoch 1792731823  (when the local tarball was imported)
      unixnu build firmware, RELEASE  -> 2de63b3ec0a6b6a90dec88ca25c6654c2bc514d1a45e7655626d83907c25df40
      build_uefi.py, pin + Gic-Spurious -> 2de63b3ec0a6b6a90dec88ca25c6654c2bc514d1a45e7655626d83907c25df40
  ```

  Each row repeats bit for bit. `firmware.py:52` takes the epoch from the checkout's own commit
  date, and EDK II embeds it, so `scripts/supply-firmware.py` commits the assembled checkout dated
  `board.toml [firmware.supply] commit_time` — which is why the runner produces `9a4e0f5a…`. The patch
  list itself is not a guess: both drivers were run here and compared (`KERNEL-BUILD.md`, and
  `scripts/explain-firmware-parity.py` for the line-by-line reason).
- UniXNU is in that volume. `UniXNU.efi` (86,528 bytes) and `UniXNUDxe.efi` (9,728 bytes) both have
  their FFS files under `Build/.../FV/Ffs/`, so the boot manager is packed and the driver that gives it
  a `Boot####` option is packed beside it. `DarwinBoot.efi` (19,456 bytes) compiles and appears only in
  `Guid.xref`, never in `Ffs/` — unpacked, matching the reference platform. Before this was wired up
  the volume booted to the plain UEFI boot manager and never read the payload at all.
  `FVMAIN` sits at 99% of its own auto-sized extent with 2.8 MB still free in `FVMAIN_COMPACT`, so
  there is room for more modules.
- UniXNU is in the *boot image*, not just the build directory. The volume is wrapped in a GUIDed
  section whose GUID is `EE4E5898-3914-4259-9D6E-DC7BD79403CF` (`LzmaCustomDecompressGuid`), so it is
  LZMA-alone, not gzip: the section header is 20 bytes, the stream starts at `fd+0xE030` (properties
  `0x5D`, 16 MiB dictionary, then the 8-byte uncompressed size), decompresses to 8,945,672 bytes with
  `_FVH` at offset 48, and the two module GUIDs appear at `+0x6E31D8` (`UniXNU`) and `+0x6F8408`
  (`UniXNUDxe`). `DarwinBoot`'s GUID is absent. `scripts/verify-bootimg.py` checks exactly this and is
  what the workflow runs before uploading:

  ```sh
  python3 scripts/verify-bootimg.py out/popsicle/boot/boot.img
  ```
- Boot image: `header_version 0`, `page_size 4096`, `kernel_addr 0x8000`, `tags_addr 0x1e00000`,
  gzip payload of 192-byte shim + 4 MiB volume, 1,723,024-byte device tree appended.
- Display: `Display_Reserved` (`0xFC800000` + `0x02B00000`) equals the device tree's
  `splash_region`, and the 1200 × 2608 × 32 framebuffer fits inside it — the invariant SimpleFbDxe
  needs. Enforced by `check-popsicle.py`.
- The DRAM window `dram_size = 0x80000000` describes. The merged tree's `/memory` node is zeroed
  (ABL fills it at boot), but its 50 `reserved-memory` children with a non-zero `reg` are laid out
  from exactly `0x80000000` to exactly `0x100000000` — `llcc_lpi_region@ff800000` + `0x800000` ends on
  the 4 GiB boundary. A carveout map cannot span an address range that is not DRAM, so the low 2 GiB
  is backed; of that window 760 MiB is carved out and about 1.2 GiB is free for DXE and the kernel.

Guessed or inherited, in rough order of how likely it is to bite:

- **`dram_size = 0x80000000`** in `[payload]` is the low 2 GiB window, not the whole of DRAM. The
  evidence above says the window is real, so XNU will come up with 2 GiB and a working memory map;
  what is *not* established is whether extending it needs more than a bigger number. Two reasons it
  might: `/proc/iomem` may show one unbroken System RAM range covering all 16 GiB (in which case the
  number should grow), and `lcp_dare_region` sits at `0x880000000` — 34 GiB — and is marked
  `reusable` in the tree, which no 16 GiB device should have. Treat that region as unexplained:
  reserving it is safe whether or not it is backed, so `MemoryMapLib.c` reserves it and moves on.
- **`owned_memory_size = 0x130000000`** likewise inherited from sheng.
- **Only 6 of 8 cores are exposed** (`cpu_regs = "0,0x100,0x200,0x300,0x400,0x500"`). The two prime
  cores sit at MPIDR `0x10000`/`0x10100`, which the ADT generator rejects (`reg <= 0xffff`). Raising
  that limit is a separate change.
- **`[ipsw]` still names an iPad** (`iPad7,1`, 17.7.11 / 21H461). That describes the Apple device
  being imitated, not the SoC; it stays until the display is up and an iPhone identity is worth
  trying.
- **No `[userdata]` section.** `first_lba`/`last_lba` are physical GPT coordinates on LUN 0;
  sheng's values would land on top of unrelated partitions here. Fill them in from
  `adb shell 'su -c "sgdisk -p /dev/block/sda"'` — or with `tools/probe-device.sh` in the working
  copy, which reads the GPT header directly instead of relying on `sgdisk` being present —
  before enabling that step.
- **No `[payload.install]` either, and for a harder reason.** That section needs a Data seed, and
  `payload.py:180` rejects the entire payload step without one. A seed cannot be made here: sheng's
  came from the tablet, and `scripts/prepare-owner-data-fixture.py` builds one with `hdiutil` from a
  mounted copy of the supplied OS. The section is therefore absent, with a comment in its place saying
  what to restore. Without it the payload boots straight into the `xnu` entry — enough for first
  bring-up, not enough for a usable iOS install with a writable Data volume.
- **`[kernel.prelink]` is inherited from sheng, and now verified rather than assumed.** Every value in
  it describes the *original Apple kernelcache*, not the kernel this board builds: `kernelcache_sha256`
  pins the cache that `board.toml [ipsw]` yields, and `early_security_init` is a `void(void)` function
  inside that cache's `com.apple.driver.AppleMobileFileIntegrity` module. `scripts/verify-prelink.py`
  checks all of it against the real thing: the kernelcache was fetched out of the live IPSW and decoded
  here (the one macOS-only step, LZFSE, substituted with the open-source `lzfse` tool), its sha256
  equals the pin, the address lands inside AMFI's `__TEXT_EXEC` at `+0xaae0` with `initprot=5` and
  `maxprot=0x5` (no write), and all 31 roots are prelinked. Both pins are also hard-checked at build
  time (`ipsw.py:229`, `kernel.py:117-119`), so a mismatched IPSW fails loudly instead of producing a
  kernel that cannot run. This is what allows `CONFIG_XNU_CRYPTO_BOOTSTRAP="prelinked"`, which
  `build-xnu-4k-baseline.sh:162-165` requires before it will enable `NATIVE_AES`. The
  verified-root/owner-* options stay off until there is a mounted owner root for them to audit.
- **`Binaries/popsicle` in the Mu-Silicium tree is `Binaries/infiniti` plus two files.** The upstream
  Device-Binaries repository has no popsicle directory. DXE drivers are distributed per SoC, so reuse
  across the same SoC is the upstream convention; all 139 `Binaries/popsicle/...` references in
  `Include/{APRIORI,DXE,RAW}.inc` resolve and the volume verifies 52 images. The two files the overlay
  adds are the panel XMLs for `p2_42_0d_0b_mp_prim_dsc_cmd` and `p2_42_06_0a_sec_dsc_cmd`, which are the
  only genuinely device-specific inputs — and they are not used at `USE_CUSTOM_DISPLAY_DRIVER = 0`
  (see [Display](#display)).

## Next steps

1. Attach the UART and `fastboot boot` the image. Success is a UniXNU banner on serial, not a picture.
2. Cross-check `dram_size` against `/proc/iomem` while the handset is attached. The same session
   yields the ECID that `step: payload` needs (`board.toml [payload.adt] unique_chip_id`).
3. Dump the GPT and fill in `[userdata]`.
4. Build the kernel. This is the one step that needs macOS, and `.github/workflows/build.yml` gets it
   from a GitHub macOS runner instead of a local Mac — dispatch it with `step: kernel`. See
   [KERNEL-BUILD.md](KERNEL-BUILD.md) for why the SDK requirement is unavoidable, what the runner has
   that this machine does not, and why the `kits/*` and `drivers/*` submodules must be a real clone
   rather than a tarball. The workflow runs `scripts/check-build-guards.py` before compiling, so a
   defconfig the build script would refuse fails in seconds instead of after a full dispatch.
   `step: prelink` and `step: payload` no longer need anything macOS-only either —
   `scripts/patch-unixnu-kernelcache.py` substitutes `lzfse` for `compression_tool`, which was the last
   such dependency — and both now decode their own output back (`verify-prelink.py`, `verify-adt.py`)
   instead of trusting it.
5. Once the display is up, stop presenting popsicle as an iPad: pick a real target IPSW, then re-derive
   `kernelcache_sha256`, `early_security_init` and `roots` from its kernelcache (KERNEL-BUILD.md, "Re-
   deriving the values") and re-enable the `VERIFIED_ROOT_PROBE` / `OWNER_ROOT_*` group.

Steps 1–3 are the whole remaining scope that does not need a Mac, and steps 2–3 are the two values the
repository deliberately leaves blank. Nothing in the firmware or boot image is waiting on more local
work: the volume builds here in 30 seconds from `supply-firmware.py`'s output, to the same sha256 every
time, and the boot image verifies down to the two FFS files. Step 4 no longer needs a Mac in the room
either. `step: payload` additionally needs the ECID from step 2; until then `step: kernel`,
`step: prelink` and `step: firmware` are the dispatches that can run.
