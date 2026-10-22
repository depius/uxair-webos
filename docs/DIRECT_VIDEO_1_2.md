# UxAir 1.2 Direct Video: implementation and B7 acceptance

PR #9 implements a separate native Starfish HLS URI player, PTTH event transport, a bounded HLS relay, and Direct Video History/Diagnostics. Hardware playback and contemporary Safari compatibility are **not yet verified**. Default discovery and the production 1.1.0 application are unchanged. No stable release or merge is authorized.

## Protocol and lifecycle

The development-only pinned-source transform routes `/play`, `/reverse`, `/action`, `/playback-info`, `/scrub`, `/rate`, `/stop`, `/info`, `/server-info` and property operations. Binary/XML dictionaries and legacy play bodies use pinned libplist; play bodies are capped at 16 KiB, action envelopes at 512 KiB, URLs at 4096 bytes, commands at eight and pending reverse messages at sixteen. PTTH returns 101 using the existing UxPlay PTTH connection type. The generated HTTP reader processes available fragments once instead of spinning until an eight-byte prefix arrives; negotiated reverse traffic is excluded from the request parser. A separate bounded writer sends loading/playing/paused/stopped/error events without firmware calls from RAOP threads. The connection destruction hook stops its owning session; short-lived control sockets do not stop playback. Legacy clients without a reverse connection have a 30-second control lease.

FCUP `unhandledURLRequest` follows the pinned UxPlay schema. `/action` accepts only correlated request ID + owner + URL responses, validates status and limits decoded manifest data to 256 KiB. It provides a three-second manifest fallback for eligible failed m3u8 retrievals; segments remain subject to the relay policy. Playlist removal stops playback. Unknown action/property mutations return 501 instead of fake acknowledgments. Read-only position/duration/rate properties accept GET and the pinned POST exchange. Play supports legacy fractional Start-Position and explicit Start-Position-Seconds (seconds takes precedence when both are valid). Known /info TXT queries use the pinned info writer only inside the opt-in gate; fp-setup2 returns 501. Playback feedback is implemented; loaded ranges are deliberately empty when unmeasured. Unknown playlistInsert variants, subtitles, DRM and property features require concrete sender evidence before extension.

Callbacks carry generation IDs in a bounded live registry; an unloaded player's callbacks cannot change its successor. No firmware API runs while a callback-state mutex is held. Only the SDL main loop owns native player operations. Relay cancellation precedes native unload. Stop, EOF, native errors, reverse connection loss and lease expiry release Direct ownership. Concurrent mirror/audio starts are rejected while Direct owns the decoder. Stop Direct before switching modes. Back stops; OK toggles live Diagnostics. Native video-info is forwarded to ACB and display windows preserve aspect ratio.

## Native HLS and network policy

Starfish receives only a random-token loopback relay URL, never a signed upstream URL. The relay rewrites master/media playlist entries and URI attributes, including separate fMP4 initialization files. HTTP/HTTPS, relative URLs and up to three redirects are supported. Each connection checks the actual destination IPv4: loopback, link-local, multicast and private networks are denied. A single explicit `UXAIR_DIRECT_LAN=PC-IP` exception enables the local fixture. Redirects and DNS changes cannot bypass that connection policy. IPv6, URI credentials and non-HTTP schemes are rejected. Proxy environment settings and netrc credentials are ignored.

The new pinned curl 8.23.0 uses pinned OpenSSL with certificate/hostname verification. A committed Mozilla CA bundle (2026-10-20, SHA-256 796f1389bfea13c0ed78f663b765baaff54a508e5483786bd6ece73c5e3e3cbe) is packaged with MPL notices. Downloads have three-second connect and twelve-second total deadlines, an eight-MiB segment cap, 256-KiB manifest cap and a catalog bounded to 4096 resources / two MiB of URLs. The relay is loopback-only with two active fetches (at most sixteen MiB of segment payloads) and bounded client read/write timeouts. No media is written to app storage. Protocol diagnostics stop after 192 records per process; the existing one-MiB log rotation also runs during Direct playback. Encryption/key playlists, byte-range HLS and content steering are explicitly rejected; TLS is never disabled. H.264/AAC SDR up to 1080p is the initial target. No 4K, all-profile HLS or DRM claim is made.

The native firmware still performs demuxing, decoding and A/V sync; its memory use and URI/Load behavior require B7 measurement. Actual duration/playtime event units and MPEG-TS/fMP4 playback require hardware verification. No host test proves these properties.

## History and Diagnostics

V3 stores a separate Direct coverage start, so legacy Direct lifetime totals are not misrepresented as known zero daily activity. Partial recorded coverage receives the existing * indicator. V3 adds Direct daily milliseconds/session counts and reads V1/V2 without erasing data or inventing prior Direct daily usage. Storage remains 64 KiB, 100 recent sessions and 365 daily entries. Played intervals are assigned to UTC day buckets across pauses, rather than incorrectly treating paused time as contiguous playback. Recent sessions, lifetime counters, three-color stacked charts, three-category duration percentages and Direct Diagnostics are integrated. Unknown codec remains unknown; no URL/credentials are stored in History.

## Source evidence / upstream comparison

Pinned UxPlay remains 9f3c2bbc (1.74). Its reverse PTTH ownership, FCUP schema and controls were inspected alongside upstream 0477d47; its YouTube/GStreamer playlist implementation is not wholesale imported into this native receiver. Source: https://github.com/FDH2/UxPlay . Native API declarations: https://www.webosbrew.org/webos-userland/d3/dda/StarfishMediaAPIs_8h_source.html . LG HLS options: https://webostv.developer.lge.com/develop/guides/mediaoption-parameter . These establish API/protocol evidence, not Safari/B7 acceptance.

## B7 zero-request investigation (2026-10-23)

The hardware-tested 4676c97 build reached Starfish Load return 1 and 147 callbacks but made **zero native relay requests** before the unchanged 15-second deadline. This rules out the Direct queue/SDL dequeue and synchronous Load failing to return. It does not prove URI acceptance, HLS parsing, socket access or decoder readiness.

The native header decodes the reported events as follows:

| Event | Native name | What the observation establishes |
| --- | --- | --- |
| 0x0d, value 1 | INT_NUM_PROGRAM | A program-count notification; not proof of any bytes fetched |
| 0x02 | INT_BUFFER_RANGE_INFO | Buffer-range notification; not a terminal error |
| 0x01 | STR_STREAMING_INFO_PERI | Periodic streaming information; numeric value 0 omits its string payload |
| 0x12 / 0x13 | INT_ERROR / STR_ERROR | Explicit error callbacks; the earlier trace did not show either |

### Source-supported correction and evidence limits

LG's [Chromium SMP initialization](https://github.com/webosose/chromium84-temp/blob/df04aa361ad493d47003a80ebfa3034dfc9de6e0/src/media/neva/webos/media_platform_api_webos_smp.cc) calls notifyForeground **before** Load. This was missing in DirectPlayer and is now called through a weak symbol (no new mandatory loader dependency), with availability/result/timing recorded. As in that source, its boolean is not interpreted as asynchronous readiness. Working mirroring initialization is unchanged.

That source builds args[0] for **BUFFERSTREAM**. Its [URI client](https://github.com/webosose/chromium84-temp/blob/df04aa361ad493d47003a80ebfa3034dfc9de6e0/src/media/neva/webos/umediaclient_impl.cc) instead calls uMediaClient::loadAsync(uri, kMedia, options). [uMediaClient implementation](https://github.com/webosose/umediaserver/blob/master/src/media_client/uMediaClient.cpp) sends the distinct object `{uri:string,type:"media",payload:object}`. These public sources **do not establish** that B7 StarfishMediaAPIs::Load supports arbitrary URI playback or that the uMS envelope is accepted by its private implementation. The old args/HLS JSON was valid JSON but its URI contract was unverified. The two URI envelopes are explicit diagnostic alternatives, not a newly claimed correct Starfish contract.

Normal Direct still defaults to args/HLS, now with notifyForeground and an explicit mediaFormat.type="video" (the [documented default](https://webostv.developer.lge.com/develop/guides/mediaoption-parameter)). HLS and URI are documented transport names; m3u8 extension and HLS MIME remain preserved. No source evidence establishes a B7 loopback prohibition, so the relay remains **loopback-only**. Namespace checks report only same/different/unavailable and do not establish which process actually handles a URI or prove a security policy.

New diagnostics emit the actual serialized Load field names/types through a bounded JSON parser, contract name, source class, suffix, fixed app identity and separate media-ID binding presence. No URI, random relay token, signed query, media ID, native errorText or unknown JSON key is printed. Numeric errorCode is retained when present in error JSON. Callbacks have header-derived names and a bounded first-event JSON shape; periodic information is not logged per frame. String payloads remain redacted. Load return 1 is explicitly logged with readiness=await-callback and network=unproven.

### One precise hardware comparison

The new **development-only** direct-uri-probe executable and launcher require all UxAir receivers closed. It creates one SDL window and serial native sessions, stops at the first PLAYING candidate (also requiring real relay requests/fetches/bytes for loopback cases), holds that candidate for six seconds for visual/audio observation, and exits. Each case has the normal startup deadline; the entire diagnostic process has a 120-second hard deadline, including blocking proprietary calls. Back/Exit abort normally. It never starts RAOP or reads/writes settings, pairing or History. Only its separate bounded diagnostic log is written. No production process is stopped.

| Case | Native Load JSON | Native URI target |
| --- | --- | --- |
| A | Existing args/HLS + foreground | Loopback HLS relay |
| B | uMS URI envelope, HLS | Loopback HLS relay |
| C | uMS URI envelope, URI | Loopback HLS relay |
| D | uMS URI envelope, HLS | Fixed PC HLS fixture directly |
| E | uMS URI envelope, URI | Fixed PC HLS fixture directly |
| F | uMS URI envelope, URI | Fixed PC progressive MP4 control |

Direct fixture targets are restricted in code to a numeric RFC1918 PC address matching UXAIR_DIRECT_LAN, HTTP port 8080 and exactly /master.m3u8 or /probe.mp4, with **no credentials, query or other path**. The regular AirPlay path cannot select this bypass. MP4 is generated by remuxing the existing fixture video/audio with -c copy; it is not a transcoding feature or a general MP4 playback claim. Cases D-F test the service's outbound path without opening a LAN relay port.

Update the checkout and install the new DEV IPK. Close the dev receiver (and close any running production receiver through its normal UI). Restart the updated PC fixture so /probe.mp4 exists:

```powershell
# PC terminal 1; leave running:
python tools/direct-hls-fixture.py --bind 192.168.1.89 --port 8080
# PC terminal 2, from this PR checkout:
ares-install -d b7 com.github.depius.uxair.dev_1.2.0_arm.ipk
ares-launch -d b7 --close com.github.depius.uxair.dev
scp tools/run-native-uri-probe-b7.sh root@192.168.1.27:/tmp/uxair-native-uri-probe.sh
```

The single test command is:

```powershell
ssh root@192.168.1.27 'sh /tmp/uxair-native-uri-probe.sh 192.168.1.89'
```

Report its sanitized output, the PC fixture's master/media/segment/mp4 status lines and the case with visible video/audible tone, if any. The fixture request log cap is 192; no URLs or request headers are printed. Return 0 means only a PLAYING callback candidate; 1 means no candidate, 130 an intentional abort and 137 a hard deadline. A timeout does not establish unsupported HLS.

Interpretation:

- A now requests/plays: missing foreground notification is implicated; only hardware video/audio confirms recovery.
- B/C requests while A did not: payload shape/transport handling is implicated. After picture/audio and controls pass, the matching explicit process flag `UXAIR_DIRECT_CONTRACT=uri-envelope-hls` or `uri-envelope-auto` can be used with the existing dev launcher. Neither changes saved settings nor enables discovery; unknown values retain args/HLS.
- Only D/E request: the native API can reach the PC but the loopback path is suspect. This is the evidence needed before considering a token-protected LAN listener; do not infer network namespace isolation solely from the symptoms.
- Only F requests/plays: generic URI handling works for progressive H.264/AAC but HLS initialization remains suspect.
- No requests in A-F: the remaining issue is the native API contract/service setup, foreground policy or unsupported URI implementation; it is not an upstream playlist/segment download failure. A B7 libplayerAPIs/uMS implementation trace would be needed to distinguish these. Do not invent required fields or claim unsupported firmware from this alone.
- Requests occur but no PLAYING: use native errorCode, video/source/resource JSON shapes and HTTP status to narrow demux, resource and ACB behavior. Even PLAYING does not verify visible video/audio or A/V sync.

### Compressed-stream fallback assessment

A fallback is feasible in principle: policy-controlled HLS retrieval -> FFmpeg HLS/MPEG-TS/MOV demux -> H.264 Annex B access units + timestamps -> the existing hardware BUFFERSTREAM Feed path; AAC can use the existing decoder/PCM path. Video should remain compressed, with no transcoder. Current pinned FFmpeg intentionally has avformat/network disabled, so this is **not an existing or completed fallback**. It would require enabling the limited demuxers/bitstream filter, a custom bounded AVIO layer that cannot bypass the relay's redirect/TLS/address policy, audio extradata handling, queue/seek/PTS/discontinuity tests and B7 A/V validation. Merely enabling FFmpeg HTTP would bypass security safeguards. This larger path is deferred until the native comparison establishes whether it is needed; firmware URI support has not been disproved.

## B7 loading failure follow-up (2026-10-22)

The owner confirmed TV-to-PC HTTP connectivity with BusyBox wget. Three native attempts reached play 200/loading, then stopped after approximately 15 seconds with playback-info 503; no picture/audio was observed. This confirms a native startup failure, **not its precise firmware cause**. Old logs printed only selected callbacks and failed fetches, so missing direct-player/hls-relay lines did not prove that the main loop or Load was skipped.

Concrete corrections:

- Relay URLs now preserve `.m3u8` for master/variant/audio playlists and safe media suffixes for TS/fMP4/AAC. Previously every path ended in an extensionless integer. Native HEAD probing is supported with correct Content-Length and no body. Playlist responses always have HLS MIME; TS responses use video/mp2t even when the Windows fixture supplies a text MIME. This removes demonstrable interoperability gaps; it does not establish that they caused the B7 timeout.
- URI playback registers the ACB VIDEO role instead of the MSE/buffer-stream role, and sets its initial window after Load, matching the working native call order. Mirroring retains its existing MSE path. The role distinction follows the public ACB definitions; its effect on this firmware needs measurement.
- Main-loop dequeue, relay startup, ACB creation/initialization, Starfish construction, media-ID registration, Load, window, sink, loaded-state, Seek/Play and cleanup each have credential-free begin/end/result diagnostics. Native operations include elapsed milliseconds, so a missing end identifies a blocking call. Numeric ACB callback events are bounded. Integer ACB returns are recorded without inventing undocumented success conventions; false boolean returns are failures.
- Every native callback is counted, including previously ignored source/resource events. Only the first occurrence of each type (at most 32/session) is printed, never its text. Dequeue records are capped at 192/process; native phase records at 128 pairs/session plus fixed cleanup transitions; relay fetch/request records at 64/session. One waiting summary appears after five seconds. Startup stays **15 seconds**, stalled progress 30 seconds; distinct errors identify load, initial-duration, playing, native, sink and control failures.
- Terminal errors preserve stage, native result, callback count/last numeric event, actual native relay requests, upstream fetches, downloaded bytes and failures. These appear in Diagnostics, normal feedback, and a diagnostic XML body on HTTP 503/error stopped events. Feedback no longer loses evidence when resources close. Control failures publish their terminal error once, after cleanup, preventing a retry accepted during teardown from being erased by a second terminal publication. A relay request counter counts actual native-facing requests, not a synthetic preflight.
- The PC client exits nonzero with a sanitized failure summary for 503, stopped-before-ready, malformed feedback or network loss. Failed stop cleanup cannot mask the first error or produce a traceback. Fixture request logs show only GET/HEAD, master/media/segment/mp4 role and HTTP status (first 192).

Primary references: [LG mediaOption](https://webostv.developer.lge.com/develop/guides/mediaoption-parameter) documents HLS/URI transport and extension/MIME detection; [native ACB declarations](https://github.com/webosbrew/webos-userland/blob/master/include/appswitching-control-block/AcbAPI.h) define VIDEO/MSE and return types; [Starfish API](https://www.webosbrew.org/webos-userland/d3/dda/StarfishMediaAPIs_8h_source.html) declares Load callbacks and Seek(millis). Duration/playtime values remain raw numeric observations in logs; their conversion to seconds is still an unverified firmware assumption.

### Minimal next retry

Use the install/fixture/dev-only launch commands below, without the Safari discovery argument. Use the receiver port from this launch; 45607 was the previous port, not a guaranteed fixed port. Run the control client once. Expected diagnostic progression is dequeued -> relay listening -> ACB/native Load begin/end -> native GET/HEAD -> fixture master/media/segment 200 -> load completed (0x16) -> Play/playing (0x1a). A different ordering is possible for asynchronous callbacks. Only moving picture **and** tone establish native playback.

If it fails, collect this one read-only command from PowerShell:

```powershell
ssh root@192.168.1.27 'for r in /media/developer/apps/usr/palm/applications /media/cryptofs/apps/usr/palm/applications; do p=$r/com.github.depius.uxair.dev/uxair.log; if [ -f "$p" ]; then grep -E "^\[(direct-player|direct-protocol|hls-relay)\]" "$p" | tail -100; fi; done'
```

Minimal evidence: picture/tone yes/no, the client's final feedback/error summary, fixture role/status lines and those bounded app log lines. In particular, Load accepted with **zero native relay requests** distinguishes firmware URI startup from an upstream fetch failure; master/media/segment 200 plus a native error points to demux/decode/ACB. No callback after accepted Load is reported explicitly. A begin without an end identifies the native call that did not return. Do not enable discovery or call Direct HLS fixed until B7 picture/audio passes, followed by stop/retry and existing mirroring/music recovery.

## Reproducible B7 test (only remaining physical evidence)

Install the new `.dev_1.2.0_arm.ipk` with the configured `b7` ares device. Do not uninstall it or reset state. Keep production UxAir idle while testing the shared TV decoder.

PC prerequisites: Python 3, FFmpeg with libx264/AAC in PATH, webOS ares CLI and an already configured rooted B7 SSH connection. Allow TCP 8080 only on the test LAN.

On the PC, in the PR checkout (using the confirmed PC 192.168.1.89 and TV 192.168.1.27 addresses):

```sh
python3 tools/direct-hls-fixture.py --bind 192.168.1.89 --port 8080
# In another terminal:
ares-install -d b7 com.github.depius.uxair.dev_1.2.0_arm.ipk
ares-launch -d b7 --close com.github.depius.uxair.dev
ssh root@192.168.1.27 'sh -s -- 192.168.1.89' < tools/run-direct-b7.sh
```

On Windows use `python` if `python3` is unavailable. Replace the final SSH command above with these exact PowerShell commands:

```powershell
scp tools/run-direct-b7.sh root@192.168.1.27:/tmp/uxair-direct-test.sh
ssh root@192.168.1.27 'sh /tmp/uxair-direct-test.sh 192.168.1.89'
```

This writes only a temporary test script, not firmware or production state. The foreground SSH terminal may stay quiet because the app writes its own log. Ctrl-C ends the test receiver.

Read the ephemeral port with this safe command (pick the application root found by the launcher):

```sh
ssh root@192.168.1.27 'grep "UxAir ready port=" /media/developer/apps/usr/palm/applications/com.github.depius.uxair.dev/uxair.log | tail -1'
```

If installed under `/media/cryptofs/apps`, substitute that root. With the reported port, run:

```sh
python3 tools/direct-control-test.py --receiver http://192.168.1.27:PORT --source http://192.168.1.89:8080/master.m3u8
```

Expected: reverse upgrade 101; play/control requests 200; loading/playing/paused/stopped events; moving 12-second colored video with a 440-Hz tone; duration about 12 seconds and increasing position; pause holds position, resume advances, seek moves near five seconds, stop returns to the UI. OK toggles Diagnostics and Back stops. Repeat twice, then check saved pairing, H.264/HEVC mirroring and AirPlay Music without restarting or clearing state. Exercise `/redirect`, `/missing-segment.m3u8`, `/bad.m3u8`, `/slow.m3u8` and `/loop` as source paths; failures must return an error and allow a subsequent good session.

Safe numeric diagnostics on the TV:

```sh
APP=/media/developer/apps/usr/palm/applications/com.github.depius.uxair.dev
grep -E '^\[(direct-player|direct-protocol|hls-relay)\]' "$APP/uxair.log" | tail -80
for p in $(pidof uxair); do
  if [ "$(readlink /proc/$p/exe)" = "$APP/uxair" ]; then
    grep -E '^(VmRSS|VmSize|Threads):' /proc/$p/status
    ls /proc/$p/fd | wc -l
  fi
done
```

Do not send full logs, pairing files, URLs or packet captures containing stream credentials. Minimal first report: picture/tone yes/no; sanitized control-client output; numeric event lines (especially duration 0x3); RSS and FD counts before/after two sessions; whether stop and subsequent mirroring/music recover. If SDL/ACB fails at launch, report only its bounded error stage.

**Only after the native fixture passes**, stop the dev receiver and relaunch explicitly with the second launcher argument:

```sh
ssh root@192.168.1.27 'sh -s -- 192.168.1.89 --safari-after-native-test' < tools/run-direct-b7.sh
```

PowerShell: `ssh root@192.168.1.27 'sh /tmp/uxair-direct-test.sh 192.168.1.89 --safari-after-native-test'`.

This explicitly enables experimental direct discovery for that process; it is never the default. On iPhone Safari open `http://192.168.1.89:8080/`, use the video's AirPlay output control and choose UxAir Dev. Fullscreen alone is not a route switch. Report events, sanitized method/response sequence and any unsupported action/property names as structure/types only. If an unknown schema blocks playback, that exact redacted request structure is the only additional protocol evidence needed; no separate Apple TV is required for this experiment.

## Automated evidence

Tests use actual libplist, socketpair reverse traffic, correlated FCUP responses, finite concurrency tests, real local HTTP redirects/404/oversize/timeouts/recovery and untrusted HTTPS certificates, plus simulated native callbacks (including stale generations). Existing full mirroring/audio/pairing/settings/History/UI regressions remain enabled. Native firmware video/audio, A/V sync, TLS on the B7 and Safari interoperability remain physical acceptance gates.
