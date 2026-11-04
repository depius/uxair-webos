#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 tests/network_bounds_test.py
python3 tests/ipk_permissions_test.py
python3 tests/source_package_test.py
python3 tests/candidate_assets_test.py
c++ -std=c++11 -g -fsanitize=address,undefined -Isrc tests/session_lifecycle_test.cpp -o build/tests/session-lifecycle-test
build/tests/session-lifecycle-test
python3 tests/teardown_origin_test.py
c++ -std=c++11 -g -Wall -Wextra -fsanitize=address,undefined -pthread -Itests/receiver_stub -Isrc tests/receiver_ingress_test.cpp src/receiver_ingress.cpp src/settings.cpp -o build/tests/receiver-ingress-test
python3 tests/receiver_ingress_log_test.py
c++ -std=c++11 -g -Wall -Wextra -fsanitize=address,undefined -pthread -Itests/receiver_stub -Isrc tests/hevc_reinit_test.cpp src/receiver_ingress.cpp src/settings.cpp -o build/tests/hevc-reinit-test
build/tests/hevc-reinit-test
c++ -std=c++11 -g -fsanitize=address,undefined -pthread -Isrc tests/now_playing_test.cpp -o build/tests/now-playing-test
build/tests/now-playing-test
c++ -std=c++11 -g -fsanitize=address,undefined -Isrc tests/secure_data_test.cpp -o build/tests/secure-data-test
build/tests/secure-data-test
python3 tests/build_platform_test.py
c++ -std=c++11 -g -fsanitize=address,undefined -Isrc tests/diagnostics_test.cpp -o build/tests/diagnostics-test
build/tests/diagnostics-test
echo 'PASS: arrival/PTS gap attribution, timing windows, duration samples and reset'
c++ -std=c++11 -g -fsanitize=address,undefined -Isrc tests/audio_session_test.cpp -o build/tests/audio-session-test
build/tests/audio-session-test
c++ -std=c++11 -g -fsanitize=address,undefined -Isrc tests/media_payload_test.cpp src/settings.cpp -o build/tests/media-payload-test
build/tests/media-payload-test
c++ -std=c++11 -g -fsanitize=address,undefined -Isrc \
  tests/media_open_failure_test.cpp -o build/tests/media-open-failure-test
build/tests/media-open-failure-test
python3 tests/media_identity_test.py
python3 tests/media_failure_path_test.py
if [[ "${1:-}" == "--diagnostics-only" ]]; then
  exit 0
fi
c++ -std=c++11 -g -fsanitize=address,undefined -Isrc \
  tests/state_test.cpp src/settings.cpp src/usage_stats.cpp -o build/tests/state-test
c++ -std=c++11 -g -fsanitize=address,undefined -Isrc tests/usage_stats_test.cpp src/usage_stats.cpp -o build/tests/usage-stats-test
build/tests/state-test
build/tests/usage-stats-test
c++ -std=c++11 -g -fsanitize=address,undefined -Isrc \
  tests/media_rate_test.cpp src/settings.cpp -o build/tests/media-rate-test
build/tests/media-rate-test
c++ -std=c++11 -g -fsanitize=address,undefined -Isrc \
  tests/ui_navigation_test.cpp src/ui_navigation.cpp -o build/tests/ui-navigation-test
build/tests/ui-navigation-test
python3 tests/history_dashboard_test.py
c++ -std=c++11 -g -fsanitize=address,undefined -Isrc -Itests/sdl_stub $(pkg-config --cflags sdl2 SDL2_ttf) \
  tests/history_render_test.cpp src/shell.cpp src/input.cpp src/ui_navigation.cpp src/usage_stats.cpp \
  $(pkg-config --libs sdl2 SDL2_ttf) -o build/tests/history-render-test
timeout 30s build/tests/history-render-test
c++ -std=c++11 -g -fsanitize=address,undefined -Isrc -Itests/sdl_stub $(pkg-config --cflags sdl2 SDL2_ttf) \
  tests/shell_signal_test.cpp src/shell.cpp src/input.cpp src/ui_navigation.cpp src/usage_stats.cpp \
  $(pkg-config --libs sdl2 SDL2_ttf) -o build/tests/shell-signal-test
timeout 10s build/tests/shell-signal-test term
timeout 10s build/tests/shell-signal-test int
c++ -std=c++11 -g -fsanitize=address,undefined -Isrc \
  tests/pairing_ui_test.cpp src/ui_navigation.cpp -o build/tests/pairing-ui-test
build/tests/pairing-ui-test
c++ -std=c++11 -g -fsanitize=address,undefined -pthread -Isrc \
  tests/pairing_registration_test.cpp -o build/tests/pairing-registration-test
build/tests/pairing-registration-test
python3 tests/pairing_render_test.py
python3 tests/donation_qr_test.py
python3 tests/donation_icon_test.py
python3 tests/pairing_secret_log_test.py
cc -std=c11 -D_POSIX_C_SOURCE=200809L -g -fsanitize=address,undefined -Isrc \
  tests/direct_request_diag_test.c src/direct_request_diag.c -o build/tests/direct-request-diag-test
build/tests/direct-request-diag-test
python3 tests/instrument_uxplay_test.py
python3 tests/direct_info_gate_test.py
python3 tests/b7_media_observe_test.py

cc -std=c11 -g -O1 -Wall -Wextra -fsanitize=address,undefined -Isrc \
  tests/hevc_config_guard_test.c -o build/tests/hevc-config-guard-test
build/tests/hevc-config-guard-test
tools/test-hevc-probe.sh
c++ -std=c++11 -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc \
  tests/hevc_startup_test.cpp -o build/tests/hevc-startup-test
build/tests/hevc-startup-test
echo 'PASS: bounded HEVC preroll before LOADCOMPLETED, delayed Play and finite failure path'
c++ -std=c++11 -g -O1 -Wall -Wextra -fsanitize=address,undefined -Isrc \
  tests/hevc_stream_test.cpp src/hevc_stream.cpp src/hevc_sample.cpp -o build/tests/hevc-stream-test
build/tests/hevc-stream-test build/hevc-native-samples/sample-1080p.hevc build/hevc-native-samples/sample-uhd.hevc build/hevc-native-samples/sample-portrait.hevc build/hevc-native-samples/sample-main10.hevc
echo 'PASS: sanitizer-backed receiver HEVC Annex B, profile, bounds, reconnect and resolution changes'
if ! pkg-config --exists sdl2; then
  echo 'SDL2 development files are required for input mapping tests.' >&2
  exit 2
fi
c++ -std=c++11 -g -fsanitize=address,undefined -Isrc $(pkg-config --cflags sdl2) \
  tests/input_test.cpp src/input.cpp -o build/tests/input-test
build/tests/input-test
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=1280x720 -frames:v 1 -c:v libx264 -profile:v baseline -f h264 build/tests/landscape.h264
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=720x1280 -frames:v 1 -c:v libx264 -profile:v high -f h264 build/tests/portrait.h264
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=1920x1080 -frames:v 1 -c:v libx264 -profile:v high -f h264 build/tests/full-hd.h264
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=1080x1920 -frames:v 1 -c:v libx264 -profile:v high -f h264 build/tests/full-hd-portrait.h264
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=2048x1080 -frames:v 1 -c:v libx264 -profile:v high -f h264 build/tests/unsupported.h264
ffmpeg -hide_banner -loglevel error -y -f lavfi -i sine=sample_rate=44100 -t 1 -ac 2 -c:a aac -f adts build/tests/audio.aac
ffmpeg_cflags="$(pkg-config --cflags libavcodec libswresample libavutil 2>/dev/null || true)"
ffmpeg_libs="$(pkg-config --libs libavcodec libswresample libavutil 2>/dev/null || true)"
if [ -z "$ffmpeg_libs" ] && command -v brew >/dev/null; then
  ffmpeg_root="$(brew --prefix ffmpeg)"
  ffmpeg_cflags="-I$ffmpeg_root/include"
  ffmpeg_libs="-L$ffmpeg_root/lib -lavcodec -lswresample -lavutil"
fi
if [ -z "$ffmpeg_libs" ]; then
  echo 'FFmpeg development files are required (pkg-config or Homebrew ffmpeg).' >&2
  exit 2
fi
c++ -std=c++11 -g -fsanitize=address,undefined -Isrc $ffmpeg_cflags \
  tests/media_test.cpp src/h264.cpp src/audio.cpp \
  $ffmpeg_libs -o build/tests/media-test
build/tests/media-test build/tests/landscape.h264 build/tests/portrait.h264 build/tests/audio.aac build/tests/full-hd.h264 build/tests/full-hd-portrait.h264 build/tests/unsupported.h264

# Actual NDL sources with host-only vendor stubs; not firmware playback validation.
cc -std=c11 -g -Wall -Wextra -fsanitize=address,undefined -pthread -Ddlsym=ndl_test_dlsym -Itests/ndl_stub -Isrc \
  -c src/ndl_backend.c -o build/tests/ndl-backend-test.o
c++ -std=c++11 -g -Wall -Wextra -fsanitize=address,undefined -pthread -DUXAIR_MEDIA_NDL -Itests/ndl_stub -Isrc $(pkg-config --cflags sdl2) \
  tests/ndl_media_test.cpp src/media_ndl.cpp src/settings.cpp build/tests/ndl-backend-test.o -o build/tests/ndl-media-test
build/tests/ndl-media-test
