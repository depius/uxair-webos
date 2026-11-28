# Phase 4.4 — HDR and Dolby Vision research

## Evidence and limits

LG lists a native 10-bit panel and HDR10, HLG and Dolby Vision for the OLED55B7T. Its webOS 3.5 decoder table lists HEVC Main and Main10 in supported playback formats on UHD models. These are product and platform specifications. The physical UxAir B7 results cover Main 8-bit SDR Direct HLS through 1080p60 and 4K mirroring. Phase 4.1 sends 4K Main 8-bit SDR to the native BUFFERSTREAM interface in host tests. A proprietary input path can behave differently from LG's player for the same stream. [LG B7 specifications](https://www.lg.com/au/tv-soundbars/oled-tv/oled55b7t/) · [LG webOS 3.5 audio and video formats](https://webostv.developer.lge.com/develop/specifications/video-audio-35)

UxAir's BUFFERSTREAM adapter supplies the source codec, geometry, frame-rate and bounded source buffer plus compressed Annex B access units. The ACB window is a display-plane rectangle independent of input geometry. The public evidence reviewed does not establish a webOS 3.9 BUFFERSTREAM contract for selecting 10-bit decode surfaces, negotiating transfer functions, or passing metadata to the compositor. Review `Media::open` and the legacy BUFFERSTREAM payload when comparing any documented LG API or hardware trace. [Starfish adapter](../src/media_starfish.cpp)

## Formats and required evidence

| Format | Required behavior | Open questions on the B7 |
| --- | --- | --- |
| Main10 SDR | Accept 10-bit 4:2:0 HEVC, retain the source primaries, transfer, range and matrix, and present through a 10-bit path. Use SDR reference ramps and color bars for measurements. | Verify decoded surface precision, display color and unchanged SDR state across swaps. Successful Load or first frame is insufficient to establish correct output. |
| HDR10 | Use the PQ transfer function with BT.2020 signaling. Preserve valid mastering-display and content-light metadata from the applicable container and HEVC SEI, and verify the TV enters its HDR10 mode. | Determine what the decoder, API and compositor each consume. Annex B conversion can carry in-band SEI and still lose container-only metadata at the native boundary. |
| HLG | Validate HLG transfer signaling with the correct primaries, range and matrix. Confirm HLG mode and expected luminance with a reference rendition. | Confirm whether a public API conveys HLG signal type and whether mode resets to SDR after replacement, seek or error. |
| Dolby Vision | Preserve profile, level, configuration and dynamic metadata. Profile 5 uses a Dolby Vision-specific signal. Profile 8 compatibility is defined by its compatibility ID; for example, 8.1 carries an HDR10-compatible base layer. | The B7 product listing gives no profile list for BUFFERSTREAM. Establish support for each profile, metadata handling and proper panel mode via an authorized, documented integration. |

Color handling spans MP4 sample entries and `colr` or Dolby configuration boxes, HEVC VUI and SEI, decoder surfaces, and display output. A verification run needs known source metadata, expected display mode, and a visual or instrumented comparison. Include a normal SDR clip before and after every test, including a failed Load, discontinuity, seek and rapid replacement. [ITU-R BT.2100](https://www.itu.int/rec/R-REC-BT.2100) · [Apple HLS authoring requirements](https://developer.apple.com/streaming/hls-authoring-specification-for-apple-devices.html) · [Apple HDR metadata](https://developer.apple.com/av-foundation/High-Dynamic-Range-Metadata-for-Apple-Devices.pdf) · [Dolby Vision bitstream profiles](https://ott.dolby.com/OnDelKits/Dolby_Vision_Online_Delivery_Kit/v1/Documentation/Specs/Visio_Profiles/help_files/topics/c_dovi_profiles_public.html)

## Recommendation

Keep Main10, HDR and Dolby Vision outside the shipped Direct HLS capability policy. Advance Main10 SDR only with a supported native contract and a B7 measurement of output precision. Consider HDR10 and HLG next if that contract handles color mode and metadata end to end. Dolby Vision additionally needs documented profile support and a licensed path where required. If the sender provides a separately authored SDR variant, select the eligible SDR rendition. Direct playback has no video tone-mapping or software HEVC decoder.
