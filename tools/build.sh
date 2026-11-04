#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
variant=production; cmake_variant=OFF
media=ndl   # ndl: webOS 5 and later (webOS 10); starfish: webOS 3-4
for arg in "$@"; do
  case "$arg" in
    --development) variant=development; cmake_variant=ON ;;
    --media=ndl|--media=starfish) media=${arg#--media=} ;;
    *) echo 'Usage: tools/build.sh [--development] [--media=ndl|starfish]' >&2; exit 2 ;;
  esac
done
python3 tools/bootstrap.py
tools/build-deps.sh
# no cmake/ninja on the host: the SDK's (the Linux SDK bundles both)
command -v cmake >/dev/null && command -v ninja >/dev/null ||
  export PATH="$PATH:$PWD/.cache/arm-webos-linux-gnueabi_sdk-buildroot/bin"
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/.cache/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake" \
  -DBUILD_RECEIVER=ON -DBUILD_DEVELOPMENT_PACKAGE="$cmake_variant" -DUXAIR_MEDIA="$media"
cmake --build build --parallel 6
printf '%s\n' "$variant" > build/package-variant.txt
printf '%s\n' "$media" > build/media-backend.txt
if [[ "$variant" == development ]]; then
  python3 tools/package.py --development
else
  python3 tools/package.py
fi
