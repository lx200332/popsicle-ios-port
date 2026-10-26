#!/bin/bash
# Detect host gateway from /proc/net/route and probe the Windows clash proxy.
GW=$(awk 'NR>1 && $2=="00000000" {print $3}' /proc/net/route | head -1)
if [ -z "$GW" ]; then echo "no default route"; exit 1; fi
# little-endian hex -> dotted quad
A=$((0x${GW:6:2})); B=$((0x${GW:4:2})); C=$((0x${GW:2:2})); D=$((0x${GW:0:2}))
HOST="$A.$B.$C.$D"
echo "gateway=$HOST"

for P in 7897 7890 10808 10809; do
  if timeout 3 bash -c "echo > /dev/tcp/$HOST/$P" 2>/dev/null; then
    echo "port $P OPEN"
  else
    echo "port $P closed"
  fi
done

echo "--- direct github ---"
timeout 15 curl -sS -o /dev/null -w 'direct github:%{http_code}\n' https://github.com 2>&1 || echo "direct failed"

echo "--- via proxy ---"
for P in 7897 7890; do
  timeout 20 curl -sS -x "http://$HOST:$P" -o /dev/null -w "proxy$P github:%{http_code}\n" https://github.com 2>&1 || echo "proxy$P failed"
done
