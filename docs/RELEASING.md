# UxAir release procedure

## UxAir 1.3.0

Version 1.3.0 combines [PR #14](https://github.com/depius/uxair-webos/pull/14) (Starfish audio-queue stability) and [afds's PR #13](https://github.com/depius/uxair-webos/pull/13) (older webOS 3.4 OpenSSL / SDL compatibility). Both original PRs were merged independently to retain contribution history. The owner confirmed hardware acceptance of the combined B7 test candidate. webOS 5+ NDL remains Public Beta without acceptance of the current build.

1. Use the final `main` commit after metadata and release-notes integration. Ensure `packaging/appinfo.json` is `1.3.0`, preserve app ID `com.github.depius.uxair`, and run the required GitHub Actions native sanitizers, storage tests, and both ARMv7 builds. Do not reuse earlier TEST IPKs: those embed different commits and are not a 1.3.0 release build.
2. In GitHub Actions, dispatch `armv7-ipk.yml` from `main` with `prepare_release_assets=true`. The workflow builds and verifies both ARMv7 backends, then retains versioned per-backend candidate artifacts including a `BUILD_COMMIT.txt`, ELF report and SHA-256 manifest; the Starfish candidate also provides the corresponding source archive.
3. Download both verified candidate artifacts. Confirm both `BUILD_COMMIT.txt` files contain **the same final main SHA**, check SHA-256, app ID/version, ARMv7 ABI, and filenames. Combine both IPKs and source archive with a single release-wide `SHA256SUMS` verified against all three files. Keep development-only and previous TEST artifacts out of the GitHub Release.
4. Confirm final owner approval and retain the LG B7 acceptance notes. Tag exactly the main commit embedded in the final packages as `v1.3.0`. Publish the assets below with the concise body in [RELEASE_NOTES_1.3.0.md](RELEASE_NOTES_1.3.0.md); never move the published tag later to a mismatched commit.

   - `UxAir-1.3.0-webOS3-4-Starfish.ipk` (Stable; physically accepted on LG B7)
   - `UxAir-1.3.0-webOS5plus-NDL-Beta.ipk` (Public Beta / Experimental; no physical validation of this build)
   - `uxair-webos-1.3.0-source.tar.gz`
   - `SHA256SUMS`

5. Verify published release download URLs, checksum matches, README links and release notes. Preserve original contribution attribution. Keep the experimental Direct Video/HLS PR #9 out of the release.

**Upgrade:** Back up private state and install the matching IPK in place. Avoid uninstalling the existing app because that may erase pairing identity, settings or history. NDL legacy state migration remains a separate concern.

---

## UxAir 1.2.0 – archived release procedure

The development scope is frozen. Starfish for webOS 3–4 is Stable. The owner accepted the exact Starfish build at `9af36d1b2e3902bfa26ab349140af394b3b95a3f` on the LG OLED B7 running webOS 3.9. NDL for webOS 5+ is Public Beta / Experimental and has no current-build physical validation. See the [acceptance record](B7_FINAL_TEST.md) and [Beta reporting guide](NDL_BETA_TESTING.md).

## Prepare and verify

1. Keep app ID `com.github.depius.uxair` and version `1.2.0`. Run the full sanitizer suite, cross-UID storage tests and both explicit ARM targets with the [locked build procedure](DEVELOPMENT.md). Verify IPK contents, permissions, ABI, dependency separation, embedded commit and checksums. Both backends must pass the required `build-ipk` check.
2. On `fix/pr10-b7-compatibility`, dispatch `armv7-ipk.yml` with `prepare_test_assets=true` for authenticated, one-day staging. The workflow verifies corresponding project and dependency sources. Retain verified local copies. Use these production IPK asset names:

   - `UxAir-1.2.0-webOS3-4-Starfish.ipk`
   - `UxAir-1.2.0-webOS5plus-NDL-Beta.ipk`

3. Compare the rebuilt Starfish payload to the physically accepted package and record any differences. Save the exact candidate commit, source archive, `SHA256SUMS` and [changelog](../CHANGELOG.md) for approval. The owner’s physical result applies to the tested IPK. Separate fresh-install and prior-version upgrade coverage was not specified. Earlier CLI and Glasshouse results are in the [branch review](B7_BRANCH_FINAL_REVIEW.md).
4. Review and record remaining technical risks in the [security findings](SECURITY_HARDENING.md) and [packaging dependency audit](PACKAGING_DEPENDENCY_AUDIT.md). Verify release assets and checksums before publication.

## Integrate and publish after approval

1. Obtain explicit owner approval for the exact prepared revision and the effect on PR #10. Recheck main and the open PRs. Main requires a pull request and passing status checks. Use a merge commit to preserve the contributor's original SHAs and authorship. PR #10's head `8526f8d` is already an ancestor, so GitHub can mark it merged indirectly. Keep PR #9 separate. See [GitHub's merge documentation](https://docs.github.com/en/pull-requests/reference/pull-request-merges#indirect-merges).
2. Verify the actual main tree and CI after the merge. Verify PR #10's recorded state before posting the approved thank-you. If the merge tree matches the prepared revision, tag the prepared revision on main's history so the release tag, source archive and embedded IPK revision agree. If the tree differs, reassess the diff and rebuild both backends from a single approved revision.
3. After final publication approval, publish `v1.2.0` with both labeled IPKs, the corresponding source archive, `SHA256SUMS`, release notes and the Beta feedback link. Verify the published files and their checksums.

## Installation notes

Both packages use the same app ID. Install the package matching the TV's webOS version. Back up private state and upgrade in place. A legacy NDL app-local identity without usable var/lib state requires reviewed migration. For the CLI and the B7 Dev Manager preflight limitation, see the [installation guide](../README.md#download-and-install).
