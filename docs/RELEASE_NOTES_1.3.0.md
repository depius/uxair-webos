# UxAir 1.3.0

## What's Changed

- Fixed unexpected **AirPlay mirroring disconnects** caused by audio queue pressure, with improved recovery and session safety.
- Added more detailed **queue and playback diagnostics** to help investigate streaming issues.
- Fixed **startup on older webOS 3.4 TVs** by improving OpenSSL random-source compatibility.
- Fixed **History and cover-art crashes on SDL 2.0.3** found on older webOS 3.x TVs.

### Compatibility
- **webOS 3–4:** Starfish Stable – tested on LG OLED B7 (webOS 3.9); webOS 3.4 fixes tested by the contributor on LG 60UH8507-ZA.
- **webOS 5+:** NDL Public Beta – current build awaits physical validation.

Special thanks to [afds](https://github.com/depius/uxair-webos/pull/13) for the webOS 3.4 compatibility fixes.

**Full Changelog:** [v1.2.0...v1.3.0](https://github.com/depius/uxair-webos/compare/v1.2.0...v1.3.0)
