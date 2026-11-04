# Milestone 5C: isolated HEVC hardware decoder probe

> Historical development record. See [UxAir 1.2.0 scope](UXAIR_1_2_SCOPE.md) for current release and validation status.

The subsequent [Milestone 5D B7 follow-up](MILESTONE5D.md) documents the 0.1.1 probe, corrected ACB sink handling, current UHD block, and `luna-send` instructions. The procedure below describes the original 0.1.0 experiment.

The owner's Rockhopper 3.9.0 inspection found `H265`, `HEVC`, `video/x-h265` and the Starfish codec option string in `libpf-1.0.so.1.0.0`. The TV's codec capability file declares HEVC up to 4096×2176/60, and its resource file names `HEVCP1_HEVC`. Those are firmware-specific clues for trying `codec.video="H265"`; they do **not** establish that this B7 accepts HEVC `BUFFERSTREAM` or presents UHD from a third-party app. The probe copies the B7-proven H.264 Starfish/ACB call sequence into a separate executable, changes only its codec candidate and local test stream, and records each stage. The working receiver and its DNS-SD advertisement remain untouched.

## Isolation and safety

- App ID: `com.github.danlflh.uxplaywebos.hevcprobe`. It has no AirPlay code, network listener, launch-at-boot hook, settings or pairing files. Installation and icon launch without explicit parameters **do not run the decoder**.
- Startup now records only the argument count, bounded argument length, parse category and selected mode. It never writes the launch JSON itself. Native webOS may add fields such as `nid` and `@system_native_app` to the launch object; these do not enable the decoder. Only a top-level `mode` or `params.mode` set to `1080p` or `uhd` does. An absent, malformed, oversized, duplicate or unsupported mode stays idle.
- Only two generated, packaged local files are accepted: 1920×1080 and 3840×2160, 30-frame, HEVC Main 8-bit 4:2:0 SDR Annex B streams. No URL or external path is accepted. The parser requires AUD-delimited access units and VPS, SPS and PPS before VCL; it checks SPS profile, bit depth, dimensions, file size (12 MiB), access-unit size (2 MiB) and frame count (60 maximum). A malformed or mismatched stream stops before `Load`.
- The UHD launch is blocked until the 1080p run has accepted every feed and reported a matching Starfish video-info event plus a `FRAMEREADY` callback. The owner should also **visually confirm** 1080p before launching UHD. A callback is not proof of panel presentation.
- The probe starts feeding only after `Load()` returns true; it does not wait for `LOADCOMPLETED` before sending preroll bytes. It attaches ACB and requests Play when that event arrives. Load event: 8-second deadline; Play event: 5-second deadline; bounded 10 Feed retries per frame; drain: 5 seconds. SIGTERM/SIGINT lead to normal unload/finalize. A 45-second hard watchdog exits if a synchronous proprietary call hangs. Such an emergency exit cannot safely invoke the media API from a signal handler; use the recovery step below if ACB retains its registration.
- This is Main 8-bit SDR only. It does not test Main10, HDR, AirPlay HEVC, 60 FPS or direct-media playback. No CPU fallback is included.

## Build and validation

The separate [HEVC probe workflow](../.github/workflows/hevc-probe-ipk.yml) runs `tools/test-hevc-probe.sh` with AddressSanitizer and UndefinedBehaviorSanitizer on Ubuntu. On `macos-15` ARM64 it uses the same checksum-pinned Darwin ARM64 ARMv7 toolchain as the receiver, builds only the new target, generates the samples with FFmpeg/libx265, checks ARM ELF32 soft-float and GLIBC <= 2.19 using `tools/verify-elf.py`, validates the isolated IPK and sample identity, then publishes an artifact named `hevc-probe-armv7-<commit>`. `SHA256SUMS`, `BUILD_COMMIT.txt` and `ELF_REPORT.txt` accompany the `.ipk` and samples. The existing Ubuntu and receiver IPK workflows remain in place.

To reproduce locally on a supported macOS ARM64 host with CMake, Ninja, pkg-config, FFmpeg/libx265, Python 3 and webOS TV CLI 3.2.6:

```sh
tools/build-hevc-probe.sh
python3 tools/validate-hevc-probe-ipk.py
cd dist/hevc-probe && shasum -a 256 -c SHA256SUMS
```

The generated pattern and encoder settings are in `tools/generate-hevc-samples.sh`. The checksums describe the exact CI output; FFmpeg/libx265 version changes may change encoded bytes, so compare a fresh build against its own reported hashes rather than assuming byte-for-byte encoder output across hosts.

## Manual B7 procedure

Use a rooted B7 with the owner's existing `ares` device entry and SSH access. Download the **separate HEVC probe** artifact from the successful workflow for the intended source commit. Verify `SHA256SUMS` before installation. Leave the current UxPlay Dev package installed; close any active mirroring session so the media hardware is free.

```sh
ares-install -d YOUR_DEVICE com.github.danlflh.uxplaywebos.hevcprobe_0.1.0_arm.ipk
ares-launch -d YOUR_DEVICE com.github.danlflh.uxplaywebos.hevcprobe -p '{"mode":"1080p"}'
```

Watch the screen for moving test pattern playback, then collect the probe log. Rockhopper launches the native app inside a jail. The working UxPlay Dev receiver writes its app log under `/var/palm/jail/<app-id>/media/developer/apps/usr/palm/applications/<app-id>/`, so the equivalent probe path is the first place to check. This **probe path remains to be confirmed on the B7**. The probe also attempts the executable's installation directory and falls back to `/tmp/uxplay-hevc-probe.log` if that directory is not writable. Its `startup ... log=app|tmp` marker identifies which one worked. On the rooted TV:

```sh
APP=com.github.danlflh.uxplaywebos.hevcprobe
for FILE in \
  "/var/palm/jail/$APP/media/developer/apps/usr/palm/applications/$APP/hevc-probe.log" \
  "/media/developer/apps/usr/palm/applications/$APP/hevc-probe.log" \
  "/media/cryptofs/apps/usr/palm/applications/$APP/hevc-probe.log" \
  "/var/palm/jail/$APP/tmp/uxplay-hevc-probe.log" \
  /tmp/uxplay-hevc-probe.log; do
  if test -f "$FILE"; then echo "$FILE"; cat "$FILE"; fi
done
```

The original implementation required `argv[1]` to be byte-for-byte `{"mode":"1080p"}` or `{"mode":"uhd"}`. Any additional native launch fields made it return before opening a log, explaining the owner's `returnValue:true` plus absent log. A Luna success response confirms launch request acceptance, not that the probe recognized its mode or stayed running. If the corrected build logs `launch=no-mode`, inspect how that firmware actually delivers `params`; do not infer decoder failure. Preserve the full log for diagnosis; it contains result counters, no launch JSON, media bytes or personal device identifiers. A short-lived process is expected after an idle launch and after the finite decoder run. If the 1080p pattern is visible and the log ends with `decoder-path=PASS`, launch UHD separately:

```sh
ares-launch -d YOUR_DEVICE com.github.danlflh.uxplaywebos.hevcprobe -p '{"mode":"uhd"}'
```

If 1080p fails, **do not run UHD**. If the 1080p result is ambiguous, send the log and visual observation before proceeding. A missing `1080p-decoder-pass.flag` intentionally blocks UHD.

To stop a run and remove this test package later:

```sh
ares-launch -d YOUR_DEVICE -c com.github.danlflh.uxplaywebos.hevcprobe
ares-install -d YOUR_DEVICE --remove com.github.danlflh.uxplaywebos.hevcprobe
```

Closing the app should enter the normal unload/finalize path. If a hard timeout or forced kill leaves a black screen or a later ACB registration error, first close the probe, then on the rooted TV run `restart AcbService` (or reboot). This restarts the service in memory; it does not change firmware files. The separate UxPlay Dev app and its pairing state remain intact. Do not run the receiver and probe concurrently.

## Interpreting the log

`sample-valid` proves the local file's parsed VPS/SPS/PPS profile, bit depth and dimensions. `load-return accepted=1` means the Starfish request was accepted, **not** that a picture was decoded. `play-return` and `feed ... accepted=1` likewise do not prove video output. `event ... video_info=WxH` is firmware-reported decoder metadata; `frame_ready_events` is a callback count, not a presented-frame rate. `acb-window` and `sdl-canvas` describe requested/UI geometry, not the hardware output plane. Visual playback plus these events is useful 1080p decoder evidence. The code has **no independent output-plane resolution query**, so even visible UHD content and a 3840×2160 SPS will not, by themselves, prove native 3840×2160 scanout. Record any B7 service/plane report that independently states output resolution before calling the UHD presentation native.

The webOS SDK ACB header's two-argument `setMediaVideoData` declaration is incompatible with the firmware wrapper; the probe uses the same three-argument symbol binding as the working receiver and the [webosbrew SMP/ACB sample](https://github.com/webosbrew/samples/blob/main/media/smp/acb/acb_plane.c). The [webosbrew native sample documentation](https://github.com/webosbrew/samples) also confirms that `ares-launch -p` passes a JSON launch object as `argv[1]` to native apps. Both references establish call shape, not HEVC behavior on this B7.
