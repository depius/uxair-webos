# webOS 5+ / webOS 10: NDL target

UxAir 1.2.0 retains one shared receiver/UI/state codebase and two compile-time backends: Starfish + ACB for webOS 3-4, NDL DirectMedia for webOS 5+. There is no runtime backend switching or universal IPK. NDL is **Beta / Experimental: no physical validation of the current 1.2.0 shared source has been performed**. Volunteer feedback will guide NDL validation. See the [Beta reporting guide](NDL_BETA_TESTING.md).

The contributor reported successful mirroring, HEVC/H.264, photo/video transitions and audio-only playback on a rooted LG OLED65C48LA C4 (webOS 10.3.1) for PR #10 at 8526f8d. That is historical evidence for the earlier source, not physical acceptance of subsequent B7 fixes, hardening or 1.2.0. The Starfish 1.2.0 candidate passed the owner’s final B7 test. See the [acceptance record](B7_FINAL_TEST.md).

## Separate builds

| Target | Command | Media files | 1.2.0 release asset |
| --- | --- | --- | --- |
| webOS 3-4 | `tools/build.sh --media=starfish` | media_starfish.cpp, Starfish/ACB | `UxAir-1.2.0-webOS3-4-Starfish.ipk` |
| webOS 5+ (Beta / Experimental) | `tools/build.sh --media=ndl` | media_ndl.cpp, ndl_backend.c | `UxAir-1.2.0-webOS5plus-NDL-Beta.ipk` |

CMake `UXAIR_MEDIA=starfish|ndl` selects sources, definitions and linked APIs. The current default is ndl; explicit commands avoid ambiguity. Both packages use com.github.depius.uxair and are alternative installations. Full locked setup/verification: [DEVELOPMENT.md](DEVELOPMENT.md).

## Existing NDL implementation and physical checks

The installed app registers with Luna, creates an SDL exported window, looks up NDL_DirectMediaSetWindowId dynamically, and binds the window before feeding. The C bridge uses LG's API-v2 headers. NDL initialization/window binding lasts for the process; sessions Load/Unload independently to avoid losing the binding. Successful Load counts as ready because LOADCOMPLETED may require data flow. Audio/silence feed is needed for playback progress. Scheduling is arrival-anchored with at most 50 ms hold; Starfish retains its own first-feed/preroll/timeline policy. None of these runtime contracts was changed by the hardening/version work.

Future optional physical tests should cover startup from an installed package, H.264 and HEVC portrait/4K reopens, audio-only FLUSH/resume/volume, photo browsing with music, rapid audio/mirroring handoffs, sender TEARDOWN versus failures, decoder resource release and late callbacks. Confirm foreground/background/screensaver behavior and remote input, relaunch/reboot, fresh installation and upgrades. Decoder callback timing and actual displayed frames must not be inferred from Feed acceptance.

Shared fixes now available to both targets include metadata cadence, RTP origin/reset accounting, epoch/closing ingress protection, split HEVC header retention, private-state validation and production allocation limits. ALQU-IT’s original NDL implementation is retained in the commit history and includes subsequent [lifecycle fixes](NDL_LIFECYCLE_FIX.md).

## Persistent identity and UI policy

Starfish retains app-local state and the validated B7 container exception. NDL retains process-owned `/media/developer/apps/var/lib/<app-id>` storage and strict ownership validation. Existing NDL var/lib state is reused; a legacy app-local identity with no var/lib state now stops startup rather than silently creating a replacement. Automatic cross-backend migration is not implemented. Preserve private backups and obtain a reviewed migration before attempting that case.

Now Playing and system-bus integration are shared. Cover images use optional SDL2_image and are bounded before decoding. Whole-layout drift is currently disabled; title scrolling and dimming remain. Background mode defaults on for NDL and off for Starfish; Starfish remote Exit still terminates. SDL_QUIT/process signals remain real termination paths. NDL background/return-to-app behavior needs renewed hardware validation.

The independent NDL/HEVC probe apps retain their own diagnostic IDs and versions; they are not UxAir 1.2.0 production packages. PR #9's experimental Direct Video/HLS work is not part of this release and has no NDL media integration here.
