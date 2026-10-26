#!/bin/bash
# Install EDK2 build deps. apt goes DIRECT (proxy 502s on ubuntu archive); shells get proxy for git/curl.
set -x
PROXY="http://172.27.32.1:7897"

# Shell proxy (github etc), but bypass apt / local
cat > /etc/profile.d/proxy.sh <<EOF
export http_proxy=$PROXY
export https_proxy=$PROXY
export ftp_proxy=$PROXY
export no_proxy=localhost,127.0.0.1,172.27.32.1,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,archive.ubuntu.com,*.ubuntu.com
EOF
chmod +x /etc/profile.d/proxy.sh

# Make apt bypass the proxy entirely
cat > /etc/apt/apt.conf.d/99noproxy <<'EOF'
Acquire::http::Proxy "false";
Acquire::https::Proxy "false";
EOF

export http_proxy= https_proxy= ftp_proxy=

apt-get update 2>&1 | tail -6

DEBIAN_FRONTEND=noninteractive apt-get install -y \
  build-essential uuid-dev iasl nasm python3 python3-venv python3-pip \
  git curl ca-certificates gcc-aarch64-linux-gnu \
  device-tree-compiler gcc g++ make bc flex bison \
  libssl-dev libgnutls28-dev pkg-config zip unzip \
  2>&1 | tail -20

echo "=== versions ==="
python3 --version
gcc --version | head -1
make --version | head -1
for t in iasl nasm dtc aarch64-linux-gnu-gcc python3; do printf '%s: ' "$t"; command -v "$t" || echo MISSING; done
echo "=== DONE ==="
