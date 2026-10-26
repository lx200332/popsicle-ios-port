#!/bin/bash
set -e
cd <BUILD>/mu-silicium || exit 1
DL=<BUILD>/dl

extract_into() { # $1=tarball $2=dest
  local tb="$1" dest="$2"
  rm -rf "$dest"; mkdir -p "$dest"
  tar xzf "$tb" -C "$dest" --strip-components=1
  echo "  $(printf '%-56s' "$dest") $(find "$dest" -type f | wc -l) files, $(du -sh "$dest"|cut -f1)"
}

extract_into $DL/mu_plus.tar.gz        Common/Mu
extract_into $DL/mu_oem_sample.tar.gz  Common/Mu_OEM_Sample
extract_into $DL/mu_basecore.tar.gz    Mu_Basecore
extract_into $DL/silicium-acpi.tar.gz  Silicium-ACPI
extract_into $DL/openssl.tar.gz        Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl

echo "=== sanity: key files ==="
for f in Common/Mu/Mu.dec Mu_Basecore/MdePkg/MdePkg.dec Common/Mu_OEM_Sample/Mu_OEM_Sample.dec \
         Silicium-ACPI/Silicium-ACPI.dec Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl/Configure; do
  [ -f "$f" ] && echo "  OK   $f" || echo "  MISS $f"
done
