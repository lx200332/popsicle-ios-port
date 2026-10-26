#!/usr/bin/env python3
"""Resolve nested submodule pins from mu_basecore @ release/202608, then emit a fetch plan."""
import json, subprocess, sys, urllib.request, urllib.error

REF = "release/202608"
REPO = "microsoft/mu_basecore"
PATHS = [
    "UnitTestFrameworkPkg/Library/CmockaLib/cmocka",
    "MdeModulePkg/Universal/RegularExpressionDxe/oniguruma",
    "MdeModulePkg/Library/BrotliCustomDecompressLib/brotli",
    "BaseTools/Source/C/BrotliCompress/brotli",
    "UnitTestFrameworkPkg/Library/GoogleTestLib/googletest",
    "UnitTestFrameworkPkg/Library/SubhookLib/subhook",
    "MdePkg/Library/BaseFdtLib/libfdt",
    "MdePkg/Library/MipiSysTLib/mipisyst",
    "SecurityPkg/DeviceSecurity/SpdmLib/libspdm",
]

def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "build-setup", "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)

def main():
    lines = []
    for p in PATHS:
        url = f"https://api.github.com/repos/{REPO}/contents/{p}?ref={REF}"
        try:
            d = get(url)
        except urllib.error.HTTPError as e:
            print(f"# ERROR {p}: HTTP {e.code}", file=sys.stderr)
            continue
        if isinstance(d, list):
            print(f"# WARN {p}: got directory listing, not submodule", file=sys.stderr)
            continue
        sha = d.get("sha")
        typ = d.get("type")
        print(f"# {p}  type={typ} sha={sha}", file=sys.stderr)
        if typ == "submodule" and sha:
            lines.append(f"{p}\t{sha}")
    with open("<BUILD>/dl/nested_pins.tsv", "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {len(lines)} pins")

if __name__ == "__main__":
    main()
