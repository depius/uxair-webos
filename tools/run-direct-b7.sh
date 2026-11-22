#!/bin/sh
# Rooted B7 lab launcher: only the isolated development application.
set -eu
lan=${1:?Usage: run-direct-b7.sh PC-LAN-IPv4 [auto|bufferstream|native]}
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
backend=${2:-auto}
case "$backend" in auto|bufferstream|native) ;; *) echo 'Use auto, bufferstream or native'; exit 2;; esac
export UXAIR_DIRECT_VIDEO=1 UXAIR_DIRECT_LAN="$lan" UXAIR_DIRECT_BACKEND="$backend"
# Optional LAN-fixture entry point. Use the same jail and credentials as a native app.
export XDG_RUNTIME_DIR=/tmp/xdg
exec /usr/bin/jailer -t native_devmode -i com.github.depius.uxair.dev -p "$app" "$app/uxair" \
    '{"@system_native_app":true,"nid":"com.github.depius.uxair.dev"}'
