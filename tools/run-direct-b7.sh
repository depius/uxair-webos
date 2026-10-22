#!/bin/sh
# Rooted B7 lab launcher: only the isolated development application.
set -eu
lan=${1:?Usage: run-direct-b7.sh PC-LAN-IPv4 [--safari-after-native-test]}
case "$lan" in *[!0-9.]*|'') echo 'Use a numeric PC LAN IPv4'; exit 2;; esac
app=''
for root in /media/developer/apps/usr/palm/applications /media/cryptofs/apps/usr/palm/applications; do
    candidate="$root/com.github.depius.uxair.dev"
    if [ -x "$candidate/uxair" ]; then app="$candidate"; break; fi
done
[ -n "$app" ] || { echo 'Development app binary not found'; exit 1; }
grep -q 'com.github.depius.uxair.dev' "$app/appinfo.json" || exit 1
for pid in $(pidof uxair 2>/dev/null || true); do
    if [ "$(readlink "/proc/$pid/exe")" = "$app/uxair" ]; then
        echo 'Close only com.github.depius.uxair.dev with ares-launch --close first'; exit 1
    fi
done
export UXAIR_DIRECT_VIDEO=1 UXAIR_DIRECT_LAN="$lan"
if [ "${2:-}" = '--safari-after-native-test' ]; then
    export UXAIR_DIRECT_DISCOVERY=after-native-test
else
    unset UXAIR_DIRECT_DISCOVERY || true
fi
cd "$app"
exec ./uxair
