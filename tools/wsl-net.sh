#!/bin/bash
# Diagnose WSL->Windows host proxy reachability for NAT mode
GW=$(awk '/^nameserver/{print $2; exit}' /etc/resolv.conf)
echo "resolv.conf nameserver (= Windows host in NAT): $GW"
for p in 7897 7890; do
  code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 15 -x "http://$GW:$p" https://github.com/ 2>/dev/null)
  echo "proxy $GW:$p -> github.com : $code"
done
echo "--- direct ---"
curl -s -o /dev/null -w 'direct github: %{http_code} time=%{time_total}\n' --max-time 20 https://github.com/
