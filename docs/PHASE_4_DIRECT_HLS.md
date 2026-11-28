# Phase 4.1 and 4.2 — LG B7 acceptance

## 4K and audio boundaries

The captured Phase 4 session identified HEVC Main (profile 1), level 5.0 (150). The old 1080p checks rejected it during HEVC configuration or access-unit validation under `unsupported-hevc-profile`. An explicitly declared 4K variant also failed master selection. No Starfish Load was reached.

The development path allows fMP4 HEVC Main 8-bit 4:2:0 SDR up to 3840×2160. Resolution above 1920×1080 is capped at 30 fps, checked against each fragment's decode cadence before submission. The existing hvcC level ceiling (5.1), parameter-set checks, HDR signaling rejection, 20 Mbit/s declared master cap, 8 MiB resource/normalized segment bound, 2 MiB access-unit limit, and 1,152 packet limit still apply. A long high-bitrate segment can hit the byte limit even when its variant declaration passes. Starfish receives the actual input geometry and the standard 1920×1080 ACB canvas. This documented geometry contract still needs physical validation at 4K.

Muxed clear fMP4 accepts AAC-LC, HE-AAC v1/v2, FLAC, ALAC, Opus, AC-3, and E-AC-3 when the demuxer recognizes the expected configuration. Opus in fMP4 uses the `Opus` sample entry. Apple does not list Opus in its current HLS authoring codec table, so sender and Safari interoperability need separate validation. MPEG-TS retains the existing AAC-LC/MP3/AC-3/E-AC-3 path. The added lossless and Opus support targets fMP4. The decoder uses the reconstructed audio layout and sample rate, resamples to stereo 44.1 kHz S16LE, and uses a sample-count PCM clock. Decode failures use the existing video-only fallback with an explicit reason. [Apple HLS authoring specification](https://developer.apple.com/streaming/hls-authoring-specification-for-apple-devices.html) · [Opus in ISO BMFF](https://www.opus-codec.org/docs/opus_in_isobmff.html)

## Reliability findings

- The Phase 4 trace shows eight `initiator=sender-rate requested-rate=0` transitions. Confirmed receiver behavior is to honor that request. Several runs resume on a later sender command. The trace does not establish why Safari sent Pause.
- Both captured `receiver-busy` responses show a stopped media session with one live transport claim. `uxair_transport_closed` releases bare reservations after the last owning socket closes. No stale-after-close condition was established. Keep session ownership and repeat tests with explicit AirPlay disconnect and reconnect.
- Nine reverse-channel rejections show `no-event-outstanding`, including before the first event and after previous ACKs. The dispatcher records this under `reverse-ack-framing`. These diagnostics establish that no ACK was pending. They do not identify the extra reply's semantics or whether it caused a later Pause. Existing encrypted ACK validation remains in force.

## Physical acceptance

1. Install the Starfish DEV package with ID `com.github.depius.uxair.dev` and launch it through the TV launcher.
2. From Safari, AirPlay a known clear 3840×2160/30 Main 8-bit SDR fMP4 HLS clip with visible motion and audible markers. Verify moving picture and synchronized sound. Capture demux geometry, cadence, tier/level/depth, BUFFERSTREAM Load/Play, first accepted feed and first-frame callback. A callback alone cannot establish picture or sound.
3. Play at least 90 seconds, seek backward and forward, pause/resume, then switch directly to 1080p60 HEVC and H.264/AAC. Capture queue bytes, retries, frame progress and terminal states.
4. Test HE-AAC v1/v2, FLAC, ALAC and Opus from a sender that can select the rendition. Confirm PCM output and sound, including stereo separation on the PS file. Record any sender-side Opus rejection separately.
5. Try a clear Main10 or HDR fixture, then replay SDR. Disconnect, reconnect, and check that RAOP audio-only and normal H.264/HEVC mirroring still work.
