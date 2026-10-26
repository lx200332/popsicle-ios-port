#!/bin/bash
# Make each submodule directory a real git repo so edk2toolext's RequiredSubmodule check passes.
set -u
cd /root/build/mu-silicium || exit 1

git config --global user.email "build@localhost" 2>/dev/null
git config --global user.name  "build" 2>/dev/null
git config --global --add safe.directory '*' 2>/dev/null

init_repo() {
  local d="$1"
  [ -d "$d" ] || { echo "  SKIP (absent) $d"; return; }
  if [ -d "$d/.git" ]; then echo "  already repo: $d"; return; fi
  ( cd "$d" && git init -q . && git add -A -f >/dev/null 2>&1 && git commit -qm "vendor import" >/dev/null 2>&1 )
  if [ -d "$d/.git" ]; then echo "  OK  $d"; else echo "  FAIL $d"; fi
}

for d in Binaries Common/Mu Common/Mu_OEM_Sample Mu_Basecore Silicium-ACPI \
         Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl; do
  init_repo "$d"
done

echo "=== verify ==="
for d in Binaries Common/Mu Common/Mu_OEM_Sample Mu_Basecore Silicium-ACPI \
         Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl; do
  printf '%-70s ' "$d"
  git -C "$d" rev-parse --is-inside-work-tree 2>/dev/null || echo "NOT_A_REPO"
done
