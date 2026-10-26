#!/bin/bash
set -u
DL=/root/build/dl/unixnu
mkdir -p "$DL"
cd "$DL" || exit 1

fetch_repo() {
  local repo="$1" dest="$2"
  local def
  def=$(curl -sSL --http1.1 --retry 3 "https://api.github.com/repos/$repo" \
        | python3 -c "import sys,json;print(json.load(sys.stdin).get('default_branch','main'))" 2>/dev/null)
  [ -z "$def" ] && def=main
  echo "=== $repo (default branch: $def) ==="
  local tb="$DL/$(echo "$repo" | tr '/' '_').tar.gz"
  if [ ! -s "$tb" ]; then
    curl -sSL --http1.1 --retry 6 --retry-all-errors -C - -o "$tb" \
      "https://codeload.github.com/$repo/tar.gz/refs/heads/$def" || { echo "  DOWNLOAD FAIL"; return 1; }
  fi
  echo "  tarball $(du -h "$tb" | cut -f1)"
  rm -rf "$dest"; mkdir -p "$dest"
  if tar xzf "$tb" -C "$dest" --strip-components=1 2>/dev/null; then
    echo "  extracted $(find "$dest" -type f | wc -l) files -> $dest"
  else
    echo "  EXTRACT FAIL"; return 1
  fi
}

fetch_repo "UniXNU/UniXNU"        "/root/build/unixnu"
fetch_repo "UniXNU/UniXNU-sheng"  "/root/build/unixnu-sheng"
fetch_repo "UniXNU/uefi-sheng"    "/root/build/uefi-sheng"

echo
echo "=== UniXNU top level ==="
ls -la /root/build/unixnu 2>/dev/null | head -30
echo
echo "=== UniXNU-sheng top level ==="
ls -la /root/build/unixnu-sheng 2>/dev/null | head -30
echo
echo "=== uefi-sheng: does it have popsicle-like platform? ==="
ls /root/build/uefi-sheng/Platforms/*/ 2>/dev/null | head -20
