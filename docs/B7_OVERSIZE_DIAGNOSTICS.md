# B7 oversized-ingress diagnostic build

Base: 318f686190d6306a21bccb43a0db7a17324014ab. Diagnostic-only changes:
packet limits, 512-packet queue, byte capacity, feed deadlines, parser decisions,
reset masks/classification and media backend implementations remain unchanged.

## Records and counter semantics

Each oversized ingress rejection produces exactly one fixed-format [ingress]
record. It contains kind, incoming_bytes, limit_bytes, video_codec, packet_codec,
epoch, generation, generation_active, queue_packets, queue_bytes, closing and
accepting. There are no payload bytes, addresses, pairing data or media titles.
The line length is bounded by fixed labels and numeric field widths; records
are not rate-suppressed, so every oversize rejection is represented.

The generation field is the last successfully started main-loop session (0
before the first). generation_active=0 means no session has started yet or the
previous session is closing: use epoch to correlate rejections during Load.
It is not an upstream connection identity. A late callback entering after a new
admission still cannot be attributed to its original connection with this API.

The ingress guard still rejects video larger than its existing profile limit
(2097152 bytes for 1080p/4K, 1048576 for 720p) and audio larger than 1048576 bytes.
It still sets Oversize (sources=16), drops the packet and ends the session through
the existing reset handling. The rejected packet never contributes to queue bytes.
Neither the oversized payload's validity nor its media kind is presumed.

reset_events counts every reset callback and every capacity/oversize rejection,
including events coalesced into one pending reset notice. It is not a count of
unique session endings. Counter updates do not alter notification coalescing.

A [session-close] record is emitted before media cleanup. Its epoch is captured
before closing invalidates ingress. reset_origin/reset_sources/reset_epoch and
reset_pending identify the reset consumed by the main loop; none/0/pending=0
means finish was entered through another path (for example remote Exit).
video_packets_accepted_epoch counts video callbacks actually inserted into the
queue since receiver.clear(), including pre-Load packets and receiver-preserving
handoffs. It is not the number of decoded or displayed pictures.
video_feed_accepted counts video access units accepted by media.feed during this
session, including decoder reinitializations. decoder_frame_callbacks sums the
Starfish frame-event counter across decoder instances, sampled at retirement and
closing; it does not prove display. NDL reports decoder_callbacks_available=0.
Displayed frames remain explicitly unmeasured. Counters do not affect playback.

## Validation

The sanitizer suite links src/receiver_ingress.cpp, the same code as production,
with opaque network handles and a deterministic test clock. It verifies exact
limits and limit-plus-one for audio/video, repeated rejection logs and counters,
mask 16, unchanged capacity accounting, closed ingress, copies invalidated during
closing/re-admission, audio FLUSH epoch invalidation, and callback accumulation
across decoder reinitializations. A subprocess checks every rejection record's
fields and fixed bound. Existing lifecycle, HEVC and storage tests still run.

## Physical LG B7 reproduction

1. Upgrade the installed production app with the new webOS 3-4 IPK. Do not uninstall,
   clear state or reset pairing/settings. Close the old process before upgrading.
2. Launch from the TV launcher; confirm saved 3840x2160, HEVC, 60 FPS loads.
3. Repeat the same sender/content/action sequence that caused generations 5 and 6.
   Record the user action immediately before each failure and whether any usable
   picture appeared. Do not infer pictures from Starfish PLAYING alone.
4. For comparison: stop mirroring normally, reconnect, then alternate portrait and
   landscape, a static screen and moving content. Repeat at least five reconnects.
5. Save the complete state/uxair.log (and uxair.log.previous if rotated) immediately
   after reproduction, before further sessions overwrite it. Correlate [ingress],
   [session-close], [session] reset and Starfish events by epoch and generation.
   Include incoming_bytes, kind, limit_bytes, generation_active and callback/feed
   counts. Note exact sender actions and iOS version separately.
6. Verify normal sender stop remains error=0; oversize remains sources=16/error=1.
   Confirm music/photo coexistence, remote Exit and saved profile/pairing survive.

Root-shell log extraction (read only):

    cat /media/developer/apps/usr/palm/applications/com.github.depius.uxair/state/uxair.log

No new build fixes the oversized-packet failure: this build establishes what the
existing guard rejected. Size changes, packet dropping/recovery policies and
connection-attribution changes require subsequent evidence and separate review.
