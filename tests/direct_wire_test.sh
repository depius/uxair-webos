#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
out="$root/build/tests/direct-wire"
mkdir -p "$out"
vendor="$out/source"
if [[ $# -gt 0 ]]; then
    vendor="$(cd "$1" && pwd)"
elif [[ ! -f "$vendor/lib/raop.c" ]]; then
    url="$(python3 -c 'import json; print(json.load(open("dependencies.lock.json"))["uxplay"]["repository"])')"
    revision="$(python3 -c 'import json; print(json.load(open("dependencies.lock.json"))["uxplay"]["commit"])')"
    git init --quiet "$vendor"
    git -C "$vendor" fetch --quiet --depth=1 "$url" "$revision"
    git -C "$vendor" checkout --quiet --detach FETCH_HEAD
fi
lib="$vendor/lib"
python3 tools/instrument-uxplay.py "$lib" "$out" --direct
python3 tools/instrument-lifecycle.py "$lib" "$out/raop.c" "$out/raop_rtp_mirror.c" "$out"

sources=("$out"/lifecycle-*.c "$out/http_request.c" "$out/httpd.c"
    "$lib"/llhttp/*.c "$lib"/playfair/*.c "$lib"/mdnsd/*.c
    "$root/src/direct_request_diag.c" "$root/src/direct_wire.c")
for source in "$lib"/*.c; do
    case "${source##*/}" in
        raop.c|raop_rtp.c|raop_rtp_mirror.c|http_request.c|httpd.c) ;;
        *) sources+=("$source") ;;
    esac
done

cd "$out"
includes=(-I"$root/src" -I"$lib" -I"$lib/llhttp" -I"$lib/playfair" -I"$lib/mdnsd")
sanitize=(-g -fsanitize=address,undefined -pthread)
# Match the cross-build options for the pinned library.
cc -std=gnu11 "${sanitize[@]}" "${includes[@]}" -DUXAIR_DIRECT_BUILD -DPLIST_210 -DPLIST_230 \
    -DOPENSSL_API_COMPAT=0x10101000L -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 \
    $(pkg-config --cflags libplist-2.0 openssl) -c "${sources[@]}"
c++ -std=c++11 "${sanitize[@]}" "${includes[@]}" -DUXAIR_DEV_BUILD -DUXAIR_DIRECT_BUILD \
    $(pkg-config --cflags libplist-2.0 openssl sdl2) \
    "$root/tests/direct_wire_test.cpp" "$root/src/receiver.cpp" "$root/src/receiver_ingress.cpp" \
    "$root/src/direct_video.cpp" "$root/src/settings.cpp" ./*.o \
    $(pkg-config --libs libplist-2.0 openssl) -ldl -lm -o direct-wire-test
if ! timeout 35s ./direct-wire-test 2>wire.log; then
    cat wire.log
    exit 1
fi
grep -F 'path=/command protocol=RTSP/1.0 route=unhandled status=200 category=media-control' wire.log
grep -F 'detail=updateMRSupportedCommands' wire.log
grep -F 'path=/play protocol=RTSP/1.0 route=direct status=200 category=direct-video' wire.log
grep -F 'play status=409 admission=audio-only' wire.log
grep -F 'play status=409 admission=mirroring' wire.log
grep -F 'descriptor=https-hls queued=1' wire.log
if grep -F -e 'wire-session-private' -e 'WIRE-PRIVATE' -e 'media.example' wire.log; then
    echo 'AirPlay trace leaked a request value' >&2
    exit 1
fi
