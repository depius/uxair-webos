# UxAir 1.0.0 release candidate 5

RC5 is an instrumentation build for the reported Starfish `Load()` failure. The owner subsequently confirmed playback on the physical B7, including H.264 and HEVC mirroring; the final release-candidate verification is summarized in the README.

## Changes

- Logs the runtime `APPID`, compiled fallback ID, and the shared ID passed to ACB and Starfish. The JSON payload and stage timings remain available in `uxair.log`.
- Tracks ACB creation, ACB initialization, media-ID registration, invalid configuration, and Starfish `Load()` failures separately. A failed start releases Starfish/ACB resources and leaves the SDL loop available for Back, Exit, and settings changes.
- Shows a user-visible stage-specific playback error after recovery handling.
- Adds tests that compare the 1080p H.264/30 payload against the pre-refactor layout and guard app-ID consistency and failure cleanup paths.

## Investigation findings

- The production manifest ID, compiled production fallback, and runtime ID passed to both `AcbAPI_initialize()` and the Starfish payload are intended to be the same. The runtime log will expose an `APPID` mismatch if webOS supplies one.
- The mirroring payload builder refactor preserves the legacy H.264/30 JSON fields; dynamic codec, frame rate, dimensions, and buffering are expected profile differences. A test now protects the baseline H.264/30 payload.
- LG's app metadata reference establishes that `appinfo.json` IDs identify apps and must be unique; it does not establish that Starfish requires the old UxPlay ID. RC5 keeps the UxAir ID unchanged.
- No legacy-ID package is produced because that ID belongs to the retained UxPlay development installation; installing another package with that ID could replace the rollback app. RC5 remains a single UxAir-identity diagnostic build.

## Subsequent hardware verification

The owner confirmed on the B7 that RC5 playback works. The tested later release-candidate baseline also confirms standalone AirPlay music/audio-only playback, pairing and reconnection, H.264/HEVC mirroring, and audio with mirroring. The detailed feature and limitation matrix is in the [README](../README.md). CI confirms build/package validity but cannot substitute for these physical checks.
