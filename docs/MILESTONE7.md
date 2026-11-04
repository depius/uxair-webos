# Milestone 7: codec profiles, HDR status and UI cleanup

> Historical development record. See [UxAir 1.2.0 scope](UXAIR_1_2_SCOPE.md) for current release and validation status.

Baseline: `89eb306908be460d2f1fa20e69a16c0cf90f9a44`. Target: LG OLED B7, webOS 3.9, ARMv7.

## Codec and resolution profiles

The user-selected codec controls the codec capability advertisement independently of resolution. H.264 offers 720p and 1080p at 30 FPS; experimental HEVC bit 42 is offered alongside H.264 at those sizes and 4K/30. DNS-SD and AirPlay capability data describe receiver support; they cannot force the sender to use HEVC or the advertised dimensions. In particular, the pinned UxPlay/RAOP implementation documents bit 42 as accepting H.265 *in addition to* H.264 and has no HEVC preference setting in its display plist. On the owner's B7, iOS chooses H.264 at 720p/1080p and HEVC at 4K with this advertisement. H.264 remains accepted as a safe sender-selected fallback to avoid reconnect loops. Logs report the selected capability mode, bit 42, sender-selected codec and negotiation outcome; diagnostics show actual incoming codec and SPS dimensions separately from the receiver limit.

If an H.264-only profile is selected and the sender insists on HEVC, negotiation is rejected so compressed HEVC is never passed to the H.264 decoder. The owner can select HEVC and reconnect. Selecting HEVC at 720p/1080p enables support but cannot force iOS to use it; this setting is now labeled as a capability, and the actual sender codec is visible. If HEVC fails after negotiation, existing bounded decoder recovery selects H.264 720p and asks the sender to reconnect. Changing from HEVC 4K to H.264 retains the highest supported H.264 resolution, 1080p; 4K is not retained for H.264.

## Main10 and HDR evidence

The Main10 SPS parser reports profile and bit depth, and the receiver rejects profiles other than the already supported Main 8-bit path before Starfish Load. The public [StarfishMediaAPIs header](https://www.webosbrew.org/webos-userland/d3/dda/StarfishMediaAPIs_8h_source.html) declares `setHdrInfo()` but provides no payload schema. The public [NDL video-info header](https://www.webosbrew.org/webos-userland/d2/d41/media-types_8h_source.html) includes HDR metadata fields marked “Only for HEVC V2”; this does not establish that those fields are available through Starfish on Rockhopper 3.9. No firmware-matched payload contract, decoder bit-depth query or output HDR measurement is present in this repository. Therefore the default-disabled `UXPLAY_HDR10_EXPERIMENTAL_ENABLED` flag remains false, Main10 stays rejected, and HDR is shown as unavailable. This milestone does not claim HDR support.

## Resolution diagnostics and UI

The four main sections are Video, Audio, Diagnostics and About. Video separates the selected codec and advertised resolution from incoming SPS. Diagnostics labels Starfish video-info, the requested ACB window, and panel/output-plane resolution separately; the last remains unmeasured. Incoming and Feed FPS are not display FPS. Audio retains the adjustable delay. Session history and its reset remain under Diagnostics. About identifies the build target and app-private receiver state.

The codec switch preserves the selected resolution when supported and changes HEVC 4K to H.264 1080p. Settings format and app-private paths are unchanged; paired keys and receiver identity are not modified by this change.

## Validation and limits

`tests/state_test.cpp` covers HEVC advertisement at each supported size, the H.264/HEVC resolution boundary, 4K-to-H.264 recovery selection and the disabled HDR flag. `tests/ui_navigation_test.cpp` covers four-section navigation, history return and existing remote-control focus behavior. The `armv7-ipk` workflow runs the host suite, cross-builds the development package, validates ARM ELF/IPK metadata and uploads the package. CI does not establish B7 decoder behavior, HEVC at 720p/1080p, native 4K output or HDR.
