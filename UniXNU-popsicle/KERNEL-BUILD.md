# Building the kernel without a Mac

The UEFI half of this port builds anywhere — `python3 build_uefi.py -d popsicle` on Linux, WSL or
macOS, 25 seconds, 4 MiB volume. The kernel half does not. `scripts/build-xnu-4k-baseline.sh:774`
runs the XNU make as

```sh
make -C .cache/xnu-work build SDKROOT=iphoneos ARCH_CONFIGS=ARM64 ...
  DEPLOYMENT_TARGET_FLAGS="-miphoneos-version-min=17.0 -DXNU_TARGET_OS_IOS" \
  DEPLOYMENT_LINKER_FLAGS="-Wl,-ios_version_min,17.0"
```

(the `SDKROOT` default is on line 774, the two deployment flags on lines 778–779) so it needs Xcode
and the iOS 17 SDK. `scripts/unixnu/kernel.py` checks for `xcodebuild` up front and says as much.
There is no Linux cross-compiler that substitutes for it: the SDK supplies the headers, the
`xcodebuild`-driven `make` supplies the build system, and `installhdrs SDKROOT=macosx` on line 115
needs the macOS SDK too.

`.github/workflows/build.yml` exists so that a GitHub macOS runner does that one step. Everything
else — firmware, preflight, boot image, ADT — stays on this machine.

## What the runner has that this machine does not

| Requirement | Where it comes from |
|---|---|
| Xcode + iOS 17 SDK | preinstalled on `macos-14` (arm64, Xcode 15.x) |
| macOS SDK for `installhdrs` | same Xcode |
| Homebrew LLVM and LLD | `brew install llvm lld`; `kernel.py:toolchain_path()` already puts `/opt/homebrew/opt/{llvm,lld}/bin` first |
| `cmake`, `ninja` | `brew install` — the AvailabilityVersions build uses both |
| `unifdef` | `brew install` — `build-xnu-4k-baseline.sh` runs it over `refer/libdispatch/os/*.h` |
| `mtools` | `brew install` — the payload volume is an `mformat`/`mcopy` FAT image |
| `lzfse` | `brew install` — decodes the LZFSE kernelcache; the workflow patches `ipsw.py` to prefer `/usr/bin/compression_tool` and fall back to this |
| Python ≥ 3.11 | `actions/setup-python@v5` |

The `firmware` and `all` dispatches also need nothing beyond `clang`, `python3`, `make` and `git` —
the Mu-Silicium checkout is downloaded and assembled inside the run by
`scripts/supply-firmware.py`, so the runner does not need one preinstalled.

## Two things the workflow must get right, and why

**Submodules are load-bearing.** `kits/*` and `drivers/*` are git submodules of the UniXNU
repository, not directories with content. A tarball download gives you the names and nothing else —
`kits/NativePrelink` comes back empty, and then `build-xnu-4k-baseline.sh:44` does

```sh
for PATCH in "$ROOT"/patches/xnu/*.patch "$ROOT"/kits/NativePrelink/patches/*.patch; do
  if ! git -C .cache/xnu-work apply --reverse --check "$PATCH" >/dev/null 2>&1; then
    git -C .cache/xnu-work apply --check "$PATCH"
    git -C .cache/xnu-work apply "$PATCH"
  fi
done
```

Under `set -euo pipefail` an unmatched glob stays literal, and the script then hands that literal
string to `git apply`. Measured on this machine, with the directory absent:

```
$ for PATCH in "$ROOT"/patches/xnu/*.patch "$ROOT"/kits/NativePrelink/patches/*.patch; do echo "$PATCH"; done
/…/patches/xnu/a.patch
/…/kits/NativePrelink/patches/*.patch          ← the glob did not expand

$ git apply --check "/…/kits/NativePrelink/patches/*.patch"; echo $?
error: can't open patch '/…/kits/NativePrelink/patches/*.patch': No such file or directory
128
```

Exit 128 inside the loop aborts the script, and the `ERR` trap names the line. So the failure is
immediate and unambiguous rather than silent — but it is also entirely avoidable, which is why the
workflow checks out with `submodules: recursive` and then asserts each needed module arrived before
building.

`kits/NativePrelink` carries two patches that matter:
`xnu-10063.141.1-security-init.patch` and `xnu-10063.141.1-sparse-index.patch`. The first adds a marker
symbol (`native_prelink_security_init_version = 1`) and the OSKext call path that `[kernel.prelink]`
relies on. Note that it does **not** supply the `early_security_init` address — see below.

**The device repository stays separate.** `unixnu` takes `-D DIR` for the device repository and reads
the main repository from its own location (`config.py`: `ROOT = Path(__file__).parents[2]`). The
workflow checks the device repository out at `device/` and UniXNU at `unixnu/`, then passes
`-D device`. `tree.assemble` symlinks `drivers/`, `kits/`, `patches/` from the main checkout and
copies `scripts/` from both, so the assembled tree at `out/popsicle/tree` is where every path in
`build-xnu-4k-baseline.sh` resolves.

## Running it

Push this repository to GitHub, then:

```
Actions → build → Run workflow → step: kernel
```

`kernel` is the right first dispatch. It runs one bootstrap pass and one real pass, and produces

```
out/popsicle/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/
  mach.development.bcm2837.unstripped.noctf     ← the artifact worth having
```

plus every `*.o.json` compile record, which the archive builders read. The step is expected to take
20–40 minutes on a runner; the job timeout is 350 minutes because a cold tree also clones XNU,
libplatform, libdispatch, AvailabilityVersions, libpthread, corecrypto and IOStorageFamily.

`step: all` chains `kernel → prelink → payload`. `prelink` stays `continue-on-error` so that a prelink
failure cannot throw away the kernel the dispatch was made for.

## How `prelink` is configured, and why the values are inherited

`unixnu build prelink` rewrites kext bindings against one exact kernelcache. `board.toml [kernel.prelink]`
therefore pins that input, and every value in it describes **the original Apple kernelcache, not the
kernel we build**:

- `kernelcache_sha256` — the decompressed kernelcache that `unixnu fetch` produces from `board.toml
  [ipsw]`. Checked twice: `ipsw.py:229` rejects a kernelcache that misses the pin, `kernel.py:117-119`
  rejects a cache that does not match it.
- `early_security_init` — a `void(void)` function inside that kernelcache's
  `com.apple.driver.AppleMobileFileIntegrity` module, called before MAC lockdown. `emit_prelink.py:150-157`
  validates it against the **original** module's `__TEXT_EXEC` segment (protections, 4-byte alignment,
  overflow after slide), never against our build. A different defconfig cannot invalidate it.
- `roots` — the kexts whose bindings get rewritten; they have to exist in that kernelcache.
- `slide` — the address slide the kernelcache is prelinked at.

Because `[ipsw]` is the same image sheng uses (`iPad7,1 / 17.7.11 / 21H461`) and `unixnu.lock` pins the
same xnu commit, all of these carry over from sheng unchanged. That was originally an argument; it is
now measured. `scripts/verify-prelink.py` opens the kernelcache, and the kernelcache it opens was
already proved to hash to the pin — not by trusting the pin, but by fetching the `KernelCache` member
out of the live IPSW and decoding it here:

```
member   kernelcache.release.ipad7  16238580 bytes
payload  16238508 bytes  first4=62767832          (bvx2 = LZFSE)
fat      46415900 bytes  magic=cafebabe            (one arm64 slice)
arm64    46415872 bytes  magic=cffaedfe
sha256   f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7
pin      f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7
```

The IPSW is a zip over an HTTP range source, so this works without downloading 6.8 GB and without
macOS: `ipsw.py`'s `KernelCache` path shells out to `/usr/bin/compression_tool`, but the open-source
`lzfse` package decodes the identical stream, so the one macOS-only step in the chain can be
substituted. Everything else — the zip member, the IM4P envelope, the fat-slice pick — is plain
Python and runs anywhere. With that kernelcache in hand, `verify-prelink.py` reports:

```
46415872 bytes, 18 segments, 237 prelinked kexts
[kernel.prelink] roots: 31 listed
  ok    all 31 roots are prelinked into this kernelcache
com.apple.driver.AppleMobileFileIntegrity = 0xfffffff005ab4ae0
  ok    bundle is prelinked into this kernelcache
  ok    bundle is an AppleSecurityExtension
  ok    address is 4-byte aligned
  ok    bundle has a __TEXT_EXEC segment
        __TEXT_EXEC 0xfffffff005aaa000..0xfffffff005aca5e8 initprot=5 maxprot=0x5
  ok    address is inside __TEXT_EXEC at +0xaae0
  ok    initprot is 5 (read+execute)
  ok    maxprot excludes write, so the page stays executable
  ok    with slide -0x1000000 the address is 0xfffffff004ab4ae0
```

so all three inherited values are confirmed against the bytes they describe: the sha256 matches, the
address lands inside AMFI's executable segment at +0xaae0 with the right protections, and every root
is actually prelinked. The negative controls behave: an address three bytes off reports
`address is not 4-byte aligned` (`+0xaae2` → low bits 2), an address outside the segment reports
`is outside __TEXT_EXEC`, and an unknown bundle reports `no such bundle in this kernelcache`, each
with exit status 1.

Two format details cost time here and are handled in the script. `__PRELINK_INFO` holds a bare
`<dict>` with no XML declaration and no `<plist>` wrapper. And it is Apple's *reference* encoding:
the first use of a value is `<string ID="100">com.apple.kernel</string>` and every later use is an
empty `<string IDREF="100"/>`, with addresses written as `<integer>0xfffffff00527c000</integer>`.
`plistlib` rejects that outright — the traceback is `invalid literal for int() with base 10: ''`,
from an empty `<integer IDREF=.../>` — so the script carries a small resolver that memoises IDs and
treats a dangling `IDREF` as an absent field rather than an error.

### This is what decides the crypto mode

`build-xnu-4k-baseline.sh:162-165` refuses `NATIVE_AES` unless `CRYPTO_BOOTSTRAP=prelinked`, and
`:166-171` / `:182-187` make `NATIVE_VIRTIO_BLOCK` and `NATIVE_RAM_BLOCK` depend on `NATIVE_AES`. So the
whole storage group is gated on the prelink path being available:

```
CONFIG_XNU_NATIVE_AES=y            requires CRYPTO_BOOTSTRAP=prelinked
CONFIG_XNU_NATIVE_VIRTIO_BLOCK=y   requires NATIVE_AES
CONFIG_XNU_NATIVE_RAM_BLOCK=y      requires NATIVE_AES
```

`prelinked` compiles `prng_random.o` with `-UXNU_SOURCE_CORECRYPTO14`: the kernel ships no crypto
provider of its own and uses the original `com.apple.kec.corecrypto` that the prelink step merges out of
the kernelcache. The alternative — a self-contained `source` kernel — would force AES, VirtIO block and
RAM block off, and without hardware AES there is no APFS to mount, so it buys nothing for a first boot.

`scripts/check-build-guards.py` evaluates all 56 of those refusal conditions against a `.config` in about
a second, and the workflow runs it before the compile. Without that a rejected combination is only
discovered after a runner has spent minutes checking out and installing a toolchain. It is a pre-flight,
not a substitute for the build: it decides the guards, not whether the selected drivers compile.

### The check that has to run before the guard check

A guard can only refuse a symbol that something reads. `CONFIG_XNU_FOO` becomes the environment
variable `XNU_FOO` for the build script (`kernel.py:10-19`) and nothing validates the name, so a
typo produces a variable no one reads: the feature quietly keeps its default, every guard passes,
and the build succeeds with a kernel that behaves like the option was off. On a phone that presents
as a driver that never probes, which is among the slower things to diagnose.

`scripts/check-defconfig-symbols.py` closes that gap. It collects every `CONFIG_XNU_*` from the
defconfig and intersects it with the set of `XNU_*` names that actually appear in the two consumers
— `scripts/build-xnu-4k-baseline.sh` and `scripts/unixnu/kernel.py`. Both have to be searched:
`XNU_BUILD_ARCHIVES` is read by `kernel.py:54` to decide whether to run the archive builders at all,
and the shell script never mentions it, so looking only at the script would report a live symbol as
dead. Verified against this board: **20 configured, 20 read, none dead.** A deliberately mistyped
`CONFIG_XNU_NATIVE_AESS` is caught, and so is a line that is not `CONFIG_NAME=value` at all.

The workflow runs it immediately before the guard check, because it decides whether the guard
results mean anything.

`check-defconfig-symbols.py` also prints the 50 `XNU_*` names the build reads but this board leaves
unset. That list is the record of which knobs exist and are knowingly left at their defaults —
the `OWNER_*` group, the Adreno and display probes, `XNU_NATIVE_ADRENO`, the tag and path variables
the driver sets instead — rather than a set of things someone forgot.

### Re-deriving the values

If `[ipsw]` ever changes — which it must, once popsicle stops presenting itself as an iPad — then:

1. Extract the new kernelcache and record its sha256.
2. Find the `void(void)` policy initializer in its `com.apple.driver.AppleMobileFileIntegrity` module:
   4-byte aligned, inside a `__TEXT_EXEC` segment whose `initprot` is `R|X` and whose `maxprot` lacks
   `W`, per the check in `emit_prelink.py`.
3. Update `roots` to the kexts that exist in the new kernelcache.
4. Keep `CONFIG_XNU_CRYPTO_BOOTSTRAP="prelinked"`; re-enable the `VERIFIED_ROOT_PROBE` / `OWNER_ROOT_*`
   group, which `:248` also gates on `prelinked`.

Nothing above needs a kernel built by this workflow — that was the earlier, mistaken assumption.

## What the payload step needs, and where it comes from

`unixnu build payload` assembles the 192 MiB FAT volume DarwinBoot reads. Two of its inputs are not
derivable from the repository, and the workflow supplies both:

**The trust cache.** `payload.py:105` rejects any trust cache whose sha256 is not listed in
`board.toml [payload] trust_cache_sha256`, and neither file the IPSW ships is on that list — the pins
are the hash of a *bundle* of two version-2 modules, each unwrapped from a DER envelope:

```
StaticTrustCache            59519 B -> DER field 3 -> 59496 B module (2478 entries)  da88460e…
Cryptex1,SystemTrustCache    1871 B -> DER field 3 ->  1848 B module (  76 entries)  d121ba89…
bundle = <2, 12, 12+len(static)> + static + system   61356 B                         7416ca4f…  = pin
```

`scripts/make-popsicle-trustcache.py` reproduces that byte for byte and refuses to write a bundle whose
hash is not a pin. It runs anywhere — both shipped files are DER, so nothing needs `compression_tool`,
`hdiutil` or a mounted 5.1 GB DMG. The literal `12` in the bundle header is a header-size field copied
from `build-original-system-trust.py:53`; a module count would be 2 and produces a different hash.

**The platform seed.** `payload.py:121-122` wants `--unique-chip-id` (or `UNIXNU_UNIQUE_CHIP_ID`)
because `board.toml [payload.adt] unique_chip_id = "required"`, and it says why: the seed is not
stored in any repository. It is the device's ECID, which only the device can report. The workflow takes
it from the `ecid` dispatch input or a `POPSICLE_ECID` repository secret and never from a commit. With
neither set, the payload step fails with that message — correctly, because a payload built with someone
else's ECID cannot boot this phone.

**The Data seed, which this board deliberately does not have.** `[payload.install]` adds a second boot
menu entry that erases and re-seeds the Data volume, and `payload.py:180` refuses the *whole* payload
step without a seed:

```
board.toml [payload.install] needs a Data seed: pass --data-ramdisk FILE
```

sheng has one because the tablet can produce it, and `scripts/prepare-owner-data-fixture.py` can build
one too — but with `hdiutil` against a mounted copy of the supplied OS at
`.cache/firmware/target-volumes/os-mount`, which is a macOS-plus-installed-OS prerequisite. popsicle
has no seed and no way to make one here, so `[payload.install]` is gone from `board.toml` and the
comment in its place says what to restore and when. First bring-up boots straight into the `xnu` entry,
which is all it is testing. Declaring the entry anyway would have made every `step: payload` dispatch
fail on a capability the port does not have — after the kernel build, at the very end.

So `step: payload` is the one dispatch that cannot succeed until the phone is in hand, for the ECID.
`step: kernel` and `step: prelink` do not need it, and `step: payload` needs nothing else: with the
`[payload.install]` entry removed it runs to a sealed 192 MiB volume from `board.toml` alone, with the
ADT verified back out of the image.

## The edk2 pin, and why this board lists six patches

The firmware step builds against whatever `unixnu.lock` pins for `edk2`, which is
`microsoft/mu_basecore` at `cfb93724` (2026-10-02). That commit is **older than the tree this board
was verified on**, and the gap is not cosmetic. Checked file by file against the pin:

| patch | what it fixes | at `cfb93724` |
| --- | --- | --- |
| `AsmMacroLib` | `ASM_FUNC` emitted `.text. ## Name`, so every function had its own section | not applied |
| `Auth-Service` | secure boot authenticated-variable handling | not applied |
| `Boot-Manager` | the boot manager menu application | not applied |
| `Timer` | the generic timer frequency readout | not applied |
| `Usb-Bus` | the USB hub API | not applied |
| `Gic-Spurious` | a level interrupt with no handler fires forever and starves the CPU | not applied |

All six report `NEEDS-APPLYING` at the pin, so `[firmware] patches` lists all six. `sheng` lists
only `Gic-Spurious`; at this pin that leaves the other five silently missing.

That table is now measured rather than inferred. `scripts/check-patches-at-pin.py` fetches the
eleven files the six patches rewrite at `cfb93724` from `raw.githubusercontent.com` (191,938 bytes
in all), `git init`s a throwaway tree, and runs `git apply --check` then `git apply` for each patch
in turn, in the order the runner will:

```
edk2 pin  cfb93724a06ff86ff5ba971413368a7809ee42a5
targets   11 file(s)
AsmMacroLib    applies
Auth-Service   applies
Boot-Manager   applies
Gic-Spurious   applies
Timer          applies
Usb-Bus        applies

all 6 patch(es) apply at the pin
```

Two details that make this a real answer rather than a formality. The pin's files are **CRLF** —
`UsbBus.h` alone has 781 CR lines — and patches contain CRLF too; `git apply` with default settings
resolves that, which is worth knowing because `core.autocrlf` on a Windows runner is the kind of
thing that turns "applies" into "corrupt patch". And the sequence is applied cumulatively, because
two patches touch neighbouring code in `MdePkg/Include/AArch64/AsmMacroLib.h` and checking them
independently would test a tree the build never sees.

### The one line in `.gitattributes` this depends on

Because both sides are CRLF, the patches have to reach the runner with their bytes intact, and
Git's line-ending conversion is the thing that would stop that. `.gitattributes` used to say:

```
*.patch text eol=lf
```

`text` makes Git normalise on commit, so the blobs would carry LF while the edk2 files at the pin
carry CRLF. Measured, with the pin's `UsbBus.h` in place and `git apply --check`:

| patch bytes | target | result |
| --- | --- | --- |
| CRLF (as generated) | `UsbBus.h` CRLF | applies |
| LF (what `text eol=lf` would commit) | `UsbBus.h` CRLF | `error: patch failed: MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBus.h:134` / `patch does not apply` |
| LF, with `-c core.autocrlf=true` | `UsbBus.h` CRLF | applies |

The last row is why this could have shipped broken and still passed on a machine with a convenient
Git config. So the attribute is now

```
*.patch -text !eol -whitespace
```

`-text` turns off normalisation in both directions, `!eol` leaves the end-of-line setting
unspecified so nothing re-enables it later in the file, and `-whitespace` keeps a console diff from
painting every line. Verified end to end: commit the six patches, `git clone`, and the clone's blobs
are byte-identical (CR counts 12/12/54/103/24/20 unchanged). Then apply the *cloned* bytes to a tree
fetched from the pin — six of six apply. Strip the CRs from the same clone, as the old attribute
would have, and six of six fail:

```
6 of 6 patch(es) do not apply at the pin: AsmMacroLib.patch, Auth-Service.patch,
Boot-Manager.patch, Gic-Spurious.patch, Timer.patch, Usb-Bus.patch
```

That is the regression `check-patches-at-pin.py` exists to catch, and the reason it reads the
patches from disk rather than from `Resources/MuPatches/` in some other checkout.

What this does *not* cover: whether the rest of edk2 differs between the pin and the checkout the
build actually receives. That question needs the whole tree, and `codeload` truncates the 7 MB
tarball here long before the end, so it stays open until the first `step: firmware` dispatch. The
workflow runs this check as its own step immediately before `build firmware`, so a stale patch list
fails in twenty seconds instead of after the clone.

It reads the raw file host rather than the contents API on purpose: the API allows 60 unauthenticated
requests an hour and this check needs eleven, so a runner that had already spent its budget would
fail here for a reason unrelated to the patches. A 404 is reported immediately instead of being
retried — at a pinned commit a missing path means the patch names something that revision does not
have, and waiting will not change that.

The five marked "not applied" were **merged upstream between the pin and `release/202608` head**
(`5dc6caf0`, 2026-10-23). Applying them at the pin reproduces that head's content for those files,
which is exactly the tree this board was built and checked against.

### Why the firmware is the same either way

Two build drivers exist and they treat patches differently, which is worth knowing before trusting a
sha256:

| | `unixnu build firmware` (the workflow) | `build_uefi.py` (local) |
| --- | --- | --- |
| how patches are found | `[firmware] patches` in `board.toml`, resolved as `uefi / patch` | a hard-coded list of five names under `Resources/MuPatches/` |
| before the build | nothing | `handle_git_patch(..., remove=True)` reverse-applies those five (`build_uefi.py:491-492`) |
| applying | `git apply --check` then `git apply`, for every listed patch (`firmware.py:56-59`) | re-applies the same five, exiting if one fails (`build_uefi.py:512-514`) |
| after the build | reverse-applies what it applied, unconditionally (`firmware.py:67`) | leaves them applied |

So `build_uefi.py` manages the five itself and only `Gic-Spurious` has to be in place by hand, while
`firmware.py` applies all six from `board.toml`. Both end at the same content and both are
byte-reproducible, but **not to the same bytes**, because the volume also depends on the build
timestamp:

```
epoch 1792564011 (2026-10-21T06:26:51Z, the upstream commit time)
    unixnu build firmware, pin + all six   -> 9a4e0f5aef964302e99970e05091cd76b7ab38e0ab665e5970fe3ad4e17f8681
epoch 1792731823 (2026-10-22T04:23:43Z, when the local tarball was imported)
    unixnu build firmware, pin + all six   -> 2de63b3ec0a6b6a90dec88ca25c6654c2bc514d1a45e7655626d83907c25df40
    build_uefi.py, pin + Gic-Spurious      -> 2de63b3ec0a6b6a90dec88ca25c6654c2bc514d1a45e7655626d83907c25df40
epoch 1600000000 (arbitrary, to prove it moves the hash)
    unixnu build firmware, pin + all six   -> a436e1ba84170b221a1ee4ded40b1cca85933f043a82936c2ed6cdffba32e868
```

Every row reproduces bit for bit when repeated with the same epoch. That is the whole point of the
table: **the sources and the epoch together determine the volume, and only the sources are in git.**
`firmware.py:52` is

```python
env.setdefault('SOURCE_DATE_EPOCH', git(uefi, 'log', '-1', '--format=%ct').strip())
```

so the epoch is the `%ct` of the checkout's own HEAD unless something exports it. A fresh clone of
upstream `42fe563` has commit time 1792564011; a locally assembled tree has whatever date its import
commit was given. `supply-firmware.py` therefore commits the assembled checkout dated
`board.toml [firmware.supply] commit_time`, which is why a runner produces `9a4e0f5a…` and why that
hash is the one to compare against. Exporting `SOURCE_DATE_EPOCH` overrides it in either direction —
use that if you want to match a specific recorded hash from a differently-dated checkout.

(`build_uefi.py` needs `Gic-Spurious.patch` copied into `<uefi>/Resources/MuPatches/` first, because
its list does not include it — that is what `stage-popsicle-patches.py` does.)

### Why a staging script exists

`unixnu/firmware.py:57` resolves a patch as `uefi / patch`, i.e. **inside the Mu-Silicium checkout**,
and the build clones that checkout fresh — so a patch committed to this repository is invisible to
it. `scripts/stage-popsicle-patches.py` runs as the first preflight step (preflight runs before any
patch is applied) and copies `patches/*.patch` into `<uefi>/Resources/MuPatches/`. It is idempotent,
and it classifies each patch before staging:

```
staged   applies cleanly, so firmware.py will apply it
present  already in the checkout with identical bytes
skipped  already applied in the tree; copying it would make git apply refuse
error    neither, so the checkout is not the one this patch was written for
```

That last case matters: `git apply` rejects both an already-applied patch and an empty file
(`No valid patches in input`), so a patch list that is wrong for the pin fails loudly here instead of
half-way through a 15-minute firmware build.

## Getting the artifacts back

The run uploads `artifacts/` with the kernel, the compile logs (`out/popsicle/logs/`) and a
`SHA256SUMS`. Download the artifact zip, then copy the kernel into the tree on this machine:

```sh
mkdir -p out/popsicle/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837
cp mach.development.bcm2837.unstripped.noctf out/popsicle/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/
```

That is the only file `prelink` reads (`spec['kernel']`), so a kernel built on a runner is
interchangeable with one built locally.

## If the runner build fails

The log that matters is `kernel.log` in the artifact — the workflow tees the whole build into it, and
`out/popsicle/logs/` holds one file per archive builder plus `kernel-bootstrap.log`. Three failures
are worth recognising:

- `build-xnu-4k-baseline.sh: line N: … failed` — the script installs an `ERR` trap that names the
  exact command and line. Most often a missing submodule, which the "Confirm the submodules actually
  arrived" step catches first.
- `error: unexpected XNU reference revision` — the cloned `refer/xnu` is not at
  `d8b80295118ef25ac3a784134bcf95cd8e88109f`. `feeds.toml` pins it; `unixnu.lock` agrees.
- `ArmCpu bring-up requires the real source provider and a real platform driver` — a defconfig
  combination the script refuses. `popsicle_defconfig` sets `ARM_DT_PLATFORM=y` and
  `SOURCE_CORECRYPTO14=y`, which satisfies it.
- `MISS <path>   ([firmware] …)` from `check-references.py`, or `ENOENT` on a `resources/` path from a
  build step — the same class of mistake. A path in `board.toml` is resolved in one of four places
  (this repository, the `firmware/` overlay, the `unixnu` tree, a build directory) and the build says
  nothing about which one it expected.

All of these are configuration refusals except the first, and the "Check the configuration against the
build script's guards" step runs before the compile for exactly that reason. If it fails, reproduce
locally without a runner:

```sh
python3 scripts/check-build-guards.py <unixnu>/scripts/build-xnu-4k-baseline.sh out/popsicle/.config
```

It prints the guard condition, the script's own refusal message, and exits 1 when the configuration
would be refused, 3 when a guard depends on something a `.config` cannot express.
