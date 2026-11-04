> Superseded by the focused [B7 storage fix](B7_STORAGE_FIX.md). The diagnostic-only behavior below describes e7fcdd2.

# B7 storage startup diagnostics (2026-10-31)

This is an investigation build, not a confirmed startup fix. It retains the
storage selection, ownership checks and permission changes from `72a276c`.
Do not treat a green package build as evidence that the application starts on B7.

## Established failure and remaining uncertainty

The reported installation directory is
`/media/developer/apps/usr/palm/applications/com.github.depius.uxair`, owned by
UID 501, GID 0, mode 0777. With no var/lib state, Starfish selects that directory.
`owned_directory()` opens it without following a symlink, then rejects
`st_uid != geteuid()` before chmod or state creation. A direct-root process has
EUID 0, so the reported rejection is fully explained. Mode 0777 grants access
but cannot satisfy an ownership check.

The jailed process's UID/GID and in-jail directory metadata are not yet known.
If its EUID differs from the installation owner, the same condition fails. If
it equals 501, the parent check passes and the existing code attempts to remove
group/other write permission with fchmod; a later filesystem or state-file check
could still fail. No UID is inferred from the app-directory owner. Root-side
paths and mount views must not be assumed identical to the jail's views.

No ownership exception, chmod-777 workaround, state migration, chown, temporary
identity directory or fallback was introduced. NDL still selects var/lib;
Starfish still prefers existing app-local state, reusing existing var/lib state
only when app-local state is absent. An unsafe local state does not cause a
switch to a different identity.

## Capture the actual jail diagnostics

Install the production-identity Starfish artifact from this branch. Keep a
backup of existing state and do not delete, recreate or chown it for this test.
Close UxAir before a manual launch so two instances cannot write the same state.

The early stderr output includes:

- PID, real/effective UID and real/effective GID, and the compiled backend.
- Both candidate parent/state paths, lstat errors or numeric ownership/modes.
- The selected storage path and selection policy.
- The exact failed operation/check, errno/text, expected UID, and available
  actual ownership/mode/link-count metadata. No key or identity contents.

From the existing root SSH shell, use the native developer jail launch documented
by [webOS Homebrew](https://www.webosbrew.org/pages/native-development):

```sh
APP=com.github.depius.uxair
APPDIR=/media/developer/apps/usr/palm/applications/$APP
XDG_RUNTIME_DIR=/tmp/xdg /usr/bin/jailer -t native_devmode -i "$APP" -p "$APPDIR" "$APPDIR/uxair" '{"@system_native_app":true,"nid":"com.github.depius.uxair"}' 2>&1
```

Capture the `[state]` lines in the terminal, including the first `startup` line
and first `rejected` line. This collects stderr even if the child exits in
0.2 seconds; `/proc/<pid>/status` does not need to be sampled in time. If the
normal launcher uses another jail profile, reproduce that profile as well;
this command alone does not prove equivalence with every launcher configuration.
`/tmp/xdg` is the existing graphics runtime path, not identity storage.
The build deliberately does not create a fallback diagnostic file in an unsafe
directory when persistent storage validation fails.

## Automated coverage and hardware follow-up

The ordinary secure-data tests cover selection priority, missing var/lib,
NDL selection, permissions, symlinks, hardlinks and FIFOs. A separate Linux
root-only sanitizer test creates a disposable 501:0/0777 fixture, then forks
processes with real/effective UIDs 0, 65001 and 501. UID 65001 is a test value,
not a claim about the B7. It checks diagnostic fields, unchanged rejected
parents, unchanged existing identity bytes/owner/mode, and the owner-UID control.
These are filesystem/credential simulations, not webOS jail emulation.

The full test suite and root fixture must pass in CI before package upload.
Only the Starfish IPK is built. NDL selection is tested, but NDL runtime behavior
and its full target build are not validated by this diagnostic run.

Next required evidence: the actual jailed startup output. Only then choose a
narrowly scoped storage compatibility fix. That fix still needs B7 tests for
normal startup, existing identity/pairing reuse, persistent settings/log writes,
restart/reboot, and an NDL television check for shared storage changes.
