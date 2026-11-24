# UxAir Dev Direct Video · Phase 3

## Hardware evidence

The B7 trace `uxair-dev-phase2-test2.log` confirms verified Pair-Verify, SAP, authenticated envelopes, AES-GCM records and real Safari `POST /play` and `POST /reverse`. The old receiver returned 501 for both. One listener run saw 48 upgrades, 84 encrypted requests, three admission attempts and zero admissions. The trace has neither decrypted media fields nor evidence of audio frames entering the playback pipeline. It shows a concurrent realtime RAOP SETUP with `usingScreen=0` and `isMedia=1`. This establishes negotiation only. [Transport architecture](ENCRYPTED_VIDEO_CONTROL_PHASE2_1_4.md)

The pinned sender upgrades a separate encrypted reverse connection to PTTH/1.0 and acknowledges its event requests over that stream. It sends a binary-plist `/play` with URL, optional start position, session, rate, stream/media type and item UUID. The iPhoneMirror receiver demonstrates a separate media broker for `mlhls`. UxAir reuses its existing Go cryptographic transport and native receiver and does not add or redistribute another broker. [Pinned go-atv sender](https://github.com/jkiddo/go-atv/blob/f47a3ffcfa9458aac40562d73c314de1447df013/airplay/fairplay_control.go) · [Pinned iPhoneMirror receiver architecture](https://github.com/RayrenSX/iPhoneMirror/blob/5e4443ceac899a3b0a7df4d92696cefc01673c6a/third_party/airplay-control/SOURCE.md)

## Encrypted playback and reverse events

- The encrypted dispatcher submits a validated media descriptor to the existing DirectSession command queue. The binary and XML plist handlers take the URL and optional starting position, validate the declared session when present and track the item UUID for seek. Stream and media type fields are checked structurally. The initial supported rates are zero and one. Legacy text parameters remain accepted. The acknowledgment means the UI thread can take the admission. Loading, backend readiness and playback progress have separate observations.
- The owner is the combination of the already authenticated controller signing key and the negotiated session alias. The session alias alone cannot authorize another registered controller or an unencrypted request. A command socket's loss queues Stop. No raw video URL fetch is authorized without verified pairing.
- Encrypted `/reverse` checks the PTTH/1.0 Upgrade and Connection headers and empty request body. The encrypted 101 is sent on the existing transport with the same directional counters. The listener sends serialized `POST /event` state updates on that socket. It expects an empty encrypted HTTP 200 acknowledgment before the next event. The listener owns socket I/O and cryptographic state. Its queue is capped at 16 and acknowledgment time is bounded at 5 seconds. A separate verified reverse socket can reconnect while the command socket remains active. Legacy plaintext `/reverse` is rejected.
- `/playback-info`, `/rate?value=0|1`, `/scrub`, the supported default `/setProperty` assignments, and `/stop` share the verified ownership. Seek requires backend-reported seekability. The implementation preserves the existing explicit rejection of unsupported `selectedMediaArray` values. The terminal state and backend diagnostics are available to the current playback session.

## Media path, audio, and limits

The first supported path is a public clear HTTP(S) HLS URL with compatible H.264 and AAC. The existing loopback HLS relay rewrites media references, checks outbound IPv4 addresses at each actual curl connection, validates TLS and bounds redirects. Encrypted admissions disable the configured LAN exception and reject private IPv4 literals before queuing. Starfish receives the scoped loopback URL. Native URI runs first in auto mode, with one fallback to the existing bounded H.264/AAC MPEG-TS VOD BUFFERSTREAM path if native startup is unavailable or makes no progress. The fallback reuses its playlist resolver, rendition selection, bounded demux queue and common audio/video timestamp origin. The UI uses the existing fullscreen video and Back/Stop handling. Public redirects and media references remain subject to the same connection policy.

The media path uses embedded audio. A negotiated RAOP stream that has delivered no packets is allowed to remain connected during encrypted Direct Video. An active RAOP stream blocks admission, as does mirroring. Late RAOP audio is excluded while the embedded session owns output. This selects one audio pipeline and preserves standalone RAOP when Direct Video is inactive. The physical trace does not establish whether Safari requires a different audio association on some media.

`mlhls` receives 415 with a structural `resource-bridge-required` diagnostic. Progressive MP4 receives 415. The phase does not implement an encrypted FCUP resource broker, commercial DRM, separate audio rendition fetching in BUFFERSTREAM, or Safari's unobserved control formats. The older trace did not determine which media source Safari sent. Use the next structural admission result to identify whether a broker is required for a specific sample.

Playback logs distinguish admission, selected audio path, backend load, frame callback and accepted audio feed. Feed acceptance is evidence of submission to Starfish. Audible synchronized sound remains a TV check. Reverse counts and the active encrypted-connection count appear in the listener summary, and playback observations appear in the DirectSession summary.

## Validation and retest

The native fixture uses the real TCP listener, independent authenticated sender and a deterministic simulated backend. Tests cover binary-plist admission, invalid bodies and private addresses, reverse upgrade, encrypted event acknowledgments, reverse reconnect, registered controller isolation, live and dormant RAOP behavior, playback feedback, pause/resume, seek, stop, simulated backend failure and re-admission. Existing test suites cover the HLS relay, real demux and PCM feeds with backend stubs, native startup/fallback, mirroring and cryptographic rejection paths. Proprietary Starfish runtime testing requires the LG B7.

1. Install the verified Starfish Dev IPK over `com.github.depius.uxair.dev` and launch from TV Home. Confirm menus, Magic Remote, discovery and PIN pairing if requested.
2. Open the public Apple BipBop HLS sample in iPhone Safari and use the player's AirPlay icon to select UxAir Dev. Check fullscreen moving picture and audible synchronized sound. Pause, resume, seek if offered, and stop. Confirm the menu returns. [Apple HLS samples](https://developer.apple.com/streaming/examples/) · [BipBop HLS playlist](https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_16x9/bipbop_16x9_variant.m3u8)
3. Verify H.264 and HEVC mirroring, standalone AirPlay audio and repeated reconnect. Stop UxAir Dev to flush the listener summaries, then retrieve the log.

```powershell
$tv = Read-Host 'LG B7 IP address'; scp -O "root@${tv}:/media/developer/apps/usr/palm/applications/com.github.depius.uxair.dev/state/uxair.log" ".\uxair-dev-phase3.log"
```
