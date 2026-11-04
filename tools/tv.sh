#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
tv_host="${TV_HOST:-}"
tv_key="${TV_KEY:-}"
tv_device="${TV_DEVICE:-}"
ssh_options=(-i "$tv_key" -o BatchMode=yes -o StrictHostKeyChecking=yes -o ConnectTimeout=8 -o LogLevel=ERROR)
app_id="$(python3 -c 'import json;print(json.load(open("packaging/appinfo.json"))["id"])')"
app_dir="/media/developer/apps/usr/palm/applications/$app_id"
app_jail_dir="/var/palm/jail/$app_id$app_dir"
dev_id="$app_id.dev"
dev_jail_dir="/var/palm/jail/$dev_id/media/developer/apps/usr/palm/applications/$dev_id"
require_device() { [ -n "$tv_device" ] || { echo 'TV_DEVICE is required (configured ares device name).' >&2; exit 2; }; }
require_ssh() { [ -n "$tv_host" ] && [ -n "$tv_key" ] || { echo 'TV_HOST and TV_KEY are required for direct SSH actions.' >&2; exit 2; }; }
case "${1:-}" in
  install)
    require_device
    version="$(python3 -c 'import json;print(json.load(open("packaging/appinfo.json"))["version"])')"
    backend="$(cat build/media-backend.txt)"
    case "$backend" in
      starfish) platform=webos3-4 ;;
      ndl) platform=webos5plus ;;
      *) echo 'Unknown media backend; rebuild before installing.' >&2; exit 2 ;;
    esac
    python3 tools/package.py
    ares-install -d "$tv_device" "dist/${app_id}_${version}_arm-${platform}.ipk"
    ;;
  launch) require_device; ares-launch -d "$tv_device" "$app_id" ;;
  close) require_device; ares-launch -d "$tv_device" -c "$app_id" ;;
  update-binary)
    require_device; require_ssh
    # Atomic replacement avoids ETXTBSY and preserves on-device pairing state.
    python3 tools/package.py
    scp "${ssh_options[@]}" build/app/uxair "root@$tv_host:$app_dir/uxair.next"
    ares-launch -d "$tv_device" -c "$app_id"
    ssh -tt "${ssh_options[@]}" "root@$tv_host" "chmod 755 '$app_dir/uxair.next' && mv '$app_dir/uxair.next' '$app_dir/uxair'"
    ares-launch -d "$tv_device" "$app_id"
    ;;
  logs)
    require_ssh
    mkdir -p build/logs
    ssh -tt "${ssh_options[@]}" "root@$tv_host" "cat '$app_jail_dir/uxair.log'" > build/logs/tv-latest.log
    echo "build/logs/tv-latest.log"
    ;;
  direct-probe-logs)
    require_ssh
    mkdir -p build/logs
    # Export only fixed-vocabulary probe records from the development app.
    ssh -T "${ssh_options[@]}" "root@$tv_host" "grep '^\[direct-probe\]' '$dev_jail_dir/uxair.log' || true" > build/logs/direct-probe.log
    echo "build/logs/direct-probe.log"
    ;;
  status)
    require_ssh
    ssh -tt "${ssh_options[@]}" "root@$tv_host" "ps -ef | grep '[u]xair'; tail -n 8 '$app_jail_dir/uxair.log'"
    ;;
  loader-check)
    require_ssh
    ssh -tt "${ssh_options[@]}" "root@$tv_host" "LD_TRACE_LOADED_OBJECTS=1 LD_BIND_NOW=1 '$app_dir/uxair'"
    ;;
  *) echo 'Usage: tools/tv.sh install|launch|close|update-binary|logs|direct-probe-logs|status|loader-check' >&2; exit 2 ;;
esac
