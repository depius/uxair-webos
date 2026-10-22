#!/bin/sh
# Native-only fixture comparison. Never starts RAOP, changes app data, or binds LAN.
set -eu
umask 0077
run_bounded_probe() {
    # BusyBox 1.24/1.26 (contemporary with B7) requires -t SECS; newer
    # BusyBox/coreutils use positional SECS. Both keep the same hard limit.
    if timeout --help 2>&1 | grep -q -- '-t SECS'; then
        timeout -s KILL -t 120 "$@"
    else
        timeout -s KILL 120 "$@"
    fi
}
lan=${1:?Usage: run-native-uri-probe-b7.sh PC-LAN-IPv4}
case "$lan" in *[!0-9.]*|'') echo '[native-probe] numeric fixture IPv4 required'; exit 2;; esac
app=''
for root in /media/developer/apps/usr/palm/applications /media/cryptofs/apps/usr/palm/applications; do
    candidate="$root/com.github.depius.uxair.dev"
    if [ -x "$candidate/direct-uri-probe" ]; then app="$candidate"; break; fi
done
[ -n "$app" ] || { echo '[native-probe] development probe not installed'; exit 1; }
grep -q 'com.github.depius.uxair.dev' "$app/appinfo.json" || exit 1
if pidof uxair >/dev/null 2>&1 || pidof direct-uri-probe >/dev/null 2>&1; then
    echo '[native-probe] close all UxAir receivers/probes first; no process was stopped'; exit 1
fi
command -v timeout >/dev/null || { echo '[native-probe] BusyBox timeout required'; exit 1; }
self_net=$(readlink /proc/self/ns/net 2>/dev/null || true)
init_net=$(readlink /proc/1/ns/net 2>/dev/null || true)
if [ -z "$self_net" ] || [ -z "$init_net" ]; then echo '[native-probe] namespace init=unavailable'
elif [ "$self_net" = "$init_net" ]; then echo '[native-probe] namespace init=same'
else echo '[native-probe] namespace init=different'; fi
ums_pids=$(pidof umediaserver uMediaServer 2>/dev/null || true)
[ -n "$ums_pids" ] || echo "[native-probe] namespace ums=not-found"
for pid in $ums_pids; do
    service_net=$(readlink "/proc/$pid/ns/net" 2>/dev/null || true)
    if [ -z "$service_net" ] || [ -z "$self_net" ]; then echo '[native-probe] namespace ums=unavailable'
    elif [ "$service_net" = "$self_net" ]; then echo '[native-probe] namespace ums=same'
    else echo '[native-probe] namespace ums=different'; fi
done
export UXAIR_NATIVE_PROBE_AUTHORIZED=fixture-only
unset UXAIR_DIRECT_DISCOVERY || true
cd "$app"
# A hard process deadline also bounds proprietary calls that never return. This
# process is exclusively the diagnostic binary; production and receiver PIDs are untouched.
set +e
run_bounded_probe ./direct-uri-probe "$lan" 2>native-uri-probe.log
result=$?
set -e
# Preserve every case summary; never send raw proprietary stderr or string payloads.
grep '^\[native-probe\]' native-uri-probe.log || true
grep -E '^\[direct-player\] (foreground|load-schema|load-return|event=|waiting|failure=)|^\[hls-relay\] (request=|stage=manifest-fetch|stage=segment-fetch)' native-uri-probe.log | tail -180 || true
echo "[native-probe] process-exit=$result"
exit "$result"
