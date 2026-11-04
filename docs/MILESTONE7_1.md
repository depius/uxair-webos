# Milestone 7.1: HEVC selection at 720p and 1080p

> Historical development record. See [UxAir 1.2.0 scope](UXAIR_1_2_SCOPE.md) for current release and validation status.

Baseline: `1270497a84f68e791bce6f882d2b9144c4617c49`. Target: LG OLED B7, webOS 3.9, ARMv7.

## Root cause

The B7 test confirms that enabling experimental HEVC while advertising 720p or 1080p still results in an H.264 stream from iOS. This is not caused by the receiver callback rejecting H.264: `video_set_codec` accepts H.264 in both modes.

The pinned UxPlay RAOP code recognizes the sender's `hvc1` codec configuration, and AirPlay feature bit 42 is named `SupportsScreenMultiCodec`. Upstream UxPlay describes its `-h265` switch as enabling H.265 *in addition to* H.264 for 4K/HEVC video. RAOP `/info` advertises display dimensions and frame rate, but its display entry has no codec-preference field; `raop_set_plist` likewise has no codec option. The available receiver-side protocol controls therefore advertise capability, not a preferred codec. The sender chooses the codec before the callback reports it.

The precise iOS decision rule is private and was not inferred from one receiver-side test. What is verified on this B7/iOS combination is H.264 at 720p and 1080p when bit 42 is set, and HEVC at 4K with the same bit set. Rejecting H.264 in the callback would make RAOP reset the session; the bundled code does not renegotiate HEVC after that reset, so rejecting it risks connection loops without changing the sender's choice.

## Change

The receiver continues to advertise bit 42 only in HEVC-capable mode and keeps resolution selection independent. H.264 remains accepted in that mode as the sender-selected fallback; HEVC remains rejected when the user chooses H.264-only mode. Codec negotiation is now expressed as a small tested policy, and logs identify the configured mode, bit 42, sender-selected codec, result and resolution limit.

The UI calls the setting a codec capability, says that iOS currently selects H.264 at 720p/1080p on the tested B7, and shows the negotiated codec separately. It does not describe HEVC as forced or claim HEVC playback at those resolutions. HEVC 4K/30 and existing H.264 playback are unchanged.

## Validation

Focused regression coverage checks H.264-only and HEVC-capable negotiation at 720p, 1080p and 4K, including H.264 fallback and rejection of HEVC when HEVC advertisement is off. Host tests and the ARMv7 IPK workflow validate software behavior and packaging; neither can verify a sender's hardware codec choice.

## B7 verification

1. H.264-only mode at 720p and confirm the Diagnostics page reports H.264.
2. H.264-only mode at 1080p and confirm H.264 playback remains stable.
3. HEVC-capable mode at 4K/30 and confirm HEVC and the existing Feed/Starfish playback path.

The known H.264 selection at HEVC-capable 720p/1080p is documented behavior for this receiver/sender combination, not a reported HEVC success. A later protocol discovery or iOS behavior change can be tested separately.
