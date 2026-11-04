# Milestone 9: experimental 60 FPS and version 1.0 preparation

> Historical development record. See [UxAir 1.2.0 scope](UXAIR_1_2_SCOPE.md) for current release and validation status.

## Frame-rate selection

Resolution, codec capability and maximum frame rate are separate saved settings. The receiver now accepts 30 or 60 FPS and advertises the selected maximum through AirPlay `refreshRate` and `maxFPS`. The Starfish Load request uses the same selected value for `videoFpsValue` and `adaptiveStreaming.maxFrameRate`. The settings file now persists `fps=30` or `fps=60`; the previous writer's hard-coded `fps=30` bug is fixed.

The UI offers 30 FPS (supported) and 60 FPS (experimental), and presents resolution without embedding a stale frame-rate label. Changing frame rate restarts receiver discovery only while idle, preserves resolution/codec/pairing, and saves atomically. On decoder failure at 60 FPS, the next advertised profile is retried at 30 FPS. HEVC decoder failure still selects H.264 720p/30. An old `hdr=` settings key is ignored, and new settings saves omit HDR preferences; Main10/HDR remains unsupported and excluded from the version 1.0 scope.

## Timing, diagnostics and buffering

Input timestamps remain the source of presentation cadence. The loop schedules video by the remote PTS converted to nanoseconds and continues to feed audio on its existing PCM timestamp timeline; no frame duplication or fixed 30 FPS pacing was added. The 200 ms pre-roll, decoder-generation deadline, video packet ceilings (1/2 MiB), and queue budgets (4/8/16 MiB by resolution) remain unchanged because no 60 FPS hardware evidence justifies larger buffers. At 60 FPS, monitor queue bytes, packet/frame drops, Feed retries/rejections, Feed time and audio continuity; the existing bounded queue remains a fail-safe rather than a promise of 60 FPS throughput.

Five-second `[video]` log samples distinguish advertised maximum, incoming FPS, PTS FPS, accepted Starfish Feed FPS and `FRAMEREADY` callbacks/s. The callback rate is explicitly not displayed FPS. The Diagnostics page and overlay expose advertised, incoming and Feed rates; display rate remains unavailable. If the sender supplies only 30 FPS, the incoming and Feed measurements will show that even when 60 is advertised.

## Supported evidence and release status

The owner-confirmed B7 modes remain H.264 720p/30, H.264 1080p/30 and HEVC 4K/30 SDR, including audio, pairing and orientation changes. H.264 720p/60, H.264 1080p/60 and HEVC 4K/60 are newly enabled experimental advertised modes. CI validates settings, advertisement, timing diagnostics, ARMv7 ABI and IPK packaging; it does not verify sender selection, decoder throughput, audio/video sync or visible 60 FPS on the B7. No 60 FPS mode is represented as hardware-verified.

The package remains a development build at the existing `0.1.0` app version. It is not a version 1.0 release candidate for publication until the three 60 FPS hardware checks and existing 30 FPS regressions pass on the target B7. HDR10/Main10 is intentionally excluded.
