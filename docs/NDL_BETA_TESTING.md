# NDL Public Beta / Experimental testing

UxAir 1.2.0 NDL is intended for webOS 5+ volunteer testing after explicit publication approval. The current source has **no physical NDL hardware validation**; the owner has no compatible TV. Successful compilation, linkage, sanitizers and IPK checks do not establish working video/audio on a particular firmware. Choose the NDL Beta IPK for webOS 5+ from the [installation guide](../README.md#download-and-install).

Use only the NDL package on a compatible rooted TV, with app ID `com.github.depius.uxair`. It is an alternative to Starfish, not a second app. Privately back up existing state before upgrade; do not uninstall or delete identity/pairing/settings to troubleshoot. Legacy app-local identity without usable var/lib state intentionally fails startup; request a reviewed migration rather than generating replacement keys. Respect browser/antivirus warnings; report the exact verdict without bypassing protection.

## Test and report

Record TV model, exact webOS/firmware version, rooted/developer environment, sender model and OS version, app version, embedded full source commit and IPK SHA-256. Note whether this was a clean installation or upgrade and the prior version. Include installer command/output and launch method. Never publish credentials, private backups, paired.keys, server.key, device.id or PINs.

Check launch/idle/Exit, pairing reuse after relaunch/reboot, H.264 first, then HEVC only if supported. Test landscape/portrait changes, repeated reconnects/reinitializations, music-only and music with photos, audio/video transitions, FLUSH/resume, volume/mute and sender stopping/TEARDOWN. Record expected versus actual visible/audible results and approximate times; decoder callbacks/accepted feeds are not proof of displayed frames. Background/foreground/screensaver and remote Exit differ from the B7 defaults and need testing.

Logs normally reside in `/media/developer/apps/var/lib/com.github.depius.uxair/uxair.log`; retain bounded startup/session output too, including generation/epoch/reset origin and `[ndl] failure=` messages. Redact sender names, local addresses and sensitive metadata before sharing. Include the lines preceding failure and the exact reproduction sequence; provide short sanitized logs rather than keys or media payloads.

Submit a manually reviewed report at [UxAir issues](https://github.com/depius/uxair-webos/issues), labeled in its title `NDL Beta: <model/webOS> <symptom>`. No logs or issue are submitted automatically. Report any security warning separately with artifact URL/hash and exact warning text. The historical Malware verdict remains unresolved.

## Known limits

NDL has no implemented decoder pause/eos/FLUSH operation: shared queued audio is invalidated on FLUSH but already-fed sound/timing needs hardware observation. Resource-release notification is logged; automated resource-loss recovery is unverified. Dynamic SetWindowId availability/signature/return convention and firmware cleanup guarantees are not specified by the pinned SDK.

The [lifecycle fix](NDL_LIFECYCLE_FIX.md) safely rejects retired callbacks. It reserves 1024 unique Load identities per process, including reinitializations; exhaustion or failed cleanup/initialization requires a full process stop/relaunch. Do not retry endlessly in a poisoned process. Fully close via the platform app manager if the remote backgrounds the app. Unknown proprietary blocking behavior cannot be resolved by a host mock. These limits are disclosed for Beta and must not be presented as stable NDL hardware support.
