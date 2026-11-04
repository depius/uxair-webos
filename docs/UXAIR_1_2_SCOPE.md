# UxAir 1.2.0: final development scope and integration plan

The 1.2.0 development scope is frozen. Both packages use `com.github.depius.uxair`.

| Target | Maturity | Evidence |
| --- | --- | --- |
| Starfish + ACB, webOS 3–4 | **Stable** | Owner accepted `9af36d1b2e3902bfa26ab349140af394b3b95a3f` on LG OLED B7 with webOS 3.9. See the [acceptance record](B7_FINAL_TEST.md). |
| NDL DirectMedia, webOS 5+ | **Public Beta / Experimental** | Automated tests and SDK build checks. Current-build physical hardware validation is outstanding. The contributor’s earlier C4 results apply to `8526f8d`. |

The historical review below records the behavior before the [NDL lifecycle fixes](NDL_LIFECYCLE_FIX.md).

## Historical NDL source review at d452b12

Reviewed actual `src/media_ndl.cpp`, `src/ndl_backend.c/h`, shared startup/persistence/pairing/audio/video paths, and the API-v2 declarations from the retained checksum-pinned SDK. No production source was changed for this review.

| Path | Confirmed behavior | Unresolved issue / disposition |
| --- | --- | --- |
| Startup and initialization | Configuration checked before Load; exported-window creation and DL/Init/Load failures report failure. NDL initialization/window binding is retained across session reopens and Quit is attempted at process cleanup. | `ndl_backend.c:50-53` logs absent/failed SetWindowId and continues; `media_ndl.cpp:128-130` logs failed SDL placement but still succeeds. Controlled tests reproduce these unchecked failures. A successful open is not evidence of visible video. Known Beta functional defect; no speculative fix applied. |
| Callback/session lifetime | Closing clears the current media pointer and bridge callback; no-session callbacks are ignored. | The callback has no generation token. Injecting a retired callback after reopen marks the new session failed. Whether firmware actually delivers such callbacks after Unload is unverified. The non-atomic bridge callback and a raw Media pointer also need a documented quiescence/thread contract before lifetime safety can be claimed; SDK declarations do not supply that guarantee. Potential race/lifetime risk, not a demonstrated firmware crash or exploit. |
| Cleanup | Unload is called once for a loaded stream; repeated close is idempotent in local state; window destroyed on Media destruction. | The SDK declares error returns for Unload and Quit; the bridge ignores them and clears its local flags. Tests characterize that behavior. Actual resource release after a vendor error/partial failed Load remains unknown. The resource-released callback only logs; resource-loss recovery is unverified. |
| Pairing and persistence | Uses shared receiver/key registration and private-file validation. Existing NDL var/lib state is reused. Legacy app-local identity without NDL storage fails closed rather than creating replacement keys. | Real jailed mounts, permissions, secure legacy migration and reboot retention are untested on the current NDL build. No automatic migration or key regeneration is introduced; preserve private backups. Shared sanitizer fixtures are not physical NDL pairing. |
| Audio | Shared decoding produces stereo PCM; bridge selects S16LE/stereo and the SDK's 44.1-kHz enum. Available-buffer checks and failed audio Feed return Retry; existing main-loop timeout remains finite. | NDL pause/eos are no-ops and there is no decoder-buffer FLUSH call. Shared FLUSH invalidates ingress/queued audio, but does not prove already-fed audio is discarded or instantly paused. A/V sync, audible output, volume and interruption timing remain unverified Beta behavior. |
| Video | H.264/HEVC type/geometry mapping, ns-to-us Feed timestamps, retained exported window, arrival-based pacing and bounded retries are present. | Load sets ready before a callback to allow feeding; PLAYING/visible frames still depend on firmware. NDL frames counts accepted feeds, not displayed frames. Late callbacks, actual latency and sustained reinitialization are unverified. |

No demonstrated Critical-severity NDL crash, memory-corruption exploit or pairing-data destruction was established by this review. This is not a clean bill of health: the callback-lifetime contract is an unresolved potential memory-safety concern, and the functional/cleanup defects above are confirmed unchecked paths. NDL remains Beta/Experimental; do not claim production-grade safety or playback support. A future concrete confirmed critical issue must be fixed or the affected Beta asset withheld, rather than hidden behind the label.

## Historical automated acceptance and limits

`tests/ndl_media_test.cpp` compiles the actual NDL C++ media and C bridge with host-only SDL/vendor stubs under ASan/UBSan. It exercises initialization failures, audio-only/H.264/HEVC setup, PCM mapping, timestamps, audio buffer/retry handling, the video retry bound, retained binding across reopens and cleanup. Explicit characterization checks reproduce the retired-callback, ignored binding/placement and cleanup-error limitations. **Passing characterization checks means those limitations were observed, not fixed.** Stubs do not validate the firmware ABI, decoder, compositor, callback threading, jail or actual playback; the separate ARM matrix compiles/links with real SDK headers/libraries.

Run the complete `tools/test.sh`, metadata validation and cross-UID storage tests, then both ARM matrix targets. Keep package contents, metadata/version, permissions, private-state exclusion, ARMv7 soft-float, backend libraries, GLIBC <=2.19 and checksums checked. Both matrix jobs check out the same GitHub source SHA. Ordinary validation runs retain checks and logs.

Use explicit backend options: the existing unqualified build default remains NDL. A stable-candidate build must specify `--media=starfish`. No default or runtime backend behavior was changed.

Explicit build commands from one exact source revision:

```sh
tools/build.sh --media=starfish
python3 tools/validate-ipk.py
tools/build.sh --media=ndl
python3 tools/validate-ipk.py
```

Expected separate packages are `com.github.depius.uxair_1.2.0_arm-webos3-4.ipk` and `com.github.depius.uxair_1.2.0_arm-webos5plus.ipk` (Beta/Experimental). They share an app ID and are alternatives, not co-installable independent apps. There is no universal IPK or runtime backend switching. Setup, ELF checks and source packaging remain in [DEVELOPMENT.md](DEVELOPMENT.md).

## Integration and publication

Follow the [release procedure](RELEASING.md). The original PR #10 head is an ancestor. Integrating the complete branch with a merge commit preserves contributor history and can mark PR #10 merged indirectly. Keep PR #9 outside this release. The security hold and owner approval govern integration and publication.

## Outstanding security

See [SECURITY_HARDENING.md](SECURITY_HARDENING.md) and [PACKAGING_DEPENDENCY_AUDIT.md](PACKAGING_DEPENDENCY_AUDIT.md). Writable platform-managed ancestors, remaining upstream malformed-media parsing, partial loader hardening and incomplete build attestation remain documented. The host CLI audit reports 18 affected package entries (including a conditional server-oriented critical advisory); package-only CI does not establish their exploitability or repair them.

The original dc3fe475 ZIP has SHA-256 `cec8ec73fb66bd4c0f1abfd59e82fd0d1111a4aa7b02d6e5e9d6b3e8767ebb70`. Firefox's Malware verdict remains unresolved, Defender scans were clean, and no verified vendor analyst verdict/case reference has been supplied. Neither confirmed malware nor confirmed false positive is established. Preserve the original sample and browser protections. The historical scope decision authorized no distribution; the later owner request explicitly authorizes temporary TEST candidates only, as described in RELEASING.md.

## Previous automated acceptance

Test/build revision: `03242d26ec6a5aba7b2c84f93f8ec75a131339d7`.

- [Full sanitizer-backed Validate workflow](https://github.com/depius/uxair-webos/actions/runs/41668703529): success, including cross-UID storage tests and the new actual-source NDL mock tests.
- [Independent native tests and both ARMv7 package jobs](https://github.com/depius/uxair-webos/actions/runs/41668703360): success. Both backend logs record the same source SHA, valid app ID/version, package permission/content checks and max required GLIBC 2.7 <=2.19.
- Starfish linked libplayerAPIs.so.1/libAcbAPI.so.1; NDL linked libNDL_directmedia.so.1. These are distinct compile/link/package targets from the same revision.
- Mock tests observed all documented retired-callback, unchecked binding/placement and ignored cleanup-error limitations without ASan/UBSan failures. This does not resolve those limitations or validate firmware playback/threading.
- GitHub artifact listing is empty. No new artifact distribution, TV installation, release, merge or branch/PR closure occurred.

The owner subsequently completed final Starfish testing. See the [B7 acceptance record](B7_FINAL_TEST.md) and the [latest pre-cleanup automated results](https://github.com/depius/uxair-webos/actions/runs/41676314394).
