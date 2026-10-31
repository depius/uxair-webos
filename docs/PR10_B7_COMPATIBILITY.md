# PR #10 B7 compatibility fixes — local validation report

> Follow-up 2026-10-31: `72a276c` fails storage validation on the B7. See [the storage diagnostic build](B7_STORAGE_DIAGNOSTICS.md). The report below is historical, not a claim of current TV compatibility.

Date: 2026-10-30. Status: implemented locally; **not ready for release or merge until Linux compilation and TV validation pass**.

This report records the original local validation before the follow-up Actions build. The branch now includes a Starfish-only ARMv7 workflow trigger and stricter archive/control/dependency validation; consult the Actions run for its final result. The hardware-validation requirements below still apply.

## Isolation

- Branch: `fix/pr10-b7-compatibility`.
- Exact PR #10 base and unchanged HEAD: `8526f8d5547709eec43c7993e1243f20f93ae2a3` from ALQU-IT's `webos10-ndl` branch.
- Worktree: `C:\Users\matej\.codex\worktrees\fix-pr10-b7-compatibility\uxplay-webos`.
- Branch, commit, working directory and clean starting status were verified and reported before editing.
- Changes are uncommitted. No push, PR comment, workflow approval or merge was performed.
- The original checkout remains on `codex/uxair-1.0.0-rc5-starfish-regression` at `333478d11a2bd294049102c339b384a1f2c289cc`. Its pre-existing edits were not copied or modified.
- `main` remains at `9be9657246bd68feb1b7469ba062f548ee76f2d3`; the PR #9 local branch remains at `420673c4b487b387f0ae262f6e6d490963251f04`. The contributor's branch was not changed.

## Root causes and changes

**Application exit — verified source defect.** PR #10 treated remote Exit, SDL_QUIT and the signal-handler result as the same background action. Since the signal flag stays set, termination could repeatedly request background transitions instead of reaching cleanup. `src/input.h` and `src/input.cpp` now distinguish unconditional `Quit` from remote `Exit`. `src/shell.cpp:298` prioritizes termination; `src/main.cpp:464` exits the main loop before background handling. Existing receiver/media cleanup remains at the end of main.

Starfish background operation defaults OFF (`src/main.cpp:129`). Idle Back closes the application after normal dialog/page navigation; Exit closes it directly. Back during playback, pairing and dialogs keeps the existing navigation/cancellation behavior. NDL keeps its background default. `UXAIR_BACKGROUND=1` explicitly enables background behavior; `0` disables it. Bus startup failure disables background behavior instead of consuming Exit without a working background service.

**Now Playing bands — hypothesis, not a verified root cause.** The old renderer stretched a 1×256 gradient over the output, modulated its brightness and slowly moved the layout. The supplied timing also overlaps brightness transitions, but there is no TV trace establishing which graphics operation causes the bands. `src/shell.cpp:181` now clears an opaque, solid album-derived background and removes the gradient texture. Whole-layout drift is removed. Artwork, title, artist, album, progress, title scrolling and OLED dimming remain. There is one presentation per composed frame; texture operations remain on the main thread. Missing Now Playing fonts fall back to the existing UI fonts, and texture/blend failures are logged with bounded frequency.

**Unexpected return to the main UI — unresolved on hardware.** The renderer itself does not end a media session. Relevant application paths include transport reset, 20-second inactivity timeout, decoder/feed failure, remote input and Luna foreground changes. Session-reset/finish and remote-settings diagnostics were added (`src/main.cpp:192`, `:493`, `:565`) to distinguish these on the next reproduction. A delayed startup-background request now cancels on connection, PIN or remote activity. No claim is made that the solid background fixes session termination.

**Artwork copying — verified source defect.** `Receiver::now_playing()` copied a vector containing encoded artwork on every snapshot, including iterations before the drawing throttle. `src/now_playing.h` now stores immutable shared ownership. `src/receiver.cpp:228` allocates replacement bytes in the callback outside the metadata lock; snapshots retain those bytes safely across replacement/disconnect. SDL textures remain cached by artwork generation. Per-session progress/feed tracking is reset in `src/main.cpp:226`.

**Network metadata bounds.** `src/now_playing.h:17` caps DMAP input at 64 KiB, each recognized text field at 1,024 bytes, nesting at four containers and item count at 256. Invalid/truncated input does not partially replace metadata. Control characters are normalized. PNG/JPEG artwork is limited to 2 MiB encoded, a 2,048-pixel maximum side and 1,048,576 total pixels. Header dimensions are checked before SDL_image decoding, and decoded dimensions are checked again before conversion/texture creation. Oversized/unsupported artwork falls back to the no-art view. This deliberately rejects some previously accepted large artwork; text/audio continue.

**Persistent data.** `src/secure_data.h:12` checks directory/file ownership and rejects symlink leaves, nonregular state files and multiply linked files. State is private (0700 directory, 0600 files); log files are validated before truncation. The predictable `/tmp` identity fallback is removed. `src/shell.cpp:55` prefers existing app-local Starfish state; existing PR #10 var/lib state is retained when app-local state is absent. If both exist, app-local state wins, with no merging or deletion. NDL continues using var/lib. Unsafe/inaccessible state fails startup instead of silently creating a new identity. These checks assume trusted platform-owned ancestor directories; they do not isolate mutually hostile processes sharing the same UID.

**Luna lifecycle and screen saver.** `src/system_bus.cpp:96` cancels outstanding foreground lookups and ignores replies after a return/background request. Tasks are explicitly queued on the bus context and have destruction callbacks. `src/system_bus.cpp:159` queues unregister/quit on that context: calling quit before the thread enters `g_main_loop_run()` could otherwise be lost, hanging join during rapid exit. Paused/flush-paused playback, settings and shutdown no longer inhibit the screen saver (`src/main.cpp:466`). These Luna changes need real-service validation, including Home/background transitions; the code does not subscribe to every external foreground change.

**Build/install tooling.** `tools/tv.sh:19` selects `arm-webos3-4.ipk` or `arm-webos5plus.ipk` from the build marker. `tools/package.py:15` rejects a missing/unknown backend before staging instead of silently labelling it NDL. `tools/bootstrap.py:23` rejects unsupported hosts before downloading; previously Windows fell through to the macOS SDK. Both local workflow definitions add the SDL2_image runtime needed by the new host artwork-decoding test. No workflow was run or approved remotely.

Byte-for-byte comparison against the PR head confirms no changes to `src/media_starfish.cpp`, `src/media_ndl.cpp`, `src/ndl_backend.c`, `src/settings.cpp`, `src/usage_stats.cpp` or `src/pairing_registration.h`. This is source preservation, not proof of H.264/HEVC/4K60/RAOP compatibility after shared lifecycle changes.

## Automated validation actually performed

All commands ran from the isolated worktree.

- **Passed:** seven portable C++ executables using MSVC 14.44, `/fsanitize=address`, assertions enabled: `diagnostics_test`, `audio_session_test`, `media_open_failure_test`, `hevc_startup_test`, `ui_navigation_test`, `pairing_ui_test`, `now_playing_test`.
- The new metadata test covers bounded nesting/lengths/items, truncation, PNG/JPEG dimension limits, empty/malformed input, 10,000 deterministic malformed buffers, and 10,000 concurrent immutable-snapshot replacement/removal cycles. No ASan error was reported. This does not replace ThreadSanitizer or leak measurement on the TV.
- **Passed:** existing Python scripts for donation icon/QR, History layout, UxPlay instrumentation, media failure/identity and pairing rendering/privacy (eight scripts).
- **Passed:** three new `build_platform_test.py` cases: supported host selection, fail-before-staging when the backend marker is absent, and mocked installer routing for Starfish/NDL/rejection of unknown backends. The install test contacts no device and produces no IPK.
- **Passed:** `tools/validate-metadata.py`, Bash syntax checks for build/test/install scripts, and `git diff --check`.
- **Blocked:** the complete `tools/test.sh` stops at `c++: command not found` under Git Bash (exit 127). The native POSIX/GCC/SDL/FFmpeg environment is unavailable. `direct_info_gate_test.py` could not find `cc`; `b7_media_observe_test.py` could not find `sh` on the Python process PATH.
- **Added but not executed here:** POSIX state permission/symlink/hardlink/FIFO tests; SDL rendering with real PNG decoding, texture reuse, dimmed solid background and repeated art removal; SIGINT/SIGTERM Shell tests; the updated SDL_QUIT mapping test. They are wired into `tools/test.sh`. Linux CI now installs SDL2_image for the decoding assertions.
- **Not performed:** full application compilation for either backend, Linux ASan/UBSan/LSan, ThreadSanitizer, Luna runtime tests, TV memory/RSS measurements or physical display/audio validation. Windows ASan does not establish leak freedom or undefined-behavior coverage.

## Production build result

The requested Starfish command was attempted through Git Bash. After directing `python3` to the installed Python interpreter, it failed with:

```
Unsupported build host: use Linux x86-64 (including WSL2) or macOS ARM64 with the pinned webOS ARM SDK.
```

This Windows host has no WSL installation. A supported Linux x86-64/WSL2 or macOS ARM64 build environment and the pinned webOS ARM cross-toolchain/dependencies are required. Visual C++ can run portable tests but cannot produce the webOS ELF binary. No system toolchain or WSL installation was attempted.

**No IPK was produced.** `dist/com.github.depius.uxair_1.1.0_arm-webos3-4.ipk` does not exist. ARMv7 ELF architecture, dynamic dependencies, IPK archive contents and SHA-256 therefore remain unverified; there is no checksum to report. Application metadata validation passed for `com.github.depius.uxair`, version `1.1.0`, but upgrade suitability is not established without the actual package and TV test.

## Required validation before release

1. On a supported host, run `tools/test.sh`, then `tools/build.sh --media=starfish` from this branch. Inspect `tools/verify-elf.py` results, IPK archive/control/appinfo contents and SHA-256. Also build NDL and run its smoke tests because the lifecycle/rendering code is shared.
2. Back up the B7's existing identity, paired keys, settings and usage history. Upgrade using the same app ID. Confirm all remain intact after restart and reboot; verify the selected data directory and ownership inside the actual jail.
3. From idle, test Back and Exit. During playback, dialogs and PIN prompts, test Back navigation/cancellation. Send SIGTERM and SIGINT and exercise SDL_QUIT. Confirm the process exits promptly, sockets/discovery disappear and relaunch/pairing works. Repeat rapid launch/termination to test the bus shutdown race.
4. Play RAOP for at least 30 minutes, repeatedly changing tracks with PNG/JPEG art, no art, long/non-ASCII titles and rejected oversized art. Observe at 8–10 seconds, 20–30 seconds and after dimming settles. Confirm there are no bands, artwork tears or audio gaps; track RSS across at least 20 stop/reconnect cycles.
5. If the UI returns unexpectedly, capture the bounded log around `[session]`, `[audio-only]`, `[input]`, `[ui]` and `[bus]`. Identify the actual reset/failure/input cause before changing playback timeouts or claiming the rendering hypothesis is confirmed. Do not include pairing secrets in shared logs.
6. Verify H.264 720p/1080p at 30/60 FPS, HEVC including 4K/60 input, photo/video mirroring, RAOP, reconnect and PIN cancellation. Check negotiated dimensions and audio continuity; input resolution is not evidence of native 4K display output.
7. Verify paused/idle/settings screen saver behavior and remote brightness recovery. With explicit `UXAIR_BACKGROUND=1`, test Home, return-to-previous-app, immediate reconnect and a connection during the first 2.5 seconds. Repeat on an NDL television. Background remains disabled by default on the B7.

The diagnostic rendering change, stricter storage checks and shared Luna lifecycle code all require these checks before merge. No remote approval, installation or release action was taken.
