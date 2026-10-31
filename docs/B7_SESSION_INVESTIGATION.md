# B7 session/RAOP investigation

Hardware baseline 2b56d99 starts, persists pairing/settings through reboot,
mirrors H264/HEVC including 4K/60 configurations, renders Now Playing without
bands and plays RAOP. The reported 72.3-second failure is a real three-second
Feed Retry recovery; zero decode errors does not identify its sink-side cause.

## Confirmed source findings

Starfish Feed returns Retry only for replies containing BufferFull or Pending.
Accepted PCM remains in pending with codec=0, so retries do not decode twice.
FLUSH clears queued audio and resets the decoder, but the late FLUSH path used
different cleanup than the early path. Both could discard the current packet
without resetting stall. A new packet could inherit that packet's three-second
deadline. This is a proven state bug, not proof of the reported firmware stall.
A callback copies outside the queue mutex; clear_audio_packets could finish
while that copy was underway, allowing old audio to be enqueued after FLUSH.

Both FLUSH paths now share cleanup/pause intent and reset the discarded audio
retry budget. Audio packets carry a receiver epoch, advanced under the queue
lock for FLUSH/clear. Enqueue checks the epoch under that lock, and main rejects
stale dequeued audio. This does not identify the transport connection that
originated a callback; a callback first entering after an epoch change remains
unattributed. No broad callback ownership redesign was attempted.

The main UI return is the expected finish/recovery behavior after timeout,
transport reset, media error, discontinuity or remote action. HEVC recovery can
change the runtime advertisement to H264/720p/30; other paths reduce FPS or
1080p to 720p. These fallback paths do not save settings. Runtime settings stay
changed for the run; reconnect need not restore the saved profile. A sender can
also select H264 under an HEVC-capable advertisement. No fallback was removed.

## Diagnostics and limits

Session finish/recovery and FLUSH include the local session counter. Feed stalls
log onset, recovery or timeout duration, queue packets/bytes and media state.
Starfish logs BufferFull versus Pending once per retry episode and pause requests.
Recovery logs runtime and loaded saved profiles. No pairing/key/identity fields
are added. Retry logging is bounded per episode; the existing rotation bounds
file size. The three-second timeout is unchanged.

Public reverse-engineered headers label 0x2b PF_EVENT_TYPE_INT_SVP_VDEC_READY:
https://www.webosbrew.org/webos-userland/d3/dda/StarfishMediaAPIs_8h_source.html
This is not a verified firmware-specific interpretation on the B7 and is not
an error in our switch. Correlation with the failure is not evidence of cause.

Starfish uses the same Media pointer as callback context across loads. Without
an established Unload callback-quiescence contract, obsolete asynchronous events
are a remaining hypothesis; local session counters cannot authenticate their
origin. No obsolete callback suppression is claimed or fake lifetime guarantee
introduced. Transport reset callbacks similarly lack connection identity. The
next trace should correlate their existing numeric reasons with session logs.
Photos have no separate implemented direct-photo pipeline; mode-switch behavior
must be established from the negotiated stream/codec logs.

## Validation and hardware matrix

Deterministic tests cover FLUSH retry-budget reset, strict timeout boundary,
old audio epoch rejection after FLUSH/clear, retained pending video during mirror
FLUSH, and audio-to-video yield. Existing tests cover saved settings, pairing,
media failure and profile recovery. These are not firmware callback lifetime tests.
Storage, codecs, advertisement policy and NDL implementation are untouched.

On B7: repeat Apple Music longer than five minutes, pause/seek/track-change
FLUSH/resume, audio -> mirror -> audio, HEVC/H264 reconnect, and Photos transitions,
both with and without explicitly stopping the sender. Capture current and previous
state/uxair.log. Look for feed retry class, duration and queue occupancy, transport
reset reason, session counter, negotiation and runtime/saved profile at recovery.
Confirm reboot pairing/settings persistence and the previously working rendering,
shutdown and 4K/60 modes. Firmware backpressure cause, true stale callbacks and
long-running audio continuity require physical testing; CI cannot establish them.
