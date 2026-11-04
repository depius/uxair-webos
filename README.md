<p align="center"><img src="assets/uxair-icon-256.png" width="128" alt="UxAir receiver icon"></p>

<h1 align="center">UxAir</h1>
<p align="center"><strong>AirPlay Receiver for webOS</strong></p>

UxAir is a native AirPlay mirroring receiver for compatible rooted LG webOS TVs, based on the open-source UxPlay project. It is an independent community project and is not affiliated with or endorsed by Apple or LG. UxAir is compatible with AirPlay; AirPlay is an Apple trademark.

## UxAir 1.2.0

| TV software | Choose this package | Support |
| --- | --- | --- |
| webOS 3–4 | `UxAir-1.2.0-webOS3-4-Starfish.ipk` | **Stable**. The owner accepted the Starfish candidate at `9af36d1b2e3902bfa26ab349140af394b3b95a3f` on an LG OLED B7 running webOS 3.9. |
| webOS 5+ | `UxAir-1.2.0-webOS5plus-NDL-Beta.ipk` | **Public Beta / Experimental**. The current build has no physical hardware validation. |

Both packages use `com.github.depius.uxair`. Choose one for the TV's webOS version. The earlier C4/webOS 10.3.1 results apply to ALQU-IT's original NDL contribution. See the [B7 acceptance record](docs/B7_FINAL_TEST.md), [NDL port notes](docs/PORT-WEBOS10.md) and [Beta reporting guide](docs/NDL_BETA_TESTING.md).

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

### Download and install

On [GitHub Releases](https://github.com/depius/uxair-webos/releases), look for version 1.2.0 and select the IPK for your webOS version from the table above. Match its SHA-256 against the release's `SHA256SUMS` before installing. If 1.2.0 is absent, publication is still pending. See [security hardening details](docs/SECURITY_HARDENING.md).

With the webOS CLI and a configured target named `YOUR_TV`, run the install command for your package:

```sh
# webOS 3–4: Starfish Stable
ares-install -d YOUR_TV UxAir-1.2.0-webOS3-4-Starfish.ipk

# webOS 5+: NDL Beta
ares-install -d YOUR_TV UxAir-1.2.0-webOS5plus-NDL-Beta.ipk
```

Launch with `ares-launch -d YOUR_TV com.github.depius.uxair`. Select UxAir from Screen Mirroring or as the AirPlay audio output. Enter the PIN if prompted. Beta testers can report their TV model, webOS version and sanitized logs using the [Beta reporting guide](docs/NDL_BETA_TESTING.md).

[webOS Dev Manager](https://github.com/webosbrew/dev-manager-desktop) also supports installing a local IPK. If its Homebrew preflight stalls on the B7, use the CLI path. The CLI was verified on the earlier B7 baseline. See the [installer investigation](docs/B7_BRANCH_FINAL_REVIEW.md).

### Upgrading UxAir 1.0.0 or 1.1.0

Back up private state, then install over the existing app with the same backend and application ID. Uninstalling can delete app-local state. An NDL installation with legacy app-local identity and no var/lib state requires a reviewed migration to retain the existing keys.

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

Validate each target before the next build changes staging. CI runs host sanitizers and both ARM targets. See the [release procedure](docs/RELEASING.md) for artifact provenance, checksums and approval gates.

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
- [**ALQU-IT**](https://github.com/depius/uxair-webos/pull/10) — the NDL DirectMedia port and its initial C4 testing.

UxAir extends this foundation with additional features, performance improvements, and a redesigned interface. Development and adaptation: [@depius](https://github.com/depius). This repository is [depius/uxair-webos](https://github.com/depius/uxair-webos) (formerly `uxplay-webos`). UxAir is licensed under [GPL-3.0-or-later](LICENSE); corresponding sources, upstream attribution, and third-party notices are preserved in [THIRD_PARTY.md](THIRD_PARTY.md).

## Support development

<a href="https://buymeacoffee.com/depius"><img src="assets/buy-me-a-coffee.svg" alt="Buy me a coffee"></a>

UxAir is free and open source. If you enjoy using it, you can support its development by buying me a coffee.

https://buymeacoffee.com/depius

Donations are optional and do not unlock features or affect the license. The app displays a bundled offline QR code for the same support URL.
