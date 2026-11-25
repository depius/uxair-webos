# UxAir 1.4.0-dev launcher recovery and LG B7 acceptance

## Startup investigation

The published 1.3.0 Starfish IPK and the failing e3e693d dev IPK both register a native app with main executable uxair. Both package the executable as 0755, use the same ARM interpreter, DT_NEEDED library list and $ORIGIN/lib lookup, and ship byte-identical libstdc++.so.6. The installed identity differs by the deliberate .dev suffix. Both builds select and validate private storage before SDL, and a failure there prevents the persistent log from updating. The baseline state checks have not changed.

The e3e693d executable raised its launch-time dependencies from GLIBC_2.7 to GLIBC_2.10 for accept4 and from GLIBCXX_3.4.29 to GLIBCXX_3.4.30 for the untimed condition-variable wait. It also made Starfish Seek and getCurrentPlaytime mandatory imports. The corrected build restores the published launcher version ceilings and checks all three URI-specific Starfish methods as optional ELF imports. If the playtime or seek method is unavailable, auto mode can use BUFFERSTREAM without creating a native URI player. These are verified differences in the executable. The reported root-shell run and missing launcher log do not identify which early stage failed on this TV. [Starfish declarations](https://www.webosbrew.org/webos-userland/d3/dda/StarfishMediaAPIs_8h_source.html)

The prior B7 jail ran as UID 5999 and GID 5000. UID 1001 is the reported installer-side owner for this development app. If the jailed process still runs as 5999 and sees a 0700 state directory owned by 1001, it cannot open it. CI reproduces that failure and verifies that a clean state directory is created and reopened as the jailed UID. The developer build now reports the effective UID and errno on an on-screen Diagnostics page when secure storage validation fails. Existing ownership checks still apply. The receiver starts after validated storage and SDL initialization. [B7 storage findings](B7_STORAGE_FIX.md) · [Native jail documentation](https://www.webosbrew.org/pages/native-development.html)

## Test with the normal launcher

1. Check the corrected IPK against the artifact's SHA256SUMS. Choose the .dev IPK and install it over the development app. Keep any existing private state. Use the TV launcher to open **UxAir 1.4.0-dev**. From PowerShell, the equivalent application-manager commands are below.

   ```powershell
   Get-FileHash .\com.github.depius.uxair.dev_1.4.0_arm-webos3-4.ipk -Algorithm SHA256
   ares-install -d b7 .\com.github.depius.uxair.dev_1.4.0_arm-webos3-4.ipk
   ares-launch -d b7 com.github.depius.uxair.dev
   ```

2. Confirm the complete UxAir UI and Magic Remote navigation. Pick **UxAir Dev** from the iPhone and confirm that the PIN is visible on the TV. Pair, close, and relaunch to check that the development app reuses the same pairing and settings.
3. Reopen the development app from TV Home to reset the trace counters. In iPhone Safari, AirPlay Apple BipBop gear 2 to **UxAir Dev**. Look for `load-returned accepted=1`, `first-feed-accepted` for video and audio, `load-completed`, `play-request`, `playing`, `first-decoder-frame` and `decoder-progress`. The first accepted feeds can precede `load-completed`. Check moving picture and synchronized audible audio for at least 25 seconds to cross multiple segments. Test pause, resume, stop and reconnect, then repeat with Mux. [Apple BipBop gear 2](https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_4x3/gear2/prog_index.m3u8) · [Mux 720p](https://test-streams.mux.dev/x36xhzz/url_0/193039199_mp4_h264_aac_hd_7.m3u8)
4. If the startup Diagnostics page appears, report its effective UID and filesystem error. If the UI appears and playback fails, wait about 30 seconds after selecting AirPlay. Disconnect AirPlay on the iPhone and exit UxAir with Back to record the final listener totals. Collect the single filtered log below before relaunching or trying mirroring. Include whether the TV displayed moving video or played audio, and the iOS version. Keep the lines for the same conn and sid together so verification, server-info, fp-setup, property and seek responses, and the first play request can be read in seq order. Include the listener-stop totals.

   ```powershell
   $tvIp = '192.168.1.27'
   ssh "root@$tvIp" 'id=com.github.depius.uxair.dev; for base in "/var/palm/jail/$id" ""; do for r in /media/developer/apps/usr/palm/applications /media/cryptofs/apps/usr/palm/applications /media/developer/apps/var/lib; do p=$base$r/$id/state/uxair.log; if [ -f "$p" ]; then grep -E "^\[(startup|airplay-wire|airplay-wire-summary|video-crypto|video-control|reverse-channel|direct-player|direct-hls|direct-bufferstream|direct-protocol|direct-session|direct-session-summary|direct-audio|hls-relay|media-init|media-state)\]" "$p" | tail -230; fi; done; done'
   ```

The development build defaults to Direct Video on and BUFFERSTREAM. Explicit `UXAIR_DIRECT_BACKEND=native` runs the URI experiment, and explicit `UXAIR_DIRECT_BACKEND=auto` attempts it before BUFFERSTREAM. The isolated six-case probe is available through the scripts in tools. Use the TV launcher for the retest so the app runs with its native credentials.

## Phase 3 load failure

The B7 reported success through encrypted `/reverse`, encrypted `/play`, relay startup, ACB initialization and Starfish construction. The last application line was `stage=load begin`. The failing call was `StarfishMediaAPIs::Load` with the experimental `args-hls` envelope, which supplies a URI and sets `args[0].mediaTransportType` to `HLS`. The firmware reported a Boost property-tree lookup failure for `plugins`. The exception names the lookup key. Identifying the tree it came from or its expected value requires the B7 implementation or a backtrace with symbols.

The shipped v1.3.0 Starfish backend uses the same private C++ entry point with ACB role MSE and `args[0].mediaTransportType=BUFFERSTREAM`. Its typed `option.externalStreamingInfo` configures H.264 and PCM packet input and has no `plugins` field. LG's Chromium Starfish source uses the same feed contract. LG's TV web-media guide covers playback of HLS URLs through an HTML media element. The separate webOS OSE `com.webos.media/load` service has a `uri`, `type` and `payload` envelope, with no documented `plugins` request field. These sources establish distinct entry points. The availability of the URI service to a jailed native B7 app and the private Starfish URI contract remain unverified. [v1.3.0 native backend](https://github.com/depius/uxair-webos/blob/d453a49cee41b0ce5e0f2d3654c3e658da776c73/src/media_starfish.cpp) · [LG Chromium Starfish implementation](https://github.com/webosose/chromium84-temp/blob/df04aa361ad493d47003a80ebfa3034dfc9de6e0/src/media/neva/webos/media_platform_api_webos_smp.cc) · [LG TV media options](https://webostv.developer.lge.com/develop/guides/mediaoption-parameter) · [webOS OSE media service](https://www.webosose.org/docs/reference/ls2-api/com-webos-media/)

Dev now fetches and demuxes supported HLS in UxAir, then calls Starfish BUFFERSTREAM with H.264 and PCM. Both development load paths report the API, ACB role, source and allowlisted JSON field types. Synchronous recoverable Load exceptions retire the callback and trigger teardown. A failed Unload prevents decoder reuse. Device-side buffering and picture and audio still require the B7 retest.

## BUFFERSTREAM demux follow-up

The B7 download trace's video stream, PTS 903000, DTS 906000 and 1077-byte MPEG-TS packet match Apple BipBop gear 2's first segment. FFmpeg identifies H.264 Main at 640×480 with 8-bit 4:2:0 video and stereo AAC-LC at 22050 Hz. In the previous build, `src/direct_hls.cpp::adts_aac_lc` rejected sampling-frequency index 7 at `(rate != 3 && rate != 4)`. `direct_demux_ts` returned `Unsupported` at its `!adts.sample_rate` branch, which the download worker reported as `unsupported-stream`. The attempts without a segment download also fit Apple’s master playlist. Its two codec names are separated by a comma and a space, which the previous variant selector treated as part of the codec identifier. [Previous demux source](https://github.com/depius/uxair-webos/blob/867b050a268caee6df209a82f3ae8039f7e6a190/src/direct_hls.cpp) · [Apple master](https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_4x3/bipbop_4x3_variant.m3u8)

FFmpeg emits that warning before parsing the packet and continues. BipBop has signed composition offsets and ordered PES decode timestamps. FFmpeg's generic timestamp fill-in can replace the explicit DTS and make the parser output appear out of order. UxAir disables that fill-in for HLS. It keeps H.264 access units in decode order and feeds their presentation times relative to a shared audio/video origin. It can infer DTS for an initial IDR when the value is absent, retain packet order across equal decode times and reject large offsets or backward decode times. Startup requires Annex B SPS, PPS and an IDR. Continuous later segments can start mid-GOP and use the already initialized decoder. The existing AAC decoder and resampler supply 44.1 kHz stereo S16 PCM. [FFmpeg 8.1.2 demux source](https://github.com/FFmpeg/FFmpeg/blob/n8.1.2/libavformat/demux.c#L1351-L1369) · [FFmpeg timestamp fill-in](https://github.com/FFmpeg/FFmpeg/blob/n6.1.1/libavformat/demux.c#L870-L906)

The HLS master selector prefers a compatible muxed rendition whose advertised bitrate fits the 8 MiB fetch budget for a maximum-length segment. This selects Mux's 720p rendition instead of the 1080p entry, whose second segment exceeds the cap. Mux's first AAC packet is accepted by the decoder and emits zero PCM. The receiver advances the sample clock for that packet, then feeds subsequent PCM with the correct timestamps. The diagnostic logs give allowlisted stage and reason names plus container, codec and packet statistics. Live playlists, fragmented MP4, external audio groups, encrypted media, and declared or detected timestamp discontinuities are rejected. [Mux master](https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8)

Curl 42 is `CURLE_ABORTED_BY_CALLBACK`. The B7 trace shows the play socket closing before curl returns 42. That queues a stop, which cancels the transfer through the worker's curl progress callback. The cancelled attempt previously ended without a load outcome. The summary now balances attempts against loaded, failed and cancelled outcomes. The full pipeline tests replay the first two clear Apple and Mux segments over HTTP through the real downloader and simulated Starfish feed, plus malformed timestamps, discontinuities and cancellation. [libcurl return codes](https://curl.se/libcurl/c/libcurl-errors.html)

## Phase 3.4 startup ordering

The a078c1d B7 trace shows a successful Load, resource-info, and no LoadCompleted, Play request, or Feed. The blocking expression was `media_.ready && media_.playing` in `DirectBufferstream::tick()`. `Media::tick()` only requests Play after LoadCompleted. A working mirroring trace on this B7 records a first accepted preroll AU with `ready=0 playing=0`, followed by LoadCompleted, ACB loaded state, Play, and Playing. That order is also permitted by the v1.3 source. [Previous Direct Video feeder](https://github.com/depius/uxair-webos/blob/a078c1d55763e36108f656c62c0f40b85b33b840/src/direct_bufferstream.cpp#L230-L256) · [v1.3 mirroring](https://github.com/depius/uxair-webos/blob/d453a49cee41b0ce5e0f2d3654c3e658da776c73/src/main.cpp)

```text
a078c1d   Load accepted -> wait for LoadCompleted and Playing -> Feed
                           LoadCompleted -> ACB loaded -> Play -> Playing
                           B7 reference accepts Feed before LoadCompleted

Dev       Load accepted -> 200 ms -> bounded H.264/PCM Feed
           LoadCompleted -> ACB loaded -> Play -> Playing
           accepted A/V + Playing + frame callback -> decoder-progress
```

| Stage | Working v1.3 mirroring | Direct Video in a078c1d | Phase 3.4 |
| --- | --- | --- | --- |
| ACB and initialization | MSE role, media ID binding and display window | Same shared Starfish adapter | Log ACB results and retain these operations |
| Load and caps | BUFFERSTREAM with codec, geometry and PCM declarations | Same schema, fixed 60 FPS | Use the existing 30/60 fields based on initial video DTS spacing |
| Feed readiness | H.264 feeds after successful Load and a 200 ms delay. HEVC has bounded preroll | Loading and Playing callbacks required before the first feed | Start bounded preroll after Load acceptance and 200 ms, independent of those callbacks |
| Play and rendering | LoadCompleted drives ACB loaded and Play. Frame callback is measured separately | Shared Play logic was blocked on LoadCompleted | Retain callback-driven Play, and distinguish Play acceptance, Playing and actual frame callback |
| Data and timing | Incoming Annex B and PCM in a scheduled queue | HLS worker supplies Annex B access units and decoded PCM | Keep per-track decode order, pass nanosecond PTS relative to a common origin and let either track advance under backpressure |
| Session ownership | Receiver ingress and UI media owner | Separate HLS worker and UI media owner | Cancel and join the download worker, retire the callback generation and unload on stop or seek |

Preroll is limited to two seconds of media, 60 video AUs, 100 PCM buffers and 8 MiB of accepted data, with at most 24 Feed calls per UI tick. Feed retries retain the AU and PTS. The startup deadline remains 20 seconds. Failures identify missing initial feeds, LoadCompleted, Play acceptance, Playing, or the first decoder frame.

The GStreamer caps assertions also appear in the successful B7 mirroring trace before its LoadCompleted and decoder callbacks. The app supplies the same v1.3 PCM declaration, 44.1 kHz stereo interleaved S16LE, and resamples BipBop's 22.05 kHz AAC with FFmpeg before scheduling it. PCM duration uses the resampled sample count. GStreamer is reached via the system media stack. The available log and SDK declarations do not identify the internal caps consumer or the null caps source. The package retains the v1.3 schema. Repeated FFmpeg timestamp warnings are counted per segment on the HLS worker while unrelated errors retain normal logging.

The reverse channel requires one bodyless HTTP/1.1 200 per encrypted event, accepts an ACK across multiple records, and checks a supplied session header against the authenticated owner. The original B7 label `reverse-ack-framing` lacked the HTTP rejection reason. Dev now reports a bounded reason without showing the response. The failing Mux log likewise omitted the tag. The public Mux master and full selected playlist are exercised, optional metadata and unknown tags can be skipped, and known directives that require another decoding or download mode still fail with an allowlisted tag name. [HLS client rules](https://www.rfc-editor.org/rfc/rfc8216.html#section-6.3.1)
