# UxAir 1.2.0 release procedure

The development scope is frozen. Starfish for webOS 3–4 is Stable. The owner accepted the exact Starfish build at `9af36d1b2e3902bfa26ab349140af394b3b95a3f` on the LG OLED B7 running webOS 3.9. NDL for webOS 5+ is Public Beta / Experimental and has no current-build physical validation. See the [acceptance record](B7_FINAL_TEST.md) and [Beta reporting guide](NDL_BETA_TESTING.md).

## Prepare and verify

1. Keep app ID `com.github.depius.uxair` and version `1.2.0`. Run the full sanitizer suite, cross-UID storage tests and both explicit ARM targets with the [locked build procedure](DEVELOPMENT.md). Verify IPK contents, permissions, ABI, dependency separation, embedded commit and checksums. Both backends must pass the required `build-ipk` check.
2. On `fix/pr10-b7-compatibility`, dispatch `armv7-ipk.yml` with `prepare_test_assets=true` for authenticated, one-day staging. The workflow verifies corresponding project and dependency sources. Retain verified local copies. Use these production IPK asset names:

   - `UxAir-1.2.0-webOS3-4-Starfish.ipk`
   - `UxAir-1.2.0-webOS5plus-NDL-Beta.ipk`

3. Compare the rebuilt Starfish payload to the physically accepted package and record any differences. Save the exact candidate commit, source archive, `SHA256SUMS` and [changelog](../CHANGELOG.md) for approval. The owner’s physical result applies to the tested IPK. Separate fresh-install and prior-version upgrade coverage was not specified. Earlier CLI and Glasshouse results are in the [branch review](B7_BRANCH_FINAL_REVIEW.md).
4. Keep the security hold in effect until the owner records a security disposition. Preserve the historical dc3fe475 ZIP and its SHA-256. The browser Malware verdict still lacks a verified analyst disposition. Follow browser and antivirus protections. See the [security findings](SECURITY_HARDENING.md) and [packaging dependency audit](PACKAGING_DEPENDENCY_AUDIT.md).

## Integrate and publish after approval

1. Obtain explicit owner approval for the exact prepared revision and the effect on PR #10. Recheck main and the open PRs. Main requires a pull request and passing status checks. Use a merge commit to preserve the contributor's original SHAs and authorship. PR #10's head `8526f8d` is already an ancestor, so GitHub can mark it merged indirectly. Keep PR #9 separate. See [GitHub's merge documentation](https://docs.github.com/en/pull-requests/reference/pull-request-merges#indirect-merges).
2. Verify the actual main tree and CI after the merge. Verify PR #10's recorded state before posting the approved thank-you. If the merge tree matches the prepared revision, tag the prepared revision on main's history so the release tag, source archive and embedded IPK revision agree. If the tree differs, reassess the diff and rebuild both backends from a single approved revision.
3. After publication approval and security disposition, publish `v1.2.0` with both labeled IPKs, the corresponding source archive, `SHA256SUMS`, release notes and the Beta feedback link. Verify the published files and their checksums.

## Installation notes

Both packages use the same app ID. Install the package matching the TV's webOS version. Back up private state and upgrade in place. A legacy NDL app-local identity without usable var/lib state requires reviewed migration. For the CLI and the B7 Dev Manager preflight limitation, see the [installation guide](../README.md#download-and-install).
