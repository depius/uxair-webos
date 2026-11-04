# Milestone 5G: what the B7 UHD probe actually proves

> Historical development record. See [UxAir 1.2.0 scope](UXAIR_1_2_SCOPE.md) for current release and validation status.

The owner-run 0.1.3 probe completed separate 1080p and UHD launches on the LG OLED B7, Rockhopper 3.9.0. In UHD mode it validated HEVC Main 8-bit SDR SPS 3840×2160, accepted 30/30 access units and EOS, received seven `FRAMEREADY` callbacks and Starfish video-info 3840×2160, reported no errors, completed ACB/Starfish cleanup and exited zero. This establishes the **input/API control path**. No one has observed the panel, decoded surfaces or an independently identified output plane. The receiver application is unchanged.

## Evidence levels

| Level | Required evidence | B7 status |
| --- | --- | --- |
| 1. Input accepted | Load, Feed, EOS and a successful control lifecycle for the verified local sample. | **Established** for 1080p and 3840×2160 Main 8-bit SDR. |
| 2. Decoder activity | A decoder-specific hardware counter, decoded-picture event with verified semantics, or another independent decoder observation tied to this probe. | **Unverified.** `FRAMEREADY` is a firmware callback; its seven calls for 30 input access units do not establish a decoded-picture count. |
| 3. Presentation activity | Evidence that this probe's decoded frames reach an active video sink or plane, ideally with a changing frame/presentation counter or visible motion. | **Unverified.** ACB PLAYING and its requested window do not prove presentation. A read-only video-sink status may add supporting evidence if present and attributable. |
| 4. Native UHD presentation | An independently identified active plane with 3840×2160 source and native UHD presentation, or an equivalent validated output measurement without downscaling. | **Unverified.** No such B7 measurement has been supplied. |

## Firmware/API findings and confidence

| Interface | Source evidence | B7 classification and interpretation |
| --- | --- | --- |
| Starfish `Load`/`Feed`/video-info and ACB state | The [public Starfish header](https://www.webosbrew.org/webos-userland/d3/dda/StarfishMediaAPIs_8h_source.html) lists `Feed`, `getVideoRenderQueueLength` and the event types. The 0.1.3 B7 log confirms the actual API lifecycle. | **Confirmed and usable for Level 1.** Video-info may reflect parsed stream metadata. Neither the header nor the B7 callback log defines `FRAMEREADY` as a displayed or decoded frame. Render queue length would describe buffering, not scanout. |
| ACB display window | [Probe code](../src/hevc_probe.cpp) passes `SDL_GetCurrentDisplayMode` width and height to `AcbAPI_setDisplayWindow`. The UHD log records 1920×1080 as `acb_window`. [LG documents separate 1920×1080 graphics and 3840×2160 UHD video resolutions](https://webostv.developer.lge.com/develop/specifications/app-resolution); [webosbrew's webOS 3 sample](https://github.com/webosbrew/samples) uses ACB for the video plane. | **Confirmed requested geometry; output resolution unverified.** The 1920×1080 value comes from the SDL display mode and is a window coordinate request. It neither proves downscaling nor native UHD display. Do not increase it to 3840×2160 without a separate geometry experiment. |
| `com.webos.service.videooutput/getStatus` | [webOS OSE documents](https://www.webosose.org/docs/reference/ls2-api/com-webos-service-videooutput/) a read-only sink status with `connectedSource`, `sourceInput` and `displayOutput` rectangles. The API is marked retired in that source. | **Supported by source, unverified on Rockhopper.** A `VDEC` sink correlated with the probe would support video-plane connection. The rectangles are reported source cropping and output placement, not decoded-frame or native panel counters. The response can include a private pipeline context, so the script never prints raw JSON. |
| `com.webos.service.avoutput/video/getStatus` | [webOS OSE's older avoutput API](https://www.webosose.org/docs/reference/ls2-api/com-webos-service-avoutput/) documents a read-only sink query and marks it retired. | **Supported by source, unverified on Rockhopper.** Used only as fallback when the videooutput query does not succeed. Interpret rectangles with the same limits. |
| `com.webos.media/getForegroundAppInfo` | [webOS OSE documents](https://www.webosose.org/docs/reference/ls2-api/com-webos-media/) foreground app and ACB window/playstate data. | **Supported by source, unverified on Rockhopper.** At most correlates a probe foreground session and ACB geometry. It does not report actual plane source resolution. |
| `/sys/kernel/debug/dri/{0,1}/state` | A standard DRM state dump can expose plane source and destination rectangles when the running kernel exposes it. | **Hypothetical on the B7.** The collector checks only already readable files and never mounts debugfs. Even a 3840×2160 source rectangle needs attribution to this probe and an active plane before it supports Level 3/4. Other planes may be listed. |
| `/proc/interrupts` decoder-named IRQ | Linux provides interrupt totals; a firmware IRQ name might include `HEVC`, `VDEC` or `VPU`. | **Hypothetical on the B7.** A rising count during the probe would be supporting hardware-activity evidence, but the interrupt can be shared or caused by another pipeline. It is not a decoded-frame count. |
| UMediServer/TVRM resource files | The owner already found firmware HEVC capability limits and `HEVCP1_HEVC`/`VDEC` resource names in configuration. | **Confirmed static capability clues only.** No verified read-only B7 runtime resource allocation or per-frame counter has been identified. |
| GStreamer 1.8, Surface Manager and screenshots | The pipeline and compositor can maintain separate graphics and video surfaces. No B7-specific read-only API with verified decoded-frame, presentation or native-plane-resolution semantics has been found in the inspected public interfaces. | **Undocumented for this purpose.** A generic screenshot may omit the hardware plane or be downscaled; it would require a controlled visual-content comparison before serving as presentation evidence. |

The public interfaces above do not by themselves establish Levels 2–4 remotely. The collector below is a **discovery and correlation experiment**, not a claim that Rockhopper implements these OSE services. If they are absent or return ambiguous values, the next step is owner-provided sanitized B7 output and identification of a firmware-specific counter or an independent visual/output measurement. No probe binary change is justified yet: longer playback would not create a trustworthy measurement, and the present 1080p/UHD control paths are already complete.

## Bounded read-only collector

[`tools/b7-media-observe.sh`](../tools/b7-media-observe.sh) uses POSIX shell, BusyBox-style tools and optional `timeout`/`luna-send`. `snapshot` takes one sample; `watch` takes 12 at one-second intervals. It checks the probe PID and RSS, a decoder-named IRQ total, already readable DRM state rectangles, and two read-only Luna status services. Each Luna call has a two-second limit and a 4096-byte response cap. The script prints only fixed categories and numeric geometry, never raw service replies, context IDs, session IDs or app data. Its output is bounded, and it makes no firmware/configuration writes, debugfs mounts, subscriptions or service restarts. Copying the script to `/tmp` for manual execution does not change the installed applications.

`first_source_rect` and `first_output_rect` are the **first rectangles found** in a service response. A multi-sink response can make them unrelated to the probe; compare idle and active samples and do not infer a native plane from them. `candidate_decoder_irq_total` is likewise unattributed. The collector reports `unavailable-or-denied` when a service cannot be queried safely. This is a finding, not a reason to elevate privileges or change service policy.

## Remote procedure

In Windows PowerShell, set the address and existing root key, copy the one small script, and take a baseline. The key remains on the PC:

```powershell
$env:TV_HOST = 'TV_IP'
$env:TV_KEY = 'C:\path\to\tv-key'
scp -i $env:TV_KEY .\tools\b7-media-observe.sh "root@$($env:TV_HOST):/tmp/b7-media-observe.sh"
ssh -i $env:TV_KEY "root@$env:TV_HOST" 'sh /tmp/b7-media-observe.sh snapshot'
```

Launch the existing 0.1.3 `mode=1080p` in a rooted SSH shell and confirm its log has `control-path=COMPLETE cleanup_ok=1`, `uhd-gate-token written=1` and `exit=0` (see [Milestone 5F](MILESTONE5F.md)). Ensure `pidof hevc-probe` is empty. In a first PowerShell window start the bounded collector, which writes only sanitized output to a **local PC file**:

```powershell
ssh -i $env:TV_KEY "root@$env:TV_HOST" 'sh /tmp/b7-media-observe.sh watch' | Tee-Object .\b7-uhd-observation.txt
```

Immediately, in a second rooted SSH shell, launch the explicitly gated UHD probe:

```sh
luna-send -n 1 -f luna://com.webos.applicationManager/launch '{"id":"com.github.danlflh.uxplaywebos.hevcprobe","params":{"mode":"uhd"}}'
```

After it exits, take another single snapshot. If the 12-second watch missed the brief playback, repeat the **fresh 1080p then UHD** sequence; the gate is single-use. Do not run concurrent probes. For the relevant probe log path identified in [Milestone 5F](MILESTONE5F.md), extract bounded markers with this one-line command, replacing `LOG` with that path:

```sh
grep -E '\[hevc-probe\] (uhd-gate|phase-begin|sample-valid|load-return|acb-|feed-stop|result|control-path|cleanup|termination|exit)' LOG | tail -n 100
```

Compare baseline, active and after values. A probe-correlated `VDEC` connection or changing plane geometry would justify a focused follow-up, but **not** an immediate Level 4 claim. A stable named decoder IRQ increase only supports a hypothesis about hardware activity. If both status services and DRM state are unavailable, report that exact result; do not replace it with SPS or ACB values. Stop and retain the current probe and receiver if media resources become unavailable. Follow [Milestone 5F rollback](MILESTONE5F.md#stop-and-rollback) for the separate probe only. No HDR, Main10 or AirPlay HEVC work follows from this milestone.

## Validation

The collector's host test uses synthetic status replies containing secret-like context and pipeline IDs; it checks numeric extraction, bounded output and absence of those IDs. The Ubuntu workflow runs that test with the existing native tests. No probe source, sample, app ID, receiver source or IPK packaging changes were made in this milestone, so no new probe IPK is needed. Actual service availability and native plane output remain to be measured on the B7.
