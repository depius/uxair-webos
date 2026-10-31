# B7 app-local storage compatibility

The hardware confirms UID/EUID 5999, GID/EGID 5000, an installation container
owned by 501:0 with mode 0777, persistent ext4 /media/developer, no developer
var/lib directory, and read-only /var/lib in the jail.

## Regression and fix

The older main implementation used app_directory()/state and app-root logs.
PR #10 at 8526f8d selected writable app-local or var/lib storage and permitted
a temporary fallback. Commit 72a276c introduced a strict parent owner check and
made start_log failure fatal, before SDL initialization. e7fcdd2 only diagnosed
that failure. Neither settings nor usage serialization caused it.

Starfish now treats its executable's app-local directory as a platform-managed
installation container. It opens that container with O_DIRECTORY/O_NOFOLLOW,
without chmod/chown and without a UID whitelist. This exception is selected
only for Starfish app-local storage; existing var/lib selection and NDL retain
the strict policy. State must still belong to the effective UID, is set to
0700, and existing files must be singly-linked regular files owned by that UID,
opened without following symlinks and restricted to 0600. Unsafe existing state
fails closed instead of selecting a new identity. No temporary fallback exists.

Existing identity, paired keys, settings and usage paths remain unchanged.
Starfish logs now live in state/uxair.log and state/uxair.log.previous. Rotation
validates both log leaves before renaming. Old app-root logs are left intact.
NDL log placement/initialization remains unchanged.

## Explicit security tradeoff

This restores the old platform/jail trust boundary; it does NOT prove that a
world-writable installation container or its ancestors are safe. It assumes
untrusted processes cannot concurrently manipulate this app's installation/state
namespace through the jail or host. A process with such access can rename state
through writable ancestors, race pathname-based operations, or remove identity
before a later launch. Same-UID processes, root, and the installation owner are
not isolated by this design. Startup validation does not eliminate those races.
A security-equivalent solution against hostile host writers requires privileged
provisioning and appropriate jail mounts of private persistent storage under
protected ancestors. That is deliberately outside this targeted regression fix.

## Validation and B7 procedure

CI runs the full sanitizer suite plus a root-created 501:0/0777 fixture with
children running as 5999:5000. Two launches exercise new and existing identity-file
bytes, real settings/usage serializers, log creation/rotation, and symlink,
hardlink, FIFO and foreign-owner rejection. The installation metadata must remain
unchanged. These tests use identity/key fixtures; they do not establish real
AirPlay cryptographic pairing or emulate the TV jail. Strict-policy and NDL
selection tests remain enabled. Only the Starfish package is cross-built.

Install the new arm-webos3-4 IPK as an upgrade, without uninstalling or deleting
state. Back up any existing state first. Launch normally and confirm the menu
appears. If startup fails, repeat the jailed command in B7_STORAGE_DIAGNOSTICS.md
and collect early stderr. Successful Starfish logs are under <app>/state/uxair.log
(in the jail's corresponding app path when viewed from root).

Pair an iPhone, change a setting, play media, then close/relaunch and reboot.
Confirm pairing/identity, settings and usage history survive, and inspect log
rotation. Test actual H.264/HEVC/RAOP and Now Playing behavior. Upgrade retention,
jail teardown/reboot persistence, graphics/media operation and newer-TV NDL
runtime behavior still require physical hardware. No performance or playback
support is inferred solely from a successful package build.
