# B7 foundation review for UxAir 1.2.0

**Status: unreleased foundation; security hold active; no merge/publication/distribution approval.** The next official version is 1.2.0. Version 1.1.0 was internal testing. This review does not include PR #9's Direct Video/HLS functionality.

## 1. Physical B7 evidence

Baseline `148bc9616f3edfff3d63ec6181f081322d1017fd` passed the owner's 30-minute test: repeated orientation and resolution changes, video, photos, mirroring and audio, with no observed errors. Earlier tests confirmed saved 4K/60 HEVC, pairing, settings, music/Now Playing, photos with background music, sender TEARDOWN classification and remote Exit. Displayed FPS/native 4K scanout and every individual negotiated audio codec were not independently measured.

The hardening/version commits are later source and need renewed acceptance before an authorized final candidate. NDL is **Beta / Experimental; no physical validation of the current 1.2.0 build has been performed**. The owner has no compatible TV; NDL hardware testing is deferred, not a development-scope or Starfish-release prerequisite. [UXAIR_1_2_SCOPE.md](UXAIR_1_2_SCOPE.md) is the final maturity/NDL review and closure policy. The contributor's C4/webOS 10.3.1 report at `8526f8d5547709eec43c7993e1243f20f93ae2a3` applies only to that earlier source.

## 2. Security disposition

[SECURITY_HARDENING.md](SECURITY_HARDENING.md) separates implemented changes and remaining risks. Production sender-controlled allocations are now bounded before allocation; allocation failure is handled; package-internal writable modes are reduced; CLI inputs are locked and lifecycle scripts disabled; source archives exclude installed host npm executables. The B7 container exception, HEVC parser/scheduling, RAOP/UI/media behavior and existing limits are retained.

The world-writable managed installation ancestor remains an incomplete local-attacker boundary. Malformed upstream media parsing still requires a dedicated review. Runner/OS inputs are not fully reproducible or independently attested. The locked host CLI graph also retains 18 audit-flagged package entries, including a conditional server-oriented critical advisory; [the separate assessment](PACKAGING_DEPENDENCY_AUDIT.md) records why package-only CI does not establish exploitability or a fix. The original dc3fe475 ZIP Firefox Malware verdict remains unresolved, with no verified vendor analyst outcome. No clean scan or these changes establishes a false positive. These remain release decision inputs.

NDL's legacy upgrade path now refuses startup if app-local state exists but var/lib state does not. It leaves old keys/settings intact and never silently initializes a new identity. Secure cross-backend migration is not implemented; approval and physical validation are required before attempting it.

## 3. Packaging and installation

The unchanged baseline TEST IPK SHA-256 is `649d29494ccb6a26fd68acf0cd261773c222a57ee3b1f8ebf9c5c7026b2ccbbd`. Its ARMv7 soft-float metadata, max GLIBC 2.7, dependencies and archive checks passed. On the physical B7, separate official CLI 3.2.6 upgrade/reinstall and clean-install scenarios each completed with status 30 (`installed`), exit 0. Original pairing/settings were verified/restored; startup reported paired=1 and the saved 4K/60 HEVC profile.

Glasshouse same-version reinstall success is owner-reported. Dev Manager 1.99.21 has an unbounded Homebrew configuration preflight; the B7 service did not reply during the bounded reproduction, and an older matching process remained. That is a concrete pre-upload blocker and strong evidence for Please wait. The original frontend trace is missing, so that historical event's exact attribution is provisional. The direct CLI path is physically verified. Uninstall deleted app-local state: perform upgrades without uninstalling and retain private backups.

The new version preserves app ID `com.github.depius.uxair`, state filenames, serializers and paths. The About version is read from generated build-info; the package pipeline derives versions from appinfo. New hardened 1.2.0 fresh installs and upgrades from 1.0.0/internal 1.1.0 still need approved physical tests. Independent diagnostic probes retain their own IDs/versions and are not release packages.

## 4. Shared versus backend-specific review

Comparison base: current remote main `ae205c2e6453de64c289e8ba0fc63a8b672b3068`, not the stale local main `9be9657`. Every changed path is assigned a primary class and backend scope in [BRANCH_CHANGE_CLASSIFICATION.csv](BRANCH_CHANGE_CLASSIFICATION.csv). Mixed files are identified explicitly.

| Class | Main changes and review outcome |
| --- | --- |
| Shared UxAir functionality | Receiver/queue/epoch/closing gates, RTP reset provenance, RAOP FLUSH/resume, split HEVC header retention, Now Playing cadence/rendering, settings/pairing validation, system bus, input/Exit distinction, allocation guards and build/package infrastructure. These must reach both targets together; individual commits depend on earlier interfaces. |
| Starfish-specific compatibility | media_starfish rename, B7 managed-container exception, background off by default, 200 ms first-video delay separated from the preserved timeline, bounded preroll with due audio progress, ACB/Starfish diagnostics. Preserve the complete validated sequence through 148bc961. |
| NDL-specific functionality | media_ndl.cpp and ndl_backend.c/h, exported-window/process-lifetime binding, arrival-anchored <=50 ms scheduling and NDL var/lib policy. The latest shared protections compile into this target; runtime hardware acceptance remains pending. |
| Diagnostic or test-only changes | B7 investigation records, sanitized bounded lifecycle/queue diagnostics, regression fixtures, independent probes and workflow validation. No Direct Video/HLS runtime feature is imported. History preview uploads and the temporary TEST workflow are absent from the final ARM workflow. |

Review findings: no unrelated tracked binaries, secrets or device backups were added. Keep bounded diagnostics needed to correlate generations/epochs and resets; no unvalidated diagnostic cleanup should disturb working media behavior. Historical investigation documents retain their original commit/version claims and are not current release status. The older README claim of an official 1.1.0 release and stale build/NDL claims are corrected in current release documentation.

Known cross-backend regression surfaces are NDL late callbacks during rapid reopens, audio-only Feed/FLUSH behavior, storage migration and background/foreground/Exit semantics. NDL readiness and frame counters differ from Starfish; successful Load/Feed or an emulated host test does not establish displayed frames. No speculative backend refactoring was performed.

## 5. Integration recommendation (not executed)

1. Keep the validated baseline commit and this isolated branch/history intact while security assessment and renewed physical tests are pending. Do not merge now.
2. After the owner approves the exact final commit, integrate this reviewed foundation once, retaining contributor attribution and the complete dependency chain. The original five PR #10 commits (`60e30c8` through `8526f8d`) are already ancestors of this branch. Do not merge/cherry-pick the NDL implementation a second time or apply only the final B7 scheduling commit over main.
3. Prefer a single reviewed foundation integration over selecting isolated runtime hunks. If selective integration is required, first take the original build/backend foundation, then all B7 storage/lifecycle/FLUSH/header/cadence/reset/ingress fixes in original order through 148bc961, followed by the minimal security and version commits and their tests. Re-run the entire matrix and the Starfish physical gate and NDL Beta disclosure on the resulting history. The temporary c894a17 TEST workflow and its removal need not be recreated in a selective sequence.
4. Once that foundation is approved/integrated, future NDL work should branch/rebase onto the new shared main. The current contributor NDL head itself requires no reapplication: it has no commits beyond the included 8526f8d in the inspected PR. Coordinate later changes on a separate branch; do not rewrite the contributor's current PR branch as part of this task.
5. Keep PR #9 separate. Its experimental Direct Video/Starfish work must later be rebased/selectively integrated over the accepted foundation with explicit backend gating and a separately chosen future release version. Its existing 1.2 development naming does not authorize including those features in this 1.2.0 foundation. Do not merge either existing PR automatically.

## 6. Two build procedures

Use the exact repository commit on Linux x86-64 or macOS ARM64 with pinned SDK/dependencies. Set up the locked CLI with `npm ci --prefix tools/webos-cli --ignore-scripts --no-audit --no-fund`, then add its node_modules/.bin to PATH. Full prerequisites: [DEVELOPMENT.md](DEVELOPMENT.md).

```sh
python3 tools/validate-metadata.py
tools/test.sh
# Starfish + ACB, webOS 3-4
tools/build.sh --media=starfish
python3 tools/verify-elf.py build/app
python3 tools/validate-ipk.py
# Separate NDL DirectMedia Beta / Experimental target; no physical validation
tools/build.sh --media=ndl
python3 tools/verify-elf.py build/app
python3 tools/validate-ipk.py
```

Expected files: com.github.depius.uxair_1.2.0_arm-webos3-4.ipk and com.github.depius.uxair_1.2.0_arm-webos5plus.ipk. Both use the same app ID and cannot be installed alongside one another as independent apps. Source packaging derives the same 1.2.0 version. The CI matrix builds/checks both and generates checksums with no artifact upload; no new download is offered by this report.

## 7. Remaining release gates

- Independent assessment of the unchanged browser-flagged ZIP, with a verifiable case reference and outcome; explicit acceptance of remaining security risks.
- Approval before distribution of one final Starfish physical-test candidate, then renewed B7 stress, pairing/settings, fresh install and 1.0.0/1.1.0 upgrade/reboot tests on that exact hardened 1.2.0 source.
- Deferred optional NDL Beta hardware follow-up (not a 1.2.0 closure gate): installed-app startup/window binding, H.264/HEVC portrait/4K and repeated reopens, audio-only and music/photo coexistence, FLUSH/resume, volume/mute, rapid handoffs, sender TEARDOWN/failures/late callbacks, foreground/background/screensaver/Exit, var/lib persistence, fresh installation and upgrades. Legacy app-local state requires an explicitly reviewed migration, not new keys.
- Owner approval of the final integration, tag and publication. No merge, worktree deletion, PR closure, release or new artifact distribution was performed here.

## Automated evidence before version preparation

Allocation/package hardening `ddfd984f280c1b0172ccf027ca4209410147402a`: [full Validate](https://github.com/depius/uxair-webos/actions/runs/41545391122) and [both ARM backends](https://github.com/depius/uxair-webos/actions/runs/41545391012) passed. Both logs show the generated bounded sources compiled, correct separate media libraries, max required GLIBC 2.7 and IPK permission/content checks. No artifacts were uploaded.

Legacy-identity guard `b8fced2747125786a513e2896adbf0be1d948585`: [full Validate](https://github.com/depius/uxair-webos/actions/runs/41545999553) and [both ARM backends](https://github.com/depius/uxair-webos/actions/runs/41545999493) passed before application version metadata was changed. The subsequent 1.2.0 metadata/documentation commit changes no runtime media or persistence code and must pass the same workflow matrix on its own exact SHA before candidate preparation. This report's release hold and physical gates remain in force after CI success.

## Final 1.2.0 automated validation

Code/metadata commit `d27fe4e840a2438378cbb862747ff03a906e0367` passed [full sanitizers and cross-UID storage validation](https://github.com/depius/uxair-webos/actions/runs/41546997117) and [separate Starfish/NDL ARMv7 builds](https://github.com/depius/uxair-webos/actions/runs/41546997122). Both 1.2.0 IPKs passed package identity/content/permission checks and ELF validation, with max required GLIBC 2.7 <=2.19. Starfish linked libplayerAPIs/libAcbAPI; NDL linked libNDL_directmedia. GitHub's artifact listing is empty. No candidate is distributed.

The subsequent remaining-advisory/report commit is documentation-only; runtime source, build logic, dependency graph and app metadata are byte-identical to d27fe4e. Physical acceptance and the security-release hold are unchanged by these CI results.

## Final owner-approved development scope

The 2026-11-03 [scope decision and NDL source review](UXAIR_1_2_SCOPE.md) supersede the former NDL hardware gate: Starfish is the stable release candidate pending hardened B7 validation; NDL is Beta/Experimental with no current-build physical validation. The owner has no webOS 5+ TV, so NDL hardware testing is deferred rather than required for this scope's completion. Confirmed unchecked paths and callback-lifetime uncertainty remain disclosed.

Revision 03242d26ec6a5aba7b2c84f93f8ec75a131339d7 passed [full sanitizer validation](https://github.com/depius/uxair-webos/actions/runs/41668703529) and [both same-revision ARM package builds](https://github.com/depius/uxair-webos/actions/runs/41668703360), including the new actual-source NDL tests with mocked APIs. No production source was changed by this scope-finalization step, and no artifacts were uploaded. Main integration, PR/branch closure and release publication still require explicit owner approval; the security release hold remains active.
