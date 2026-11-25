#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
sdk="$root/.cache/arm-webos-linux-gnueabi_sdk-buildroot"
prefix="$root/.cache/deps"
# the host's own perl, found before the SDK's (a Linux SDK bundles a perl whose
# module paths point at the machine it was built on)
host_perl="$(command -v perl)"
export PATH="$sdk/bin:$PATH"
export CC="$sdk/bin/arm-webos-linux-gnueabi-gcc" CXX="$sdk/bin/arm-webos-linux-gnueabi-g++"
export AR="$sdk/bin/arm-webos-linux-gnueabi-ar" RANLIB="$sdk/bin/arm-webos-linux-gnueabi-ranlib"
export CFLAGS="--sysroot=$sdk/arm-webos-linux-gnueabi/sysroot -O2 -fPIC"
export CXXFLAGS="$CFLAGS -D_GLIBCXX_USE_CXX11_ABI=0"
export LDFLAGS="--sysroot=$sdk/arm-webos-linux-gnueabi/sysroot"
mkdir -p "$prefix" build/logs
if command -v sha256sum >/dev/null; then sha256() { sha256sum "$1"; }; else sha256() { shasum -a 256 "$1"; }; fi
ffmpeg_input=(--disable-avformat)
if [[ "${1:-}" == --direct ]]; then
    ffmpeg_input=(--enable-avformat --enable-demuxer=mpegts,mov --enable-parser=aac --enable-bsf=h264_mp4toannexb)
fi
build_stamp="$(sha256 "$0" | cut -d ' ' -f 1):${1:-base}"
previous_stamp="$(cat "$prefix/build-config.sha256" 2>/dev/null || true)"
if [ "$build_stamp" != "$previous_stamp" ] || [ ! -f "$prefix/lib/libcrypto.a" ]; then
    (cd .cache/openssl-3.5.8
     "$host_perl" ./Configure linux-armv4 no-shared no-tests no-module no-async '-DDEVRANDOM=\"/dev/urandom\"' --prefix="$prefix" --libdir=lib
     make -j6 build_libs
     make install_dev) >build/logs/openssl.log 2>&1
fi
if [ "$build_stamp" != "$previous_stamp" ] || [ ! -f "$prefix/lib/libplist-2.0.a" ]; then
    (cd .cache/libplist-2.7.0
     ./configure --host=arm-webos-linux-gnueabi --prefix="$prefix" --disable-shared --enable-static --without-cython --without-tests
     make -j6
     make install) >build/logs/libplist.log 2>&1
fi
if [ "$build_stamp" != "$previous_stamp" ] || [ ! -f "$prefix/lib/libavcodec.a" ]; then
    (cd .cache/ffmpeg-8.1.2
     ./configure --prefix="$prefix" --arch=arm --cpu=cortex-a9 --target-os=linux --enable-cross-compile \
       --sysroot="$sdk/arm-webos-linux-gnueabi/sysroot" \
       --extra-cflags=-Dstatic_assert=_Static_assert \
       --cross-prefix="$sdk/bin/arm-webos-linux-gnueabi-" --disable-everything --disable-programs --disable-doc \
       --disable-shared --enable-static --enable-pic --disable-autodetect --disable-network \
       --disable-avdevice "${ffmpeg_input[@]}" --disable-avfilter --disable-swscale \
       --enable-avcodec --enable-avutil --enable-swresample --enable-decoder=aac,alac,pcm_s16le \
       --enable-parser=h264 --enable-small --disable-debug
     make -j6
     make install) >build/logs/ffmpeg.log 2>&1
fi
if [[ "${1:-}" == --direct ]] && { [ "$build_stamp" != "$previous_stamp" ] || [ ! -f "$prefix/lib/libcurl.a" ]; }; then
    (cd .cache/curl-8.23.0
     # The SDK pkg-config wrapper prepends its sysroot to absolute private
     # prefixes. Use the pinned OpenSSL headers/libraries directly here.
     # Use curl's portable socket paths to keep the B7 launcher libc baseline.
     ac_cv_func_accept4=no ac_cv_func_pipe2=no ac_cv_func_eventfd=no \
     PKG_CONFIG=false CPPFLAGS="-I$prefix/include" LDFLAGS="$LDFLAGS -L$prefix/lib" ./configure --host=arm-webos-linux-gnueabi --prefix="$prefix" --disable-shared --enable-static \
       --with-openssl="$prefix" --without-libpsl --without-zlib --without-brotli --without-zstd \
       --without-libidn2 --without-libssh2 --without-librtmp --without-nghttp2 --without-ngtcp2 --without-nghttp3 --disable-ldap --disable-ldaps \
       --disable-ftp --disable-file --disable-dict --disable-telnet --disable-tftp --disable-pop3 \
       --disable-imap --disable-smtp --disable-gopher --disable-mqtt --disable-rtsp --disable-smb \
       --enable-threaded-resolver --disable-manual --disable-docs --disable-libcurl-option \
       --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt
     make -j6
     make install) >build/logs/curl.log 2>&1
fi
printf '%s\n' "$build_stamp" > "$prefix/build-config.sha256"
echo "Dependencies ready in $prefix"
