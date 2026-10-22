# UxAir 1.2 Direct Video development status

This branch implements an experimental native HLS URI backend and a bounded AirPlay command adapter. It is **not complete Safari support**. DNS-SD direct-video/HLS bits remain disabled. No stable release is published.

## Source evidence and architecture

The pinned UxPlay 1.74 commit 9f3c2bbc exposes on_video_play/scrub/rate/stop/acquire_playback_info callbacks, HTTP handlers and FCUP/reverse playlist infrastructure. UxAir previously supplied none of these callbacks and disabled HLS routing. Current upstream HEAD inspected: 0477d47; no dependency pins were changed. Upstream README documents YouTube HLS rather than general Safari compatibility: https://github.com/FDH2/UxPlay . Its subsequent changes span HTTP parsing, playlists and RTP; wholesale upgrade would also change the stable mirroring path.

The development source transform intercepts selected direct commands before the disabled HLS gate, only with UXAIR_DIRECT_VIDEO=1. The adapter uses pinned libplist for binary/XML dictionaries and parses legacy text parameters. A mutex-protected queue caps pending commands at eight; bodies at 16 KiB and URLs at 4096 bytes. Only the main loop calls Starfish/ACB. It rejects starts while mirroring/audio owns the decoder, and rejects a different session owner. BUFFERSTREAM is untouched. Unsupported reverse/action commands return 501 rather than success.

DirectPlayer uses a separate Starfish instance with HLS mediaTransportType, URI and the development app ID. ACB owns the main video plane. The native player is responsible for demuxing, audio/video sync, variants and downloads. Seek uses milliseconds; initial Start-Position is a fraction and waits for duration. Loading has a 15-second state deadline. Back stops and releases native resources. Playing time (excluding paused/loading intervals) records an existing Direct recent-session/lifetime entry. Daily charts still have only mirror/music aggregates: a bounded V3 history extension and three-category charts remain outstanding.

Primary native API evidence: https://www.webosbrew.org/webos-userland/d3/dda/StarfishMediaAPIs_8h_source.html . LG documents native HLS on webOS 3 at https://webostv.developer.lge.com/develop/guides/mediaoption-parameter . Neither establishes B7 payload compatibility or Safari interoperability.

## Explicit blockers / unfinished acceptance items

- Safari /reverse events, FCUP /action and property exchanges are not implemented in this adapter. Exact current iOS requests require capture on the B7; synthetic plist tests cannot establish Safari compatibility. Receiver advertisement stays disabled.
- Native HLS URI Load, callback duration/playtime units, aspect ratio, HTTPS trust and MPEG-TS/fMP4 handling are hardware-unverified. Native redirect/DNS/response/segment limits cannot be enforced by this adapter. No HTTP downloader, manifest sanitizer or TLS override was added. Native memory use and cleanup blocking must be measured before enabling discovery.
- URL validation rejects non-HTTP schemes, credentials, control bytes, loopback literals, link-local literals, IPv6 and malformed ports. It does not prevent DNS rebinding or redirects to local services; private IPv4 is intentionally allowed for lab media. Opt-in testing must use a trusted isolated LAN. Signed URLs, bodies and native callback text are not logged.
- Sender socket-loss notifications are not yet mapped to direct session ownership. Diagnostics, network timeout recovery, master/segment error fixtures, three-category daily history and automatic mode switching are incomplete. There is no claim that all HLS formats, 4K or DRM work.

## Reproducible first B7 gate

1. Install only com.github.depius.uxair.dev_1.2.0_arm.ipk. Stop that development receiver, then launch its binary manually with UXAIR_DIRECT_VIDEO=1. Keep the production app stopped during this experiment; its state is never touched. Read the ephemeral HTTP port from the receiver log.
2. On a trusted LAN computer generate unprotected H.264/AAC HLS using FFmpeg (testsrc + sine, 20 seconds, libx264 yuv420p, AAC, -f hls -hls_time 4 -hls_list_size 0), and serve the output with python3 -m http.server 8080 --bind the computer's LAN IPv4. A Safari page can use <video controls playsinline x-webkit-airplay="allow" src="stream.m3u8"></video>. Fullscreen alone is not a direct-route test.
3. POST /play to the receiver port with X-Apple-Session-ID: lab-session, Content-Type: text/parameters and CRLF body Content-Location: http://LAN-IP:8080/stream.m3u8. Verify picture/audio, LOADCOMPLETED/PLAYING/EOS and bounded memory. POST /rate?value=0 then value=1; /scrub?position=5; GET /playback-info; POST /stop. Repeat, then check normal mirroring/music and pairing.
4. If native playback passes, capture a genuine Safari AirPlay Video exchange with a separate known compatible receiver on the trusted LAN. Record request methods/content types, redacted structure and event order, never PINs, credentials or signed URLs. Use this evidence to implement reverse/FCUP/session teardown before enabling advertisement on this B7.

Host tests use real libplist and simulated firmware; they validate parsing, command/state ownership, queue bounds and native adapter calls, not HLS network playback. ARM build and ELF/IPK validation establish binary/package compatibility only.

A ready-to-run fixture is included: `python3 tools/direct-hls-fixture.py --bind LAN-IP`. It generates 12 seconds of 720p H.264/AAC MPEG-TS HLS into a temporary directory, serves relative master/media/segment URLs and a Safari HTML5 page, then deletes all generated media when stopped. Open `http://LAN-IP:8080/` on the iPhone. `/redirect` exercises a relative HTTP redirect and `/missing.ts` a 404. This does not validate firmware playback automatically.
