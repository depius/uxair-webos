# B7 mirroring disconnects at the audio packet limit

The November 13, 2026 TV log records three HEVC session resets. All hit the shared queue's 512-packet bound on audio ingress. The profile's byte ceiling is 16,777,216 bytes.

| Generation | Duration | Bytes at reset | Audio / video packets at last periodic sample |
| --- | --- | --- | --- |
| 1 | 941.4 s | 1,475,525 | 173 / 28 (line 1768) |
| 2 | 377.6 s | 492,579 | 291 / 20 (line 2867) |
| 3 | 755.4 s | 425,874 | 346 / 17 (line 4614) |

The periodic samples precede the exact capacity events. They show audio-dominated occupancy, while the capacity record identifies the incoming packet type. The log does not measure the split at the exact overflow. Generation 3 had 397 queued audio packets and 17 video packets at line 4586, with 0.80 audio feeds per second. At lines 4557 and 4571, video was feeding near 60 FPS and audio Feed was zero with 173 audio packets queued. Other zero-Feed windows contained only video. Audio later recovered to about 92 feeds per second. There was no Feed stall or FLUSH in the three failed sessions. Generation 1 overflowed just after the seventh HEVC reopen retained its keyframe for the first-feed delay. Generations 2 and 3 overflowed with the decoder ready and playing. A zero Feed rate can also mean that no audio arrived or no PCM was produced.

UxPlay turns RAOP RTP timestamps into remote NTP timestamps and passes audio and video from separate threads. The receiver adjusts audio PTS by the configured delay, copies the packets, and enqueues both types under the same mutex. The consumer selects the earliest eligible PTS across both types. It leaves future audio in the queue while due video can continue. Pending Feed retries and decoder Load or first-feed waits can also defer dequeues. The reported Feed counters measure accepted buffers, so they cannot by themselves establish dequeue starvation. The reported lock waits do not support lock contention as the primary cause. Old logs did not capture audio PTS eligibility or dequeue rates, so the exact source of each audio buildup is unresolved. The source and logs establish why these audio-heavy periods could disconnect the whole session.

## Queue and reset behavior

At Starfish mirror startup and geometry reinitialization, remove queued audio with PTS before the new origin. The main loop already discards these packets because they have negative decoder PTS. Reject later arrivals that fall before the same origin. The FLUSH epoch still invalidates in-flight audio independently of video.

At the packet-count bound, a Starfish mirror that accepted video in the previous second can shed future audio while unpaused. Among queued and incoming future audio, prefer to discard the farthest-future packet. An incoming video packet can replace a queued future audio packet. This keeps due audio, video access units and the ordering and PTS of retained packets. The recovery policy can cause audible gaps during pressure. Byte overflow, oversize packets, a full queue with no safely replaceable packet, and audio-only overflow still take the existing failure path. NDL uses the original queue policy.

The old `reset_events` count included every rejected packet and callback. Counts of 5, 63 and 171 do not imply that 171 sessions reset. In the first failure, a second capacity notice appears between reset consumption and ingress closure. The consumer now closes ingress and advances the epoch atomically when it claims a reset. Additional origins are coalesced before that point. The pinned SETUP code records the epoch before starting RTP. Media ingress, RTP reset and state callbacks, FLUSH and sender RTP TEARDOWN carry that epoch, so a retired stream cannot stamp a late reset with a new session's epoch.

`[queue-flow]` reports audio and video counts and bytes, oldest packet age, earliest audio PTS relative to the scheduler, enqueue/dequeue rates, audio high-water marks, shed packets, latest accepted audio Feed age and current pending state. `[pipeline]` and `[queue-video]` retain total and video high-water marks. Capacity logs distinguish the packet and byte bounds. `[health]` reports observed reset events, effective notices and suppressed requests.

## GStreamer warnings

The log contains the GStreamer assertions during Starfish Load, including successful reinitializations followed by sustained playback. UxAir calls the Starfish player API and has no direct GStreamer calls. The existing ELF verification rejects direct GStreamer linkage. The timing is consistent with the vendor media stack emitting the warnings in the UxAir process. There is no firmware call stack to locate the exact origin, and the logs do not connect them to the packet-limit resets.

## Verification and remaining work

The accelerated queue simulation models 60 FPS video and simultaneous audio, silent runs, delayed audio eligibility and repeated PTS origin changes. It exercises the production enqueue/dequeue implementation, including audio shedding at 512 packets, retained due audio and video, separate byte overflow, FLUSH, normal sender TEARDOWN and recovery with a new epoch. A concurrent reset test sends 256 requests from eight threads and checks that the generation closes once. Existing tests exercise HEVC preroll, repeated geometry changes and standalone RAOP behavior.

On Windows, the targeted queue-pressure, session lifecycle, ingress, HEVC reopen, audio-only and generated TEARDOWN tests passed with MSVC AddressSanitizer. Production and development lifecycle generation passed against the pinned upstream source. The Linux ASan/UBSan suite and ARMv7 build remain to be run on a supported Linux or macOS environment.

A multi-hour B7 test with repeated rotations, silence and audio restarts is needed to establish whether the reported disconnects are resolved on hardware. The new queue diagnostics distinguish future PTS, stalled dequeue and Feed rejection if a failure recurs.
