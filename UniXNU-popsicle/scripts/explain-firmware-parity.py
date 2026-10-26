#!/usr/bin/env python3
"""Why this board's firmware sha256 is comparable across the two build drivers.

There are two ways to build the popsicle firmware volume, and they do not obviously agree. This
records what each one does to the shared edk2 checkout, because a sha256 that matches is only
meaningful if the inputs match, and "it built" is not evidence of that.

`build_uefi.py` -- the local driver, in the Mu-Silicium repository
    :491-492  reverse-applies AsmMacroLib, Auth-Service, Boot-Manager, Timer, Usb-Bus
              (silently: a failed reverse is ignored, so an already-clean tree is fine)
    :512-514  re-applies the same five, and exits if any of them fails
    so at compile time the checkout is at the pin plus those five. `Gic-Spurious` is not in either
    list and has to be in place before the build starts. The five are left applied afterwards.

`unixnu build firmware` -- the driver the workflow uses
    firmware.py:56-59  for every patch in board.toml [firmware] patches: `git apply --check`, then
                       `git apply`. Nothing is reversed first, so a patch that is already present
                       fails the check.
    firmware.py:67     reverse-applies exactly what it applied, in a `finally` block.
    so at compile time the checkout is at the pin plus all six, and it is left clean afterwards.

Both therefore compile a checkout whose content is "pin plus six", and both produce

    2de63b3ec0a6b6a90dec88ca25c6654c2bc514d1a45e7655626d83907c25df40

That equality was measured, not assumed: `build_uefi.py` with `Gic-Spurious` staged gave this volume,
and so did the same tree once the artifact was rebuilt end to end.

What could not be measured here: `unixnu build firmware` itself. It does not reuse the `--uefi`
checkout's Mu_Basecore -- it fetches its own copy at the pin into `out/feeds/mu_basecore-<hash>` --
and this machine's WSL cannot complete a `git fetch` of that repository (`fetch-pack: unexpected
disconnect while reading sideband packet`, the same wall documented for large clones). So the
runner path is established by reading firmware.py and by checking each patch against pin files
fetched one at a time, not by running it. The first dispatch of `step: firmware` is what converts
that into a measurement, and its `firmware.log` is where to look if the sha256 differs.
"""
import sys

PIN = 'cfb93724a06ff86ff5ba971413368a7809ee42a5'
EXPECTED = '2de63b3ec0a6b6a90dec88ca25c6654c2bc514d1a45e7655626d83907c25df40'

PATCHES = (
    'AsmMacroLib.patch',
    'Auth-Service.patch',
    'Boot-Manager.patch',
    'Timer.patch',
    'Usb-Bus.patch',
    'Gic-Spurious.patch',
)

FRAMEWORK = {
    'build_uefi.py': {
        'reverses_first': True,
        'manages': PATCHES[:5],
        'needs_staged': ('Gic-Spurious.patch',),
        'leaves_applied': True,
    },
    'unixnu build firmware': {
        'reverses_first': False,
        'manages': PATCHES,
        'needs_staged': (),
        'leaves_applied': False,
    },
}


def main():
    print(f'edk2 pin      {PIN}')
    print(f'expected fd   {EXPECTED}')
    print()
    for name, how in FRAMEWORK.items():
        print(f'{name}')
        print(f'  reverse before build : {how["reverses_first"]}')
        print(f'  applies at compile   : {len(how["manages"])} patch(es)')
        print(f'  must be staged first : {list(how["needs_staged"]) or "nothing"}')
        print(f'  leaves applied after : {how["leaves_applied"]}')
        print()
    print('Both compile "pin plus six" and therefore agree; see the module docstring for the '
          'line numbers and for what is measured versus read.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
