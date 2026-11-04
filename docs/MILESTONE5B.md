# Milestone 5B: Rockhopper HEVC decoder discovery

> Historical development record. See [UxAir 1.2.0 scope](UXAIR_1_2_SCOPE.md) for current release and validation status.

**Status: stop before decoder initialization.** This phase found a concrete candidate `BUFFERSTREAM` codec token but no verified Rockhopper 3.9.0 HEVC Load/Feed contract. It adds no executable, package, AirPlay feature bit, pairing change or automatic TV action. The existing H.264 development IPK remains the rollback baseline.

## What the sources establish

| Question | Evidence | Status for the LG B7 |
| --- | --- | --- |
| Codec identifier | The independent [ss4s Starfish backend](https://github.com/mariotaku/ss4s/blob/main/modules/webos/smp/src/smp_video.c#L12-L22) maps HEVC to `H265` and places it in `externalStreamingInfo.contents.codec.video` in a [`BUFFERSTREAM` Load](https://github.com/mariotaku/ss4s/blob/main/modules/webos/smp/src/smp_player.c#L276-L309). Our proven [H.264 load](../src/media_starfish.cpp) uses the same object path with `H264`. | `H265` is a **candidate**, not a B7-verified token. It would be unsafe to substitute it into the AirPlay app on this evidence alone. |
| webOS 3.x compatibility | The [Moonlight TV maintainer's compatibility table](https://github.com/mariotaku/moonlight-tv/wiki/Compatibility-Status) marks SMP video available but H.265 **No** on webOS 3.x. A [C7/3.9.0 owner report](https://github.com/mariotaku/moonlight-tv/issues/67) describes 4K playback, but the displayed log used NDL and does not identify the codec of the 4K session. | These are conflicting or incomplete neighboring-device observations, not a B7 HEVC result. The negative 3.x report is a reason to demand a local contract and controlled probe. |
| TV product capability | [LG's webOS TV 3.5 table](https://webostv.developer.lge.com/develop/specifications/video-audio-35) lists HEVC Main/Main10 for UHD models, with up to 4096×2160/60 limits. | This describes supported playback formats, not this native app's raw `BUFFERSTREAM` decoder entry point. |
| Elementary stream and parameter sets | UxPlay's pinned [HEVC mirror branch](https://github.com/FDH2/UxPlay/blob/9f3c2bbc658645533fa1057e76c29ec8c947fa0f/lib/raop_rtp_mirror.c#L625-L708) converts VPS/SPS/PPS to Annex B. Upstream GStreamer declares HEVC byte-stream caps. The ss4s SMP backend feeds the stream bytes directly but does not document a B7-specific VPS/SPS/PPS or HVCC requirement. | Annex B is the first candidate for an isolated test; acceptance and required initialization on Rockhopper are **unverified**. Do not use AirPlay packets as the first test source. |
| Load, buffers, lifecycle | The independent [webosbrew SMP sample](https://github.com/webosbrew/samples/blob/main/media/smp/common/smp_payload.c) documents the `BUFFERSTREAM`/`RAW`/`Chrome` JSON shape, nanosecond PTS and source-buffer settings. Its [ACB sample](https://github.com/webosbrew/samples/blob/main/media/smp/acb/acb_plane.c) attaches the media ID, sets a display window and transitions ACB states. That sample only stages H.264 Annex B and AAC, and reports a webOS 3.4 H.264 device test. Our app's [existing H.264 path](../src/media_starfish.cpp) has run successfully on this B7. | The **H.264** lifecycle is evidenced; HEVC Main 8-bit dimensions, maximum access-unit size, Feed behavior and teardown on this firmware remain unknown. The 2 MiB limit in our app is not a measured HEVC limit. |
| SDK and local artifacts | The [pinned toolchain](../dependencies.lock.json) supplies ABI headers and link stubs, and [build.sh](../tools/build.sh) downloads it on macOS. This Windows checkout has no `.cache`, `build`, `dist`, bundled firmware libraries or TV headers. The public [Starfish header](https://www.webosbrew.org/webos-userland/d3/dda/StarfishMediaAPIs_8h_source.html) declares `Load`, `Feed` and `smp::util` capability-query functions; it does not define accepted codec strings or expose this B7's resource policy. | Compilation against a stub would prove linkage, not decoder support. No existing artifact establishes the Rockhopper HEVC contract. |
| UHD output evidence | [Our ACB path](../src/media_starfish.cpp) sets a display window. Starfish video-info and ACB window coordinates can report geometry but are not independent proof of native panel scanout. [LG distinguishes graphics from UHD video resolution](https://webostv.developer.lge.com/develop/specifications/app-resolution). | No B7 output-plane resolution query or hardware result has been identified. A 3840×2160 SPS and accepted Feed would still not prove UHD presentation. |

## Minimal read-only discovery on the rooted B7

Open an interactive SSH shell using the owner's existing root access. For example, in PowerShell, with `TV_HOST` and `TV_KEY` set to the known TV address and key path:

```powershell
ssh -i $env:TV_KEY -o BatchMode=yes -o StrictHostKeyChecking=yes "root@$env:TV_HOST"
```

Then run the following commands **on the TV**, one block at a time. They only read system files and print bounded file names, symbol names and fixed media option/codec tokens. They do not call `Load`, start playback, change settings or scan private app data. A missing path or utility is useful information; do not install a replacement on the TV.

```sh
uname -m
for f in /usr/lib/libplayerAPIs.so* /usr/lib/libpf-1.0.so* /usr/lib/libAcbAPI.so*; do
    [ -e "$f" ] && ls -l "$f"
done
```

```sh
for d in /usr/include /usr/local/include; do
    [ -d "$d" ] && find "$d" -maxdepth 5 -type f \( -name 'StarfishMediaAPIs.h' -o -name 'AcbAPI.h' -o -name 'dile_vt.h' \) -print 2>/dev/null
done
```

```sh
if command -v strings >/dev/null 2>&1; then
    for f in /usr/lib/libpf-1.0.so.1.0.0 /usr/lib/libplayerAPIs.so.1.0.0; do
        [ -r "$f" ] || continue
        printf '\n%s\n' "$f"
        strings "$f" | grep -E '^(H264|H265|HEVC|h264|h265|hevc|video/x-h265|option\..*(codec|adaptiveStreaming|srcBufferLevelVideo).*)$' | sort -u | head -80
    done
fi
```

```sh
if command -v readelf >/dev/null 2>&1; then
    for f in /usr/lib/libplayerAPIs.so.1.0.0 /usr/lib/libAcbAPI.so.1.0.0; do
        [ -r "$f" ] || continue
        printf '\n%s\n' "$f"
        readelf -Ws "$f" 2>/dev/null | grep -E 'getMaxVideoResolution|isSupportedVideo|StarfishMediaAPIs|AcbAPI_set(DisplayWindow|MediaVideoData)' | head -60
    done
fi
```

```sh
for d in /etc/umediaserver /usr/share/umediaserver; do
    [ -d "$d" ] && grep -rilE 'H265|HEVC' "$d" 2>/dev/null | head -20
done
```

Share the output of these five blocks, not a full firmware log or configuration file. The final block lists matching **paths only**. If the files are elsewhere or `strings`/`readelf` is absent, report that result rather than changing the device. Do not post raw pairing files, app-private data, full binary dumps or unfiltered logs.

## Decision gate after discovery

The returned evidence must identify which Rockhopper component accepts `H265`, the `BUFFERSTREAM` raw elementary-stream form and any codec/resource restriction. If that remains ambiguous, stop again. If it is credible, the next change is a **separate, manually launched local probe** with its own app ID and app-private test file: HEVC Main 8-bit SDR Annex B at 1920×1080/30 first, then 3840×2160/30 only after the first run succeeds. Bound the file and access-unit size, use finite Load/Play/Feed/teardown deadlines, and record SPS profile/bit depth, Feed results, Starfish events, ACB window, memory and any independent UHD output-plane signal. No CPU 4K fallback, listener, boot hook or change to the existing AirPlay application belongs in that probe.
