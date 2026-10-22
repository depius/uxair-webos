# UxAir 1.2 Direct Video: implementation and B7 acceptance

PR #9 implements a separate native Starfish HLS URI player, PTTH event transport, a bounded HLS relay, and Direct Video History/Diagnostics. Hardware playback and contemporary Safari compatibility are **not yet verified**. Default discovery and the production 1.1.0 application are unchanged. No stable release or merge is authorized.

## Protocol and lifecycle

The development-only pinned-source transform routes `/play`, `/reverse`, `/action`, `/playback-info`, `/scrub`, `/rate`, `/stop`, `/info`, `/server-info` and property operations. Binary/XML dictionaries and legacy play bodies use pinned libplist; play bodies are capped at 16 KiB, action envelopes at 512 KiB, URLs at 4096 bytes, commands at eight and pending reverse messages at sixteen. PTTH returns 101 using the existing UxPlay EVENT connection type. A separate bounded writer sends loading/playing/paused/stopped/error events without firmware calls from RAOP threads. The connection destruction hook stops its owning session; short-lived control sockets do not stop playback. Legacy clients without a reverse connection have a 30-second control lease.

FCUP `unhandledURLRequest` follows the pinned UxPlay schema. `/action` accepts only correlated request ID + owner + URL responses, validates status and limits decoded manifest data to 256 KiB. It provides a three-second manifest fallback for eligible failed m3u8 retrievals; segments remain subject to the relay policy. Playlist removal stops playback. Unknown action/property mutations return 501 instead of fake acknowledgments. Read-only position/duration/rate properties and playback feedback are implemented; loaded ranges are deliberately empty when unmeasured. Unknown playlistInsert variants, subtitles, DRM and property features require concrete sender evidence before extension.

Callbacks carry generation IDs in a bounded live registry; an unloaded player's callbacks cannot change its successor. No firmware API runs while a callback-state mutex is held. Only the SDL main loop owns native player operations. Relay cancellation precedes native unload. Stop, EOF, native errors, reverse connection loss and lease expiry release Direct ownership. Concurrent mirror/audio starts are rejected while Direct owns the decoder. Stop Direct before switching modes. Back stops; OK toggles live Diagnostics. Native video-info is forwarded to ACB and display windows preserve aspect ratio.

## Native HLS and network policy

Starfish receives only a random-token loopback relay URL, never a signed upstream URL. The relay rewrites master/media playlist entries and URI attributes, including separate fMP4 initialization files. HTTP/HTTPS, relative URLs and up to three redirects are supported. Each connection checks the actual destination IPv4: loopback, link-local, multicast and private networks are denied. A single explicit `UXAIR_DIRECT_LAN=PC-IP` exception enables the local fixture. Redirects and DNS changes cannot bypass that connection policy. IPv6, URI credentials and non-HTTP schemes are rejected. Proxy environment settings and netrc credentials are ignored.

The new pinned curl 8.23.0 uses pinned OpenSSL with certificate/hostname verification. A committed Mozilla CA bundle (2026-10-20, SHA-256 796f1389bfea13c0ed78f663b765baaff54a508e5483786bd6ece73c5e3e3cbe) is packaged with MPL notices. Downloads have three-second connect and twelve-second total deadlines, an eight-MiB segment cap, 256-KiB manifest cap and a catalog bounded to 4096 resources / two MiB of URLs. The relay is loopback-only with one active fetch and bounded client read/write timeouts. No media is written to app storage. Encryption/key playlists, byte-range HLS and content steering are explicitly rejected; TLS is never disabled. H.264/AAC SDR up to 1080p is the initial target. No 4K, all-profile HLS or DRM claim is made.

The native firmware still performs demuxing, decoding and A/V sync; its memory use and URI/Load behavior require B7 measurement. Actual duration/playtime event units and MPEG-TS/fMP4 playback require hardware verification. No host test proves these properties.

## History and Diagnostics

V3 stores a separate Direct coverage start, so legacy Direct lifetime totals are not misrepresented as known zero daily activity. Partial recorded coverage receives the existing * indicator. V3 adds Direct daily milliseconds/session counts and reads V1/V2 without erasing data or inventing prior Direct daily usage. Storage remains 64 KiB, 100 recent sessions and 365 daily entries. Played intervals are assigned to UTC day buckets across pauses, rather than incorrectly treating paused time as contiguous playback. Recent sessions, lifetime counters, three-color stacked charts, three-category duration percentages and Direct Diagnostics are integrated. Unknown codec remains unknown; no URL/credentials are stored in History.

## Source evidence / upstream comparison

Pinned UxPlay remains 9f3c2bbc (1.74). Its reverse EVENT ownership, FCUP schema and controls were inspected alongside upstream 0477d47; its YouTube/GStreamer playlist implementation is not wholesale imported into this native receiver. Source: https://github.com/FDH2/UxPlay . Native API declarations: https://www.webosbrew.org/webos-userland/d3/dda/StarfishMediaAPIs_8h_source.html . LG HLS options: https://webostv.developer.lge.com/develop/guides/mediaoption-parameter . These establish API/protocol evidence, not Safari/B7 acceptance.

## Reproducible B7 test (only remaining physical evidence)

Install the new `.dev_1.2.0_arm.ipk` with the configured `b7` ares device. Do not uninstall it or reset state. Keep production UxAir idle while testing the shared TV decoder.

On the PC, in the PR checkout (replace 192.168.1.10 and 192.168.1.20 with PC and TV addresses):

```sh
python3 tools/direct-hls-fixture.py --bind 192.168.1.10 --port 8080
# In another terminal:
ares-install -d b7 com.github.depius.uxair.dev_1.2.0_arm.ipk
ares-launch -d b7 --close com.github.depius.uxair.dev
ssh root@192.168.1.20 'sh -s -- 192.168.1.10' < tools/run-direct-b7.sh
```

On Windows use `python` if `python3` is unavailable. For PowerShell without input redirection, copy the launcher to `/tmp/uxair-direct-test.sh` with `scp`, then run `ssh root@TV-IP 'sh /tmp/uxair-direct-test.sh PC-IP'`. This writes only a temporary test script, not firmware or production state. The foreground SSH terminal may stay quiet because the app writes its own log. Ctrl-C ends the test receiver.

Read the ephemeral port with this safe command (pick the application root found by the launcher):

```sh
ssh root@192.168.1.20 'grep "UxAir ready port=" /media/developer/apps/usr/palm/applications/com.github.depius.uxair.dev/uxair.log | tail -1'
```

If installed under `/media/cryptofs/apps`, substitute that root. With the reported port, run:

```sh
python3 tools/direct-control-test.py --receiver http://192.168.1.20:PORT --source http://192.168.1.10:8080/master.m3u8
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
ssh root@192.168.1.20 'sh -s -- 192.168.1.10 --safari-after-native-test' < tools/run-direct-b7.sh
```

This explicitly enables experimental direct discovery for that process; it is never the default. On iPhone Safari open `http://192.168.1.10:8080/`, use the video's AirPlay output control and choose UxAir Dev. Fullscreen alone is not a route switch. Report events, sanitized method/response sequence and any unsupported action/property names as structure/types only. If an unknown schema blocks playback, that exact redacted request structure is the only additional protocol evidence needed; no separate Apple TV is required for this experiment.

## Automated evidence

Tests use actual libplist, socketpair reverse traffic, correlated FCUP responses, finite concurrency tests, real local HTTP redirects/404/oversize/timeouts/recovery and untrusted HTTPS certificates, plus simulated native callbacks (including stale generations). Existing full mirroring/audio/pairing/settings/History/UI regressions remain enabled. Native firmware video/audio, A/V sync, TLS on the B7 and Safari interoperability remain physical acceptance gates.
