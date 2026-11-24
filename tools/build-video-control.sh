#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode="${1:-arm}"
case "$mode" in
    arm|host|test) ;;
    *) echo 'Usage: bash tools/build-video-control.sh [arm|host|test]' >&2; exit 2 ;;
esac

module=tests/airplay_control_phase1
cache="$PWD/.cache/video-control"
mkdir -p "$cache" build
read -r go_pin mirror_pin < <(python3 -B - "$module" <<'PYTHON'
import sys
sys.path.insert(0, sys.argv[1])
from check import GO_ATV, IPHONE_MIRROR
print(GO_ATV, IPHONE_MIRROR)
PYTHON
)
checkout() {
    local path="$cache/$1" url="$2" sha="$3"
    if [[ ! -d "$path/.git" ]]; then
        git init --quiet "$path"
    fi
    if ! git -C "$path" cat-file -e "$sha^{commit}" 2>/dev/null; then
        git -C "$path" fetch --quiet --depth=1 "$url" "$sha"
    fi
}
checkout go-atv https://github.com/jkiddo/go-atv.git "$go_pin"
checkout iphone-mirror https://github.com/RayrenSX/iPhoneMirror.git "$mirror_pin"
refs=("$cache/go-atv" "$cache/iphone-mirror")
if [[ "$mode" == test ]]; then
    python3 "$module/check.py" "${refs[@]}"
elif [[ "$mode" == host ]]; then
    mkdir -p build/tests/direct-wire
    python3 "$module/check.py" "${refs[@]}" --shared-output build/tests/direct-wire/libuxair-video-control.so
else
    sdk="$PWD/.cache/arm-webos-linux-gnueabi_sdk-buildroot"
    GOARCH=arm CC="$sdk/bin/arm-webos-linux-gnueabi-gcc" \
        CGO_LDFLAGS='-Wl,--hash-style=both -Wl,-soname,libuxair-video-control.so' \
        python3 "$module/check.py" "${refs[@]}" --shared-output build/libuxair-video-control.so \
            --reference-archive "$cache/upstream-video-control-source.tar"
fi
