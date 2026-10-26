#!/bin/bash
cd /root/build/mu-silicium || exit 1
echo "=== Common/Mu top ==="; ls Common/Mu | head -20
echo "=== Common/Mu_OEM_Sample top ==="; ls Common/Mu_OEM_Sample | head -20
echo "=== Silicium-ACPI top ==="; ls Silicium-ACPI | head -20
echo "=== Conf present? ==="; ls -d Conf 2>/dev/null || echo "no Conf"
echo "=== Build present? ==="; ls -d Build 2>/dev/null || echo "no Build"
