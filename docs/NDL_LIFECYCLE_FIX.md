# NDL Beta lifecycle correction

The final 1.2.0 candidate fixes confirmed source defects reproduced with the real NDL bridge/media code under host vendor/SDL mocks. It adds no playback feature and changes no Starfish implementation, receiver, HEVC parser, RAOP, UI, private storage, queue limit or feed timeout.

## Contracts and minimal correction

Reviewed API-v2 headers in the checksum-pinned Linux SDK (`dependencies.lock.json`, openlgtv/buildroot-nc4 webos-d7ed7ee). `NDLMediaLoadCallback(int, long long, const char *)` carries no application context/session token. Load, Unload, Init and Quit declare success/error returns. The headers do not guarantee callback quiescence after Unload/Quit or define cleanup after a partially failing call. They do not declare the dynamically resolved SetWindowId extension. SDL_webOSSetExportedWindow returns bool. No stronger firmware guarantee is assumed.

- Each attempted Load receives a unique immutable callback function address. A mutex-protected active slot/context gate rejects retired addresses before inspecting message data. Close retires and waits for an in-flight application callback before vendor Unload or Media destruction. API calls are serialized separately; the private Media callback does not reenter the bridge. This handles callbacks during Load, Unload and Quit without holding the callback gate over vendor calls.
- There is a finite budget of **1024 Load attempts per process** (including reinitializations). Addresses are never recycled, even after Quit, because an old callback has no distinguishable token. Exhaustion fails closed with `callback-identities-exhausted-restart-required`; fully stop/relaunch UxAir. This conservative bound is specific to NDL, not a frame/queue limit or Starfish limit. Unlimited identity reuse requires a verified vendor quiescence contract or a different supported callback API.
- Missing/failed SetWindowId aborts video startup before Load. Its firmware signature/return convention remains the existing port's assumed `int(const char *)`, dynamically resolved; the pinned SDK does not validate that extension. Failure poisons reuse if binding may have partially succeeded. Audio-only operation does not require a video window. SDL placement failure aborts and unloads the stream.
- Even a failing Load is retired and unloaded. Unload or Quit failure is logged and prevents future reuse/Feed in this process, without retrying uncertain operations. Failed initialization similarly prevents retry. Successful Quit can confirm resource release after an Unload failure, but does not make the poisoned process reusable. Quit failure retains the exported window until process exit rather than destroying a possibly referenced resource. The bound window ID uses process-owned storage.

Confirmed old-session contamination, unsynchronized callback registration/lifetime, unchecked binding/placement, ignored cleanup results and skipped partial-Load cleanup are addressed by these mechanisms. Host tests do not establish whether firmware actually generated late callbacks in prior builds. Unknown vendor blocking behavior, resource-loss recovery and partial failed Init internals cannot be proved from declarations; the application fails closed rather than assuming a fresh backend. No speculative recovery timer is introduced.

## Deterministic regression coverage

`tests/ndl_media_test.cpp` links actual `src/media_ndl.cpp` and `src/ndl_backend.c` against mocks with ASan/UBSan and pthreads. Independent child processes preserve the production fail-closed state instead of adding test reset hooks. Checks cover:

- Old callbacks after close, reopen, Quit and owner destruction, including invalid message pointers on retired callbacks; active callbacks still affect only the correct session.
- Callback already executing while close waits, followed by safe target destruction and a new session; synchronous Load callback from a separate vendor thread.
- Export/DL/Init/Load/binding/placement failure, error during Load, partial cleanup and no Feed after unsuccessful startup.
- Failed Unload/Quit, no repeated uncertain cleanup, no new Load after poison, and no window destruction after unconfirmed Quit.
- All 1024 unique identities and exhaustion without wraparound, including after Quit.
- Existing H.264/HEVC geometry, audio-only/PCM enums, timestamp conversion, buffer retries, original video retry bound and retained exported window across reopen.

The ARM matrix separately compiles/links against real SDK headers and libraries. Mocks do not prove compositor binding, audible/visible playback, firmware callback scheduling, interruption/FLUSH semantics or jailed persistence. These remain [Beta hardware follow-ups](NDL_BETA_TESTING.md), not a claim of stable NDL support.
