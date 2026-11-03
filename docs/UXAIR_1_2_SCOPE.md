# UxAir 1.2.0: final development scope and integration plan

Status set by the owner on 2026-11-03. Application ID and version remain `com.github.depius.uxair`, `1.2.0`. No new playback functionality or runtime fix is part of this scope-finalization change.

| Target | Maturity | Acceptance for this scope |
| --- | --- | --- |
| Starfish + ACB, webOS 3-4 | **Stable release candidate** | Automated suite/build/package validation plus final physical validation of the hardened 1.2.0 build on the B7. The successful 30-minute 148bc961 test is the preserved baseline, not acceptance of a changed binary. |
| NDL DirectMedia, webOS 5+ | **Beta / Experimental** | Automated tests, real-SDK compile/link and package validation. **No physical hardware validation of the current 1.2.0 source has been performed.** The owner has no compatible TV; unavailable hardware testing does not block development-scope completion or Starfish integration/release by itself. |

Historical contributor C4/webOS 10.3.1 results belong to the earlier 8526f8d source and are not presented as validation of this build. NDL must retain its Beta/Experimental label and known limitations in any later approved release description. Beta is not a waiver of a confirmed critical defect or the global security hold.

## Final NDL source review

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

## Automated acceptance and limits

`tests/ndl_media_test.cpp` compiles the actual NDL C++ media and C bridge with host-only SDL/vendor stubs under ASan/UBSan. It exercises initialization failures, audio-only/H.264/HEVC setup, PCM mapping, timestamps, audio buffer/retry handling, the video retry bound, retained binding across reopens and cleanup. Explicit characterization checks reproduce the retired-callback, ignored binding/placement and cleanup-error limitations. **Passing characterization checks means those limitations were observed, not fixed.** Stubs do not validate the firmware ABI, decoder, compositor, callback threading, jail or actual playback; the separate ARM matrix compiles/links with real SDK headers/libraries.

Run the complete `tools/test.sh`, metadata validation and cross-UID storage tests, then both ARM matrix targets. Keep package contents, metadata/version, permissions, private-state exclusion, ARMv7 soft-float, backend libraries, GLIBC <=2.19 and checksums checked. Both matrix jobs check out the same GitHub source SHA. No artifact uploads occur under the security hold.

Use explicit backend options: the existing unqualified build default remains NDL. A stable-candidate build must specify `--media=starfish`. No default or runtime backend behavior was changed.

Explicit build commands from one exact source revision:

```sh
tools/build.sh --media=starfish
python3 tools/validate-ipk.py
tools/build.sh --media=ndl
python3 tools/validate-ipk.py
```

Expected separate packages are `com.github.depius.uxair_1.2.0_arm-webos3-4.ipk` and `com.github.depius.uxair_1.2.0_arm-webos5plus.ipk` (Beta/Experimental). They share an app ID and are alternatives, not co-installable independent apps. There is no universal IPK or runtime backend switching. Setup, ELF checks and source packaging remain in [DEVELOPMENT.md](DEVELOPMENT.md).

## Integration and branch closure (plan only)

1. Freeze this development scope and record the exact successful test/build SHA and known limitations. No NDL hardware requirement is added to this scope; future hardware results may improve Beta status.
2. Preserve the 148bc961 B7 rollback baseline. Obtain approval before distributing the final hardened Starfish test candidate; complete B7 playback, fresh/upgrade, pairing/settings and reboot checks on its exact source.
3. Keep the global security release hold: obtain the original browser verdict's independent assessment or explicit owner-reviewed security disposition, including remaining parser, installation-container and host-tool risks. CI success and a Beta label do not resolve those findings.
4. After explicit owner approval for integration, integrate this foundation once into main, preserving contributor attribution and the complete shared-fix chain. PR #10's head is already an ancestor; do not apply it a second time. Keep PR #9 Direct Video/HLS outside 1.2.0. Approval must cover any resulting PR-state effects of integrating existing contributor commits; inspect those effects before performing an approved merge.
5. Run the full automated matrix on the actual resulting main SHA. Confirm both outputs use that same revision and retain Starfish stable-candidate / NDL Beta labels. NDL hardware testing remains deferred, not a closure prerequisite.
6. Publication, PR closure and branch/worktree deletion are separate owner-approved actions. Record checksums, corresponding sources, support labels, security disposition and rollback reference before any approved release. Do not delete this branch/worktree until the owner explicitly approves cleanup; no such action is performed here.

## Outstanding security

See [SECURITY_HARDENING.md](SECURITY_HARDENING.md) and [PACKAGING_DEPENDENCY_AUDIT.md](PACKAGING_DEPENDENCY_AUDIT.md). Writable platform-managed ancestors, remaining upstream malformed-media parsing, partial loader hardening and incomplete build attestation remain documented. The host CLI audit reports 18 affected package entries (including a conditional server-oriented critical advisory); package-only CI does not establish their exploitability or repair them.

The original dc3fe475 ZIP has SHA-256 `cec8ec73fb66bd4c0f1abfd59e82fd0d1111a4aa7b02d6e5e9d6b3e8767ebb70`. Firefox's Malware verdict remains unresolved, Defender scans were clean, and no verified vendor analyst verdict/case reference has been supplied. Neither confirmed malware nor confirmed false positive is established. Preserve the original sample and browser protections. No release or artifact distribution is authorized by this scope decision.

## Completed automated acceptance

Test/build revision: `03242d26ec6a5aba7b2c84f93f8ec75a131339d7`.

- [Full sanitizer-backed Validate workflow](https://github.com/depius/uxair-webos/actions/runs/41668703529): success, including cross-UID storage tests and the new actual-source NDL mock tests.
- [Independent native tests and both ARMv7 package jobs](https://github.com/depius/uxair-webos/actions/runs/41668703360): success. Both backend logs record the same source SHA, valid app ID/version, package permission/content checks and max required GLIBC 2.7 <=2.19.
- Starfish linked libplayerAPIs.so.1/libAcbAPI.so.1; NDL linked libNDL_directmedia.so.1. These are distinct compile/link/package targets from the same revision.
- Mock tests observed all documented retired-callback, unchecked binding/placement and ignored cleanup-error limitations without ASan/UBSan failures. This does not resolve those limitations or validate firmware playback/threading.
- GitHub artifact listing is empty. No new artifact distribution, TV installation, release, merge or branch/PR closure occurred.

The final result-recording follow-up is documentation-only; production source, app identity/version and build/test logic remain identical to the tested revision. Development scope is complete with Starfish stable-candidate / NDL Beta maturity. Security disposition, hardened B7 physical acceptance and explicit owner approval still govern integration/publication/cleanup; unavailable NDL hardware testing does not.
