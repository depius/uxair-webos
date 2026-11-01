# B7 render scheduling and RTP lifecycle fix

Baseline: 558c552732774fe0f13f08b7f0d2d041260f3961. Full hardware log reviewed:
1423 lines, three HEVC RTP_SHUTDOWN callbacks, one audio capacity failure, a
later audio RTP_SHUTDOWN callback, then normal remote termination. No Feed retry,
Feed rejection, decoder error or HEVC fallback appears. Private media metadata
from that log is deliberately not copied here.

## Confirmed corrections

Now Playing previously measured its next redraw interval from before rendering.
A render longer than 33 ms could make every following loop render before its
single-packet audio service. A completion-based cadence now guarantees an
interval for packet service even with repeated dirty metadata. The main loop
refreshes its clock after rendering and before dequeue. The five-second window
reports draw count, mean/max render time, queued audio count/bytes, timestamp
range relative to session origin, range span, and time until the queue head is
due. It prints no media titles, keys or connection identifiers.

A deterministic 40 ms draw / 1 ms service / 125 packets-per-second model exceeds
512 queued packets with the former cadence; the corrected cadence stays below
32. The source scheduling bug is proven; renderer cost as the sole cause of
the physical capacity event remains a hypothesis until the new measurements.
No queue bound or Feed timeout changed; lookahead still respects packet PTS.

The pinned UxPlay raop_handlers.h has two RTP_SHUTDOWN callbacks in its sender
TEARDOWN handler. An always-on, exact-Git-blob-checked transform tags only those
sites through a small C-to-receiver bridge, retaining original numeric reason
and upstream cleanup. Production builds do not enable the development HTTP
probe. Development builds compose with their existing instrumentation.
Unattributed video callbacks, transport callbacks, capacity and oversized packets
remain failures. Sender-origin shutdown is normal completion; any same-epoch
failure takes priority when notifications coalesce. HLS transition callbacks
retain conservative failure behavior. Reset logs retain origin, reason and epoch.
No legitimate source notification is silenced. Notices already superseded by
teardown are logged as obsolete and do not terminate the next admitted session.

finish() closes ingress before statistics persistence or potentially blocking
Starfish/ACB close unless preserve_receiver is requested. Closing increments the
transport epoch; admission is rejected while closing, and copied packets are
revalidated under the queue lock. Disconnect leaves ingress closed until a new
admitted setup. The existing audio-to-video handoff preserves the receiver and
pending keyframe, so it does not close the gate. Receiver stop also gates ingress.
No storage, HEVC parser/header handling, Starfish payload, appearance or NDL
backend implementation changes are part of this fix. Shared lifecycle cleanup
applies to both backends.

## Validation

Local MSVC AddressSanitizer tests exercise slow-render starvation, FLUSH/resume,
100 repeated handoff/close/admit cycles, obsolete packet copies, failure priority
and untagged resets. A C test compiles the actual pinned TEARDOWN excerpt with
stubbed protocol dependencies, verifying audio/video/all-stream/HLS cleanup and
origin. Production source generation is tested against the pinned archive.
Full Linux CI runs these under ASan/UBSan plus the existing receiver, media,
HEVC and B7 storage suite. The real cross-build validates the complete linked
handler/bridge; the native C shim is not a network integration or firmware test.

## Limits and B7 test

The callback API still lacks per-connection identity. A callback that first
enters after a new setup is admitted cannot be attributed to the old connection
by these local epochs. Logs do not establish why iOS sent the prior TEARDOWNs.
The new sender classification distinguishes protocol origin, not user intent.
Rendering, incoming bursts and queued future timestamps must be correlated on TV.

Upgrade the Starfish IPK without deleting state. Exercise long scrolling titles
and fades for at least 10 minutes in Apple Music, pause/seek/track FLUSH/resume,
then repeat audio/photo/mirror/video transitions and 4K/60 HEVC geometry changes.
Capture state/uxair.log and .previous: now-playing-perf, reset origin/class,
queue capacity, obsolete-reset and generation/epoch lines. Expected sender stop
still unloads cleanly but logs finish error=0; real transport/capacity failures
retain error=1. Confirm normal Back/Exit, unchanged rendering, and pairing/settings
through reboot. Actual graphics duration, sustained queue stability, network
races and newer-TV NDL lifecycle behavior require physical testing.
