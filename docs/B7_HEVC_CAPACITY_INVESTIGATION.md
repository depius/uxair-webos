# B7 HEVC capacity investigation

> Historical investigation. See the [final B7 acceptance record](B7_FINAL_TEST.md) for current status.

Baseline: dc3fe475845acae0afa32d999f783e9a0e72a623. Input: the complete
uxair-capacity.log, inspected as diagnostic data, including interleaved stderr.
No private Now Playing titles or full user log are published here.

## Confirmed observations and source defects

Generation 11 ends at 882.3 s, epoch 20. Its final Load/reinitialization 22 to
996x2160 returns success (52.14 ms); the sampled queue is already 495 packets,
183 video and 312 audio, with video queue residence approximately 2.84 s.
The first 60354-byte video AU is accepted at Load age 202 ms. Audio ingress then
hits 512 packets / 8261976 bytes; ready=0, playing=0. This happens far before the
existing eight-second per-Load deadline. There is no Feed retry/stall or decoder
error to attribute it to. GStreamer warning output interleaves the reset log;
the reset class is still capacity (sources=8), not a sender shutdown.

Generation 12, epoch 22, starts normally with approximately 0.27 s video queue
residence. Reinitialization 23 to 996x2160 returns success (53.90 ms), and a
47512-byte AU is accepted at age 203 ms. It accepts 60 preroll AUs totalling
2395199 bytes, then waits for LOADCOMPLETED. The queue reaches 512 / 9854481
bytes and video ingress triggers capacity at session age 29.8 s. The per-Load
deadline has not expired. There is no oversize rejection, fallback, Feed stall
or decoder error. Reinitializations 24 and 25 in the following session succeed;
this is not evidence of a permanently exhausted decoder.

Two source defects are directly established:

1. Starfish schedule_start ignores arrival and returns now+preroll. HEVC reopens
   reset both PTS origin and wall-clock mapping to Load completion+200 ms. Every
   rotation therefore adds Load/close time and a fresh 200 ms to the buffered
   timeline. Consumption then continues at the sender rate, preserving the
   added debt. Generation 11's video residence rises from subsecond to about
   2.46 s before reinitialization 21 and 2.84 s thereafter, despite short feed
   calls and zero Feed retries. Audio bursts then exhaust the packet count.
2. At the bounded preroll limit, the single pending video packet is retained and
   the loop continues without dequeuing audio. This is application-level head
   of line blocking: no Feed call is active, so stall_ms=0 is expected. Whether
   this firmware's LOADCOMPLETED requires further audio is not established, but
   the code unnecessarily prevents that progress and buffers all ingress.

## Minimal correction

Starfish HEVC reopens preserve the previous sender-to-wall-clock mapping:
new_start = previous_start + keyframe_pts. A separate per-Load first-video
not-before deadline still retains the existing 200 ms delay. After it expires,
late queued media can catch up on the existing timeline instead of each reopen
adding permanent delay. Decoder PTS still restarts at zero on the validated
keyframe. Initial startup, the advertised 4K/60 profile and NDL scheduling stay
unchanged. No video is fed ahead of its due time or before the preroll deadline.

At the 60-frame/8-MiB preroll bound, one pending parsed video AU is held while
the existing queue can select due audio only. No further video reaches Feed
until readiness. On LOADCOMPLETED the held AU resumes unchanged; audio FLUSH
still invalidates audio copies, and transport epoch change/session finish also
retires the held video. The fixed 512-packet receiver queue and byte bounds are
unchanged. There is at most one held video AU plus the normal pending packet;
this finite pending storage is not an enlarged ingress queue.

Already queued audio/video with timestamps older than the new keyframe origin
continues to be discarded by the existing pts<0 path. Future packets from the
same live transport are preserved: they can legitimately follow the retained
new-configuration keyframe. Clearing the entire receiver or changing transport
epoch on geometry changes would discard valid future media and reset handoff
state; neither is introduced.

## Remaining hypotheses and recovery

The final two decoder instances have no recorded LOADCOMPLETED/PLAYING before
cleanup. That does not prove a lost callback or permanent firmware stall:
one is stopped at about 202 ms and the other before the eight-second deadline.
Media callbacks reuse a Media pointer and lack per-instance provenance; stale
callbacks remain a separate hypothesis, not a confirmed cause in this log.
The repeated GStreamer warnings also occur on successful instances and are not
shown to cause either reset. No speculative callback-lifetime rewrite or blind
retry/restart is added. The existing per-Load eight-second failure recovery and
Feed deadlines still apply; genuine capacity, decoder and transport failures
remain errors. A never-ready decoder can still fail at its existing limits.

## Deterministic validation

hevc_reinit_test.cpp links the production receiver ingress/dequeue code and
Starfish scheduling helper. A 60-FPS/100-audio-packets-per-second simulation
reproduces legacy accumulation to 512 packets, while the corrected mapping
peaks at 73 over 29 reopens. It tests the separate first-feed deadline, future
media scheduling, exactly 60 preroll frames, due-audio selection with a held
video packet, video retention, FLUSH, transport invalidation and the unchanged
startup timeout. The existing parser, startup, oversize, pairing, storage,
RAOP/lifecycle and UI tests remain part of tools/test.sh under ASan/UBSan.

## Physical validation and release hold

CI builds/verifies ARMv7 Starfish and records checksums, but does not upload the
IPK/source artifact. There is no new release or release candidate distribution.
A working source/host test is not confirmation of physical B7 playback.

Once the unchanged dc3fe475 artifact has a recorded independent/vendor malware
assessment, prepare one reviewed candidate for physical B7 testing. Preserve
state/pairing. Test the original content sequence for at least 30 minutes and
30 portrait/landscape changes with video+audio and repeated silent-to-audio
transitions. Check that queue residence does not grow on each rotation, the
first-video delay remains about 200 ms, due audio continues at the preroll bound,
and LOADCOMPLETED/PLAYING progress. Confirm music/photo coexistence, standalone
RAOP/Now Playing, remote Exit, sender TEARDOWN error=0 and reboot persistence.
Capture complete logs and actual visible/audible behavior, not just callbacks.

Branch closure and merging remain on hold: physical validation, separately
tracked security hardening and the browser Malware verdict are unresolved.
A clean Defender scan or a generic browser warning is not a vendor analyst
verdict. No case ID or written verdict was provided for ZIP SHA-256
cec8ec73fb66bd4c0f1abfd59e82fd0d1111a4aa7b02d6e5e9d6b3e8767ebb70.
