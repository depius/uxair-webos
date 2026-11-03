<p align="center"><img src="assets/uxair-icon-256.png" width="128" alt="UxAir receiver icon"></p>

<h1 align="center">UxAir</h1>
<p align="center"><strong>AirPlay Receiver for webOS</strong></p>

UxAir is a native AirPlay mirroring receiver for compatible rooted LG webOS TVs, based on the open-source UxPlay project. It is an independent community project and is not affiliated with or endorsed by Apple or LG. UxAir is compatible with AirPlay; AirPlay is an Apple trademark.

## Status

**UxAir 1.2.0 is the next planned official release and is not published.** Version 1.1.0 was internal development/testing. The security release hold remains active while the original browser Malware verdict is independently assessed. Do not bypass browser download protection.

The Starfish baseline at `148bc961` passed 30 minutes of repeated orientation changes, resolution switching and transitions among video, photos, mirroring and audio on the owner's rooted LG OLED B7 (webOS 3.9). The new security-hardened 1.2.0 build needs renewed physical acceptance. HDR10/Main10 is unsupported; native 4K scanout and displayed FPS are unmeasured.

### One codebase, two separate packages

| Target | Backend | Expected 1.2.0 filename | Hardware status |
| --- | --- | --- | --- |
| webOS 3-4 | Starfish + ACB | `com.github.depius.uxair_1.2.0_arm-webos3-4.ipk` | Stable release candidate; final hardened B7 validation pending |
| webOS 5+ | NDL DirectMedia | `com.github.depius.uxair_1.2.0_arm-webos5plus.ipk` | **Beta / Experimental; no current-build physical validation** |

NDL hardware is unavailable to the owner. Its lack of physical validation is accepted for Beta/Experimental scope and does not by itself block Starfish completion. See [final development scope](docs/UXAIR_1_2_SCOPE.md) for the integration/closure plan. The confirmed callback, placement and cleanup defects are addressed by the [NDL lifecycle fixes](docs/NDL_LIFECYCLE_FIX.md); firmware limitations remain explicit. Volunteer testers: [Beta test and log-reporting guide](docs/NDL_BETA_TESTING.md).

The packages use the same application ID and are alternative installations. Backend selection is at build time; there is no universal IPK or runtime switch. The contributor's earlier C4/webOS 10.3.1 results apply to PR #10's older source. Details and pending NDL tests: [PORT-WEBOS10.md](docs/PORT-WEBOS10.md).

## Features

- H.264 AirPlay mirroring up to 1080p/60, tested on the B7.
- HEVC/H.265 mirroring up to 4K/60, tested on the B7.
- 30 and 60 FPS profiles, including the B7-tested H.264 720p/1080p and HEVC 4K modes.
- Portrait and landscape adaptation.
- Photos and videos displayed through the tested mirroring workflows.
- Starfish hardware-accelerated video playback with stereo audio during mirroring.
- Standalone AirPlay music/audio-only playback, confirmed on the B7 in the tested workflow; individual audio codec variants were not tested separately.
- Configurable codec, resolution, frame rate, audio delay, and diagnostics.
- Magic Remote pointer, arrow, OK, and Back navigation.
- Large on-screen AirPlay PIN pairing, saved configuration, session history, and automatic decoder recovery/fallback.
- Lightweight diagnostics separating advertised rate, incoming rate, accepted Feed rate, and decoder callbacks. Decoder callback rate does not claim displayed FPS.

The iPhone, iPad, or Mac selects the actual AirPlay codec and rate. Selecting HEVC advertises HEVC alongside H.264; it cannot force a sender that chooses H.264. Direct Photos playback is distinct from Photos or video shown by mirroring. Direct Photos/video, HLS, AirPlay 2, HDR/Main10, and native 4K scanout are not supported or verified.

In UxAir 1.2.0, a fresh installation defaults to H.264 at 1920×1080 and 30 FPS. Valid saved profiles from earlier UxAir builds are retained across upgrades. Automatic decoder recovery is temporary for that run and does not replace the saved preference.

## Screenshots

### Development interface (captured before the 1.2.0 release)

![UxAir promotional image](screenshots/uxair-marketing.png)

![UxAir video settings](screenshots/uxair-video.png)

![UxAir history dashboard](screenshots/uxair-history.png)


## Hardware and requirements

- A compatible rooted LG webOS TV with native application development enabled.
- A TV and sender on the same local network with multicast/mDNS discovery available.
- Tested on LG OLED B7 (2017) running rooted webOS 3.9. Other LG webOS models may be compatible but have not yet been verified.
- The current NDL package is Beta/Experimental and physically unvalidated. The contributor reported earlier C4/webOS 10.3.1 testing before these shared follow-ups.
- The ARMv7 build uses the pinned webOS Brew toolchain. CI performs host tests and builds/validates the ARMv7 IPK.

## Install and use

### Download UxAir

Published versions remain on [GitHub Releases](https://github.com/depius/uxair-webos/releases/latest). **There is no published 1.2.0 download.** Use the installation examples below only for an explicitly approved candidate or released package, choosing its backend from the table above. The owner has authorized temporary TEST candidates for this validation round, not a public release. Do not bypass browser or antivirus warnings. See [final B7 test instructions](docs/B7_FINAL_TEST.md).

### Method 1 — webOS CLI (verified on the B7 baseline)

Use this method if you already have the webOS CLI installed and `YOUR_TV` configured as a target device. For an approved B7/Starfish candidate, use the platform-suffixed filename below; select the webos5plus package only for an approved NDL test:

```sh
ares-install -d YOUR_TV com.github.depius.uxair_1.2.0_arm-webos3-4.ipk
ares-launch -d YOUR_TV com.github.depius.uxair
```

Open UxAir, select it from Screen Mirroring, and enter the pairing PIN when prompted. Pairing credentials and settings are private to the UxAir app ID.

### Method 2 — webOS Dev Manager (preflight limitation)

1. Install [webOS Dev Manager](https://github.com/webosbrew/dev-manager-desktop).
2. Connect a compatible rooted LG webOS TV.
3. Download the UxAir `.ipk` package.
4. Use webOS Dev Manager's application installation functionality to select the downloaded IPK.
5. Install and launch UxAir on the television.
6. Select UxAir from the AirPlay / Screen Mirroring menu on an Apple device.
7. Enter the pairing PIN if requested.

For standalone music playback, select UxAir as the AirPlay audio output.

### Upgrading UxAir 1.0.0 or internal 1.1.0

Keep `com.github.depius.uxair` and install as an upgrade; do not uninstall or delete state. Back up original private state first. The 1.2.0 version change retains state paths and formats. B7 same-version CLI upgrade and clean installation were verified on the 148bc961 baseline with pairing/settings preservation; 1.0.0/1.1.0-to-1.2.0 and hardened-package physical checks remain required. A legacy app-local NDL identity without var/lib state now stops startup rather than silently creating new keys; obtain a reviewed migration before using that case.

Dev Manager 1.99.21 may wait indefinitely on an unresponsive Homebrew configuration preflight before uploading. That blocker was reproduced on the B7; direct official CLI 3.2.6 installation succeeded for both baseline scenarios. Glasshouse reinstall also succeeded according to the owner. The original Dev Manager frontend trace was not captured. Do not change package versions to work around this unverified UI incident; see [RELEASING.md](docs/RELEASING.md).

### Migrating from UxPlay webOS development builds

UxAir uses the new ID `com.github.depius.uxair`. It installs alongside the existing UxPlay webOS development app and does not overwrite or uninstall it. The new ID has separate pairing keys, receiver identity, and settings; you will pair again and configure UxAir separately. You may keep the old development app installed while verifying UxAir on your own TV. No state is migrated or removed automatically.

## Remote and configuration

- Left/Right changes the Video, Audio, History, Diagnostics, and About sections.
- Up/Down focuses rows; OK opens or confirms a choice; Back cancels a dialog or pairing attempt and returns from playback to the UI. Starfish remote Exit terminates; background operation defaults off. NDL defaults to background/discoverable behavior and still needs physical validation.
- During playback, OK opens diagnostics and Back returns to video.
- Set audio delay before connecting; positive values delay the TV audio.
- Choose 30 or 60 FPS as the advertised maximum. The sender may use a lower rate.
- HEVC capability does not force the sender's codec selection.

## Build from source

Use Linux x86-64 (including supported WSL2 Ubuntu) or macOS ARM64 with the checksum-pinned SDK. See [DEVELOPMENT.md](docs/DEVELOPMENT.md) for full host prerequisites and locked, script-free packaging-tool setup.

```sh
python3 tools/validate-metadata.py
tools/test.sh
tools/build.sh --media=starfish
python3 tools/validate-ipk.py
tools/build.sh --media=ndl
python3 tools/validate-ipk.py
tools/source-package.py
```

Validate each target before the next build changes staging. CI runs host sanitizers and both ARM targets. Ordinary runs upload no artifacts; the branch-only, explicit `prepare_test_assets` dispatch retains authenticated TEST artifacts for one day. Public release remains on hold. Prepared asset names are `UxAir-1.2.0-webOS3-4-Starfish.ipk` and `UxAir-1.2.0-webOS5plus-NDL-Beta.ipk`; both embed the same source revision. No compile result establishes physical NDL support. Review [release gates](docs/RELEASING.md), [security findings](docs/SECURITY_HARDENING.md), and [branch integration review](docs/B7_BRANCH_FINAL_REVIEW.md).

## Known limitations and troubleshooting

- The tested standalone music workflow does not validate each negotiated audio codec separately. Protected media and unsupported AirPlay protocols are not decrypted or altered.
- An AirPlay sender may select a different codec or frame rate than the requested maximum.
- Incoming/Feed rates and Starfish `FRAMEREADY` callbacks are not proof of displayed FPS. Native panel output resolution is unmeasured.
- HDR10/Main10, direct Photos/video, AirPlay 2, startup at boot, broad TV compatibility, and macOS extended-display use are not supported or certified.
- Starfish buffering on webOS 3.9 can add latency.
- If the receiver does not appear, check same-network connectivity and multicast/mDNS. If a codec fails, reconnect and choose a supported profile. Review `uxair.log`; logs can contain sender names and local network details.

## Acknowledgements and license

This project builds on the excellent work of:

- **UxPlay by FDH2** — the original open-source AirPlay mirroring server.
- [**uxplay-webOS by Daniel Filho (@danlflh)**](https://github.com/danlflh/uxplay-webos) — the original webOS port.

UxAir extends this foundation with additional features, performance improvements, and a redesigned interface. Development and adaptation: [@depius](https://github.com/depius). This repository is [depius/uxair-webos](https://github.com/depius/uxair-webos) (formerly `uxplay-webos`). UxAir is licensed under [GPL-3.0-or-later](LICENSE); corresponding sources, upstream attribution, and third-party notices are preserved in [THIRD_PARTY.md](THIRD_PARTY.md).

## Support development

<a href="https://buymeacoffee.com/depius"><img src="assets/buy-me-a-coffee.svg" alt="Buy me a coffee"></a>

UxAir is free and open source. If you enjoy using it, you can support its development by buying me a coffee.

https://buymeacoffee.com/depius

Donations are optional and do not unlock features or affect the license. The app displays a bundled offline QR code for the same support URL.
