# Milestone 6.2: HEVC decoder-generation startup deadline

> Historical development record. See [UxAir 1.2.0 scope](UXAIR_1_2_SCOPE.md) for current release and validation status.

Target: the separate UxPlay webOS **development** app on the LG OLED B7, Rockhopper 3.9.0. The owner has confirmed visible HEVC AirPlay mirroring and working Photos and video *through screen mirroring* with the Milestone 6.1 package. Sustained incoming and Feed rates were about 29–30 FPS, with `LOADCOMPLETED` (`0x16`) and `PLAYING` (`0x1a`) observed. Native 3840×2160 output-plane resolution remains unmeasured.

## Confirmed failure mechanism

On an SPS/parameter-set change, the receiver reopened Starfish successfully, reset its HEVC Feed preroll budget, and retained the validated random-access access unit. It then set a new 200 ms PTS preroll and returned to the event loop with that unit still pending. The startup watchdog ran at the **top of the next loop**, before the pending unit could be fed. It measured against the original AirPlay `session_started` time, already 23.72 seconds old, instead of the new decoder Load. It therefore immediately selected `startup-timeout` with `ready=0`, `playing=0` and `preroll=0/0`. That log is evidence of premature application fallback; it does not show that the new decoder rejected Feed or failed to initialize.

The sender's reported geometry is distinct from the parsed HEVC SPS geometry. Only a real parsed VPS/SPS/PPS byte change or SPS width/height change sets the reinitialization requirement. Repeated identical parameter sets do not reopen the decoder. A new configuration waits for a random-access frame; non-keyframes are dropped until then. If the changed parameter sets arrive before the random-access frame, cached VPS/SPS/PPS are restored to that frame before feeding the new decoder. A bytewise-different parameter set may alter decoder-relevant fields beyond width and height, so it still causes a conservative reopen. The code does not infer effective equivalence from dimensions alone.

## Correction and bounds

The HEVC startup gate now records a monotonic timestamp for **each successful Starfish Load**. The unchanged eight-second deadline is measured from that decoder generation. Each Load resets its accepted-frame count and 60-frame/8 MiB Feed-before-`LOADCOMPLETED` preroll budget. The reinit path also clears a prior Feed retry timer and per-generation diagnostics. The validated keyframe remains pending through the new 200 ms PTS preroll, then starts the new decoder at PTS zero; the AirPlay session start and usage duration remain continuous. The log now distinguishes session Feed count from generation Feed count and reports both generation and session age on timeout.

The working Starfish/ACB Load, callback, Feed, Play and teardown order has not been redesigned. ACB is set to UNLOADED, Starfish `Unload()` is called, then the old Starfish and ACB objects are released before the new Load. Firmware callback quiescence after `Unload()` is not independently documented; this change does not infer it from a successful return. If a future B7 run shows stale callbacks, collect the transition log before changing this proven lifecycle.

HEVC profile and bit depth are now parsed from SPS. Main 8-bit SDR remains the only accepted playback profile. Main10 SPS is identified and rejected before Starfish Load; the decoder API contract and metadata propagation required for a safe HDR10 path have not been established, so HDR remains disabled.

## Tests and B7 retest

`tools/test.sh` runs sanitizer-backed tests for the exact late-reopen case: 605 prior accepted frames, a successful Load at 23.72 seconds of session age, retained first preroll Feed, delayed `LOADCOMPLETED`, eventual Play, and a fresh eight-second timeout only when the new decoder genuinely stalls. HEVC parser tests cover 996×2160 portrait to 3840×2160 UHD parameter sets, restoration of cached parameter sets, Main10 detection/rejection, repeated identical sets, non-keyframes and malformed input. Existing H.264, audio and UI tests remain in the suite. The ARMv7 workflow must pass ELF/IPK validation before its artifact is used.

Keep the Milestone 6.1 development IPK for rollback. Manually upgrade the development app **without uninstalling** so pairing and settings persist; leave the production app untouched. Confirm H.264 720p/1080p and stereo audio first, then select experimental HEVC 4K/30, reconnect the iPhone, and mirror an SDR pattern. Rotate portrait ↔ landscape and switch between photo and video content. Inspect `[codec] reinitialize`, `[codec-start] load-accepted`, `reinit-keyframe retained`, `first-video-feed`, `[media-state] load-completed` and `playing`, plus `[video]` and `[pipeline]` rates and drops. A reinit should allow approximately 200 ms of new PTS preroll, accept Feed and reach `0x16`/`0x1a` without an immediate fallback. The generation-age timeout remains available for genuine failures. Avoid sharing logs with pairing or device identifiers.

If HEVC still fails, use the automatic H.264 720p recovery for a new sender connection. Reinstall the saved Milestone 6.1 development IPK over the app if needed; do not uninstall it or change firmware files. This package is not installed automatically. Visible playback does not by itself establish native 4K output-plane resolution.
