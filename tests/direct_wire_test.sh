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

sources=("$out"/lifecycle-*.c "$out/http_request.c" "$out/httpd.c" "$out/pairing.c"
    "$lib"/llhttp/*.c "$lib"/playfair/*.c "$lib"/mdnsd/*.c
    "$root/src/direct_request_diag.c" "$root/src/direct_wire.c" "$root/src/direct_crypto.c")
for source in "$lib"/*.c; do
    case "${source##*/}" in
        raop.c|raop_rtp.c|raop_rtp_mirror.c|http_request.c|httpd.c|pairing.c) ;;
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
grep -E 'path=/pair-setup protocol=RTSP/1.0 route=video-pairing status=200 .*reason=public-key-only .*cseq=absent session=1 rtsp_sid=[0-9a-f]{16} session_header=1 sid=[0-9a-f]{16}' wire.log
grep -E 'path=/pair-verify protocol=HTTP/1.1 route=video-pairing status=200 .*reason=proof-verified auth=video-verified' wire.log
grep -E 'path=/pair-verify protocol=RTSP/1.0 route=video-pairing status=470 .*reason=proof-rejected' wire.log
grep -E 'path=/fp-setup protocol=HTTP/1.1 route=direct status=200 .*reason=fairplay-v3-handshake auth=video-verified cseq=absent' wire.log
grep -E 'path=/fp-setup protocol=HTTP/1.1 route=direct status=470 .*reason=video-proof-required' wire.log
grep -E 'path=/setProperty protocol=HTTP/1.1 route=direct status=200 .*control=forwardEndTime control_body=cm-time' wire.log
grep -E 'path=/scrub protocol=HTTP/1.1 route=direct status=409 .*reason=no-video-session .*control=position-query control_body=dict-with-uuid' wire.log
grep -E 'path=/fp-setup2 protocol=HTTP/1.1 route=direct status=470 .*reason=pair-bound-proof-required .*fp_envelope=legacy-control-envelope-shape' wire.log
grep -E 'path=/setProperty protocol=HTTP/1.1 route=direct status=501 .*control=selectedMediaArray control_body=array media_array=2:\[dict\(2\)\{mediaType:string,unlisted:string\},boolean\]' wire.log
grep -E 'point=listener-stop .*video=[1-9][0-9]* .*pairing_failures=[1-9][0-9]* unknown=[1-9][0-9]* admitted=3' wire.log
python3 - <<'PYTHON'
import re
from pathlib import Path

lines = Path("wire.log").read_text().splitlines()
details = [line for line in lines if "reason=public-key-only" in line or "reason=proof-verified" in line
           or "path=/play protocol=RTSP/1.0 route=direct status=200" in line]
correlated = [re.search(r" conn=(\d+).* sid=([0-9a-f]{16})", line).groups() for line in details]
assert len(set(conn for conn, _ in correlated)) >= 3
assert len(set(sid for _, sid in correlated)) == 1
PYTHON
if grep -F -e 'wire-session-private' -e 'WIRE-PRIVATE' -e 'media.example' -e 'WIRE-RTSP-PRIVATE' -e 'WIRE-DIFFERENT-OWNER' -e 'WIRE-FP-PRIVATE-DATA' -e 'WIRE-PLIST-PRIVATE' -e 'WIRE-QUERY-PRIVATE' -e 'WIRE-SCRUB-PRIVATE' -e 'WIRE-MEDIA-PRIVATE' -e 'WIRE-KEY-PRIVATE' wire.log; then
    echo 'AirPlay trace leaked a request value' >&2
    exit 1
fi
if [[ "${UXAIR_RUN_NATIVE_VIDEO_TEST:-}" == 1 ]]; then
    bash "$root/tools/build-video-control.sh" host
    UXAIR_NATIVE_VIDEO_TEST="$out/direct-wire-test" LD_LIBRARY_PATH="$out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        bash "$root/tools/build-video-control.sh" test
fi
