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
3. Reopen the development app from TV Home to reset the trace counters. Start with the receiver idle. In Safari, use the AirPlay control on an unprotected H.264/AAC HLS video and select **UxAir Dev**. The normal launcher selects BUFFERSTREAM. Check for encrypted `/reverse` 101 and `/play` 200, then `contract=BUFFERSTREAM`, a successful Starfish Load, LoadCompleted, and `decoder-progress`. On the TV, check moving picture, audible synchronized audio, pause, seek and stop. Try H.264/HEVC mirroring and AirPlay audio after stopping. BUFFERSTREAM supports muxed MPEG-TS VOD and seeks to the preceding independently decodable segment. [Public 720p H.264/AAC test playlist](https://test-streams.mux.dev/x36xhzz/url_0/193039199_mp4_h264_aac_hd_7.m3u8)
4. If the startup Diagnostics page appears, report its effective UID and filesystem error. If the UI appears and playback fails, wait about 30 seconds after selecting AirPlay. Disconnect AirPlay on the iPhone and exit UxAir with Back to record the final listener totals. Collect the single filtered log below before relaunching or trying mirroring. Include whether the TV displayed moving video or played audio, and the iOS version. Keep the lines for the same conn and sid together so verification, server-info, fp-setup, property and seek responses, and the first play request can be read in seq order. Include the listener-stop totals.

   ```powershell
   $tvIp = '192.168.1.27'
   ssh "root@$tvIp" 'id=com.github.depius.uxair.dev; for base in "/var/palm/jail/$id" ""; do for r in /media/developer/apps/usr/palm/applications /media/cryptofs/apps/usr/palm/applications /media/developer/apps/var/lib; do p=$base$r/$id/state/uxair.log; if [ -f "$p" ]; then grep -E "^\[(startup|airplay-wire|airplay-wire-summary|video-crypto|video-control|reverse-channel|direct-player|direct-bufferstream|direct-protocol|hls-relay|media-init|media-state)\]" "$p" | tail -230; fi; done; done'
   ```

The development build defaults to Direct Video on and BUFFERSTREAM. Explicit `UXAIR_DIRECT_BACKEND=native` runs the URI experiment, and explicit `UXAIR_DIRECT_BACKEND=auto` attempts it before BUFFERSTREAM. The isolated six-case probe is available through the scripts in tools. Use the TV launcher for the retest so the app runs with its native credentials.

## Phase 3 load failure

The B7 reported success through encrypted `/reverse`, encrypted `/play`, relay startup, ACB initialization and Starfish construction. The last application line was `stage=load begin`. The failing call was `StarfishMediaAPIs::Load` with the experimental `args-hls` envelope, which supplies a URI and sets `args[0].mediaTransportType` to `HLS`. The firmware reported a Boost property-tree lookup failure for `plugins`. The exception names the lookup key. Identifying the tree it came from or its expected value requires the B7 implementation or a backtrace with symbols.

The shipped v1.3.0 Starfish backend uses the same private C++ entry point with ACB role MSE and `args[0].mediaTransportType=BUFFERSTREAM`. Its typed `option.externalStreamingInfo` configures H.264 and PCM packet input and has no `plugins` field. LG's Chromium Starfish source uses the same feed contract. LG's TV web-media guide covers playback of HLS URLs through an HTML media element. The separate webOS OSE `com.webos.media/load` service has a `uri`, `type` and `payload` envelope, with no documented `plugins` request field. These sources establish distinct entry points. The availability of the URI service to a jailed native B7 app and the private Starfish URI contract remain unverified. [v1.3.0 native backend](https://github.com/depius/uxair-webos/blob/d453a49cee41b0ce5e0f2d3654c3e658da776c73/src/media_starfish.cpp) · [LG Chromium Starfish implementation](https://github.com/webosose/chromium84-temp/blob/df04aa361ad493d47003a80ebfa3034dfc9de6e0/src/media/neva/webos/media_platform_api_webos_smp.cc) · [LG TV media options](https://webostv.developer.lge.com/develop/guides/mediaoption-parameter) · [webOS OSE media service](https://www.webosose.org/docs/reference/ls2-api/com-webos-media/)

Dev now fetches and demuxes supported HLS in UxAir, then calls Starfish BUFFERSTREAM with H.264 and PCM. Both development load paths report the API, ACB role, source and allowlisted JSON field types. Synchronous recoverable Load exceptions retire the callback and trigger teardown. A failed Unload prevents decoder reuse. Device-side buffering and picture and audio still require the B7 retest.
