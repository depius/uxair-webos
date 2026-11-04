# Milestone 8: HDR evidence, 4K presentation and UI responsiveness

> Historical development record. See [UxAir 1.2.0 scope](UXAIR_1_2_SCOPE.md) for current release and validation status.

## HDR10 / Main10

No HDR playback path is enabled. The receiver detects HEVC Main10 and rejects it before Starfish Load; it does not pass bit depth, transfer function, color primaries, mastering metadata or content-light metadata to the decoder or output. Old settings files may contain `hdr=auto` / `hdr=sdr`; the loader ignores these legacy keys and new saves omit them.

LG's webOS TV 3.5 format table lists Main10 for supported HEVC video playback formats, including UHD on UHD models ([LG format table](https://webostv.developer.lge.com/develop/specifications/video-audio-35)). That documents the TV playback stack, not the raw Starfish `BUFFERSTREAM` contract used here. The public Starfish header declares `setHdrInfo()` without a payload schema, while public NDL HDR metadata fields belong to a different DirectMedia API generation. Neither establishes a usable webOS 3.9/Rockhopper contract. A B7 decoder/output trace is also absent. Main10 and HDR therefore remain unavailable; this milestone does not claim HDR support.

## 4K and scaling evidence

The sender's parsed SPS dimensions, Starfish `video-info` dimensions, and requested ACB display window are separate values. The UI reports the Starfish dimensions and ACB window as such and continues to label physical panel output unmeasured. SDL now logs its size explicitly as the logical graphics window. This avoids treating app graphics coordinates or decoder-reported dimensions as proof of panel scanout resolution.

LG documents 1920×1080 graphics coordinates for apps on UHD TVs while video playback can use 3840×2160 ([LG app-resolution specification](https://webostv.developer.lge.com/develop/specifications/app-resolution)). This supports keeping the app UI and ACB window in logical app coordinates; it does not prove that this app's ACB plane is output at native UHD. `fit_video_window()` retains centered aspect-preserving fit for portrait and landscape, with existing tests covering HD and UHD input dimensions. No unmeasured scaling or decoder-mode change was introduced.

Direct 4K Photos/video, HDR still images, native panel output, and displayed FPS remain unverified or unsupported. HEVC 4K/30 remains the only owner-confirmed HEVC mirroring case; H.264 720p/1080p remains supported. The iOS sender still chooses codec independently of the receiver's HEVC advertisement.

## UI change and validation

While the session information page is visible, remote input now redraws it on the next main-loop pass rather than waiting for the periodic one-second diagnostics refresh. The existing one-second refresh remains for live values. A focused unit test covers hidden/idle state, immediate redraw, and the periodic interval. The graphics-window log and session diagnostics distinguish logical app geometry from physical output.

The host test suite and ARMv7 package workflow validate software behavior and package structure only. Neither proves HDR, native 4K output, nor actual remote responsiveness on the B7. Hardware verification is still required for those observations.
