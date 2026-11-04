# Milestone 5F: separate manual launches for UHD HEVC testing

> Historical development record. See [UxAir 1.2.0 scope](UXAIR_1_2_SCOPE.md) for current release and validation status.

## What the B7 evidence shows

Probe 0.1.2 completed its fresh 1080p control path (30/30 Feed acceptance, seven `FRAMEREADY` callbacks, matching 1920×1080 video-info, and successful cleanup). A SIGTERM (15) arrived during the following 500 ms in-process pause. UHD never began. The probe source did not send SIGTERM; its 45-second watchdog uses SIGALRM. The available `/var/log/messages` excerpt does not identify the sender. Application Manager, a media service, or another process remain hypotheses. Neither the 1080p callback nor UHD SPS alone proves decoded pictures or native presentation.

## Probe 0.1.3 behavior

Use **two explicit Luna launches**. `mode=1080p` runs only the established 1080p case. If its complete control path and Starfish/ACB cleanup succeed, it writes a five-minute, one-use gate. `mode=uhd` consumes that gate before initializing SDL or media, then runs only the packaged HEVC Main 8-bit SDR 3840×2160/30 sample. It cannot run from a stale `1080p-decoder-pass.flag`. The gate is bound to the current boot ID, packaged source commit, installed executable identity and successful 1080p run. A new 1080p launch clears the previous gate. One process lock prevents overlapping probes. If the app directory is unwritable, the log uses `/tmp/uxplay-hevc-probe.log` and the gate uses a private, owner-only `/tmp/uxplay-hevc-probe-state` directory. If required identity or storage is unavailable, UHD fails closed. A new process and 45-second watchdog are used for each launch; no TV service is restarted.

The log records bounded `phase-begin`, `uhd-first-feed`, `feed-stop`, `result`, `control-path`, `cleanup-*`, `termination` and `exit` markers. Signal diagnostics include sender PID (when the OS supplies it), `si_code`, captured phase (0 startup, 1 1080p, 2 UHD, 3 cleanup, 4 done), monotonic milliseconds and whether the probe had requested normal teardown. A PID or signal code may narrow the source but does not by itself establish why it was sent. No raw launch JSON or media payload is logged. The 1 MiB startup log cap and existing event limits remain.

The sample parser enforces VPS/SPS/PPS, Main 8-bit, exact dimensions and bounded size before Starfish Load. Existing Feed retry, sample, memory and cleanup bounds remain. `control-path=COMPLETE` means the input/API lifecycle completed. `FRAMEREADY` is callback evidence, not independent proof of displayed frames. The requested ACB display window and Starfish video-info are not output-plane resolution measurements. Even a complete UHD path is **not native 4K presentation proof**.

## Manual B7 procedure from rooted Windows SSH

Download the `hevc-probe-armv7-<source SHA>` artifact from the successful [HEVC probe workflow](https://github.com/depius/uxair-webos/actions/workflows/hevc-probe-ipk.yml), verify all files against `SHA256SUMS`, and keep the 0.1.2 IPK for rollback. Close active AirPlay playback and make sure the probe is not running. These commands never install or start anything automatically from CI.

From PowerShell, replace the key and IP placeholders:

```powershell
scp -i C:\path\to\tv-key .\com.github.danlflh.uxplaywebos.hevcprobe_0.1.3_arm.ipk root@TV_IP:/tmp/
ssh -i C:\path\to\tv-key root@TV_IP
```

On the TV, confirm no existing probe PID, then install **only the separate probe** and wait for the install service's completion response:

```sh
pidof hevc-probe || true
luna-send -i -f luna://com.webos.appInstallService/dev/install '{"id":"com.github.danlflh.uxplaywebos.hevcprobe","ipkUrl":"/tmp/com.github.danlflh.uxplaywebos.hevcprobe_0.1.3_arm.ipk","subscribe":true}'
```

Launch 1080p and wait for `pidof hevc-probe` to become empty. Check the log for `control-path=COMPLETE cleanup_ok=1`, `uhd-gate-token written=1`, and `exit=0`. Start UHD within five minutes; never launch both at once:

```sh
luna-send -n 1 -f luna://com.webos.applicationManager/launch '{"id":"com.github.danlflh.uxplaywebos.hevcprobe","params":{"mode":"1080p"}}'
pidof hevc-probe || true
luna-send -n 1 -f luna://com.webos.applicationManager/launch '{"id":"com.github.danlflh.uxplaywebos.hevcprobe","params":{"mode":"uhd"}}'
```

`luna-send` returning `returnValue:true` acknowledges launch, not test success. Read the final log after the probe exits. The startup marker reports `log=app` or `log=tmp`:

```sh
APP=com.github.danlflh.uxplaywebos.hevcprobe
for FILE in \
  "/var/palm/jail/$APP/media/developer/apps/usr/palm/applications/$APP/hevc-probe.log" \
  "/media/developer/apps/usr/palm/applications/$APP/hevc-probe.log" \
  "/media/cryptofs/apps/usr/palm/applications/$APP/hevc-probe.log" \
  "/var/palm/jail/$APP/tmp/uxplay-hevc-probe.log" \
  /tmp/uxplay-hevc-probe.log; do
  if test -f "$FILE"; then echo "LOG=$FILE"; tail -n 350 "$FILE"; fi
done
```

Share the `uhd-gate`, `phase-begin`, `sample-valid`, `load-return`, `feed-stop`, `result`, `control-path`, `cleanup`, `termination` and `exit` lines from both launches, plus any independent output-plane measurement. If a signal appears, also share its sender PID and `si_code` and a short matching `/var/log/messages` excerpt. Do not infer native UHD display from SPS, Feed, or ACB window values.

## Stop and rollback

Wait for the finite probe to exit. If it hangs before the watchdog, identify the exact probe PID with `pidof hevc-probe` and send `kill -TERM <PID>`; the probe attempts normal cleanup, while the 45-second hard deadline bounds a stuck firmware call. It never runs at boot. To revert, copy the saved **0.1.2 probe IPK** to `/tmp` and use the same `dev/install` call with that filename. If the installer refuses a downgrade, remove only `com.github.danlflh.uxplaywebos.hevcprobe` via `luna://com.webos.appInstallService/dev/remove`, then reinstall 0.1.2. Leave `com.github.danlflh.uxplaywebos.dev` and its pairing state untouched. If a media resource remains busy, stop testing and inspect it manually; the probe does not restart firmware services.

## Validation status

The Ubuntu workflow runs native HEVC parser, launch, Feed and one-use gate tests with sanitizers before the macOS ARM64 cross-build. The build workflow verifies the ARMv7 ELF ABI, package isolation and checksums before uploading the IPK and source SHA. The actual 0.1.3 UHD control path and panel presentation require an owner-run B7 test.
