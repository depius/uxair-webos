# HEVC reset/fallback investigation

Hardware baseline 39be759 improves RAOP, supports music with photo browsing,
mirroring audio, and video audio takeover. A later video BufferFull episode
had 414 queued packets; transport reset followed, then invalid-parameter-sets
triggered the runtime H264/720p fallback. The saved preference stayed intact.

## Confirmed defects

HevcStream::inspect previously committed VPS/SPS/PPS only when the same callback
also contained VCL and a complete configuration. Valid header-only callbacks or
partial configuration were discarded. After finish() resets the cache, valid
slice-only AUs could therefore count toward 30 invalid startup AUs and lower
the runtime advertisement. This is a reproducible parser defect, not proof that
the actual sender used that sequence in the supplied trace.

The parser now caches individually parsed headers, rejects inconsistent known
ID references, and distinguishes incomplete configuration/non-VCL input from
malformed AUs. Changes remain latched until a VCL frame observes them. Initial
keyframes missing repeated headers receive the validated cached configuration.
Cache is still cleared on disconnect; old headers are never reused across it.
Incomplete startup waits use the existing 15-second media startup budget and
end with reconnect requested, retaining the selected profile. They no longer
count toward the malformed-AU fallback threshold. Malformed streams, unsupported
profiles/dimensions, actual Load failures and decoder recovery keep their safety
fallbacks. This does not automatically restore a previously lowered profile.

The receiver copied packets outside its queue lock. Audio epochs already guarded
FLUSH/clear, but an old video copy could enter after clear. A transport epoch now
invalidates both media types under the queue lock and is checked after dequeue.
This covers copies already in flight across clear; it cannot attribute callbacks
first entering after clear to an old connection. Firmware callbacks still share
a Media pointer; obsolete callbacks remain an unverified lifetime hypothesis.

## Causality and diagnostics

Feed BufferFull stalls the consumer while callback ingress continues. The queue
has a 512-packet limit independent of its byte limit, so 414 packets can grow to
an overflow below the byte limit. Capacity overflow sets the same reset flag as
transport callbacks. The existing trace cannot distinguish those reset causes.
New capacity logs identify packet/byte occupancy, incoming size and transport
epoch; reset consumption logs session, epoch, stall age and queue occupancy.
Startup diagnostics distinguish missing headers, malformed data and header reuse.

Repeated Load calls are explained by session startup, geometry changes and
parameter changes. False configuration-change detection is not demonstrated by
the supplied trace. GStreamer warnings occur inside proprietary Load processing;
this repository does not call those GStreamer functions. Their proximity to Load
is not proof of causality. No payload, decoder-preroll or 4K/60 scheduling changes
were made. Physical firmware testing is required to assess them.

## Validation and B7 follow-up

Deterministic HEVC fixture tests cover separate VPS/SPS/PPS callbacks, a following
keyframe without headers, prefixing that produces a valid fresh decoder input,
reset with missing configuration, malformed input, latched configuration changes
and exact wait-budget boundaries. Existing tests cover Main/Main10 limits,
geometry/reinitialization, keyframe gates and saved settings. Transport epochs
use the already-tested equality guard from audio; full connection/callback and
proprietary decoder behavior are not emulated by those tests.

On B7, upgrade without removing state. Test extended 4K/60 HEVC mirroring, repeated
music/photo/mirror/video transitions, reconnect after sender stop, and normal
remote Exit. Capture state/uxair.log and .previous including queue reset-cause,
transport reset reason, epoch, waiting-configuration and fallback reason lines.
Verify that incomplete-header reconnect retains the selected runtime profile,
that saved settings/pairing persist through reboot, and that RAOP/photo audio
continues to work. Long-run BufferFull cause, stale firmware callbacks, actual
sender header ordering and GStreamer warnings remain hardware questions.
