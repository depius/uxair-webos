# UxAir Dev encrypted video control · Phase 2

## Integration and compatibility

UxAir Dev Starfish keeps the native UxPlay listener, existing registered-controller Pair-Verify, HTTP parser and response handlers. The shared component uses the same Go SAP, envelope and record state exercised at Phase 1 commit `8655ea285a1b2169256582c73bc8daa5132ccfac`. The implementation moves that state behind an integer-handle C ABI. [Phase 1 design and results](ENCRYPTED_VIDEO_CONTROL_PHASE1_1_4.md)

| Approach | Compatibility assessment |
| --- | --- |
| Go shared object | Go supports Linux ARM cgo shared builds. The build uses GOARM 7 soft-float and the pinned webOS SDK for the final C link. UxAir loads the DSO only when a registered, pair-bound video socket starts SAP. The loader, runtime threads, signal handling, memory footprint and jail behavior still need a B7 observation. The DSO remains mapped for process lifetime. |
| Native C/C++ port | The shipped private OpenSSL already provides X25519, Ed25519 and AES-GCM. A port could use the same listener and avoid the Go runtime. The pinned legacy native FairPlay responder supplies canned SAP replies and retains the M3 message. It cannot authenticate the reference `/fp-setup2` envelope. A native implementation would need a separate SAP port and parity testing against the Go vectors. |

The Go shared object keeps the validated SAP and record code and isolates its loading from application startup. The package preflight checks the library's ARM ELF32 soft-float ABI, exported C ABI, glibc 2.19 ceiling and system-library allowlist. It independently checks the app's GLIBC 2.7 and GLIBCXX 3.4.29 launch ceilings, its existing loader and absence of a mandatory dependency on the encrypted library. These checks establish link compatibility. The hardware retest is required for loader and runtime compatibility. [Go Linux minimum kernel versions](https://go.dev/wiki/Linux) · [Go build mode support](https://go.dev/src/internal/platform/supported.go)

The build uses a private DSO under the Dev application's `lib/` directory. It uses the application's existing runtime search path. Development Starfish includes the LGPL notices and corresponding source archive for the pinned library. The C ABI supports replacing the DSO with an interface-compatible build. Rebuilding the component uses the exact reference commits, checked by `check.py`. To build a modified library from the accompanying source, extract the reference tar, initialize a Go workspace against its `third_party/airplay-control` directory and UxAir's `tests/airplay_control_phase1` module, and build `./cmd/shared` with `-buildmode=c-shared` and the same SDK compiler. [Phase 1 license review](ENCRYPTED_VIDEO_CONTROL_PHASE1_1_4.md#pinned-sources-and-licensing)

## Connection state

1. The native registered-key callback authorizes the controller's Ed25519 key. The existing Pair-Verify code proves possession and generates fresh X25519 material. A video socket that negotiated `X-Apple-PD: 1` retains its own verified shared secret. The AirPlay session header binds request ownership without authenticating anyone.
2. The pair-bound socket uses the pinned SAP receiver to validate M1 and M3 and generate M2 and M4. Existing unbound legacy SAP remains available on its original path. That path has no eligible encrypted upgrade.
3. `POST /fp-setup2` checks proof, session binding and SAP sequence. The DSO authenticates the 120-byte control envelope, rejects weak or exhausted initial material and derives separate pair-bound directions. The native listener writes the empty plaintext HTTP 200 with explicit `Content-Length: 0`. A complete write opens the encrypted read path. The parser returns the exact plaintext boundary so bytes from a coalesced TCP read go straight to the record reader.
4. The native connection accumulates one bounded encrypted record at a time. The DSO verifies AES-GCM before bytes reach a dedicated HTTP parser. That parser can continue across records and can dispatch multiple requests from one record. The listener seals every response on the same socket. Truncation, invalid authentication, framing, short writes and deadlines close the socket.
5. The listener owns transport and parser state. Pairing, SAP and record states never move between connections. The C ABI receives bounded temporary C buffers and returns copied bytes. The Go state keeps no C pointers. Teardown frees the integer handle, cleans C-owned secret and plaintext buffers and drops Go crypto objects. Go runtime memory reclamation provides no guarantee of zeroization.

The handshake has a 30-second deadline from the Pair-Verify challenge and a 30-second window for SAP after proof verification. Incomplete encrypted records and HTTP messages have individual 10-second deadlines. The record cap is 256 KiB. Parsed encrypted headers and bodies are limited to 8 KiB and 16 KiB respectively, with a 24 KiB total per request. Each listener uses its existing native thread. The DSO runs crypto synchronously on that thread while its Go runtime manages its own workers. The listener can close a socket on a failed or incomplete nonblocking write.

## Dispatch and diagnostics

- Encrypted bodyless `GET /server-info` and `GET /playback-info` use the existing discovery and Direct Video handlers. Supported encrypted `PUT /setProperty` requests use the existing property handler.
- Encrypted `POST /play` is counted as an admission attempt and receives HTTP 501. Other unsupported controls, including `/reverse` and `/scrub`, receive encrypted HTTP 501. A conflicting session alias receives encrypted HTTP 409. HTTP framing errors close the connection.
- `[video-crypto]` reports the real pairing, SAP, envelope, acknowledgment boundary, first authenticated record and teardown. `[video-control]` records the first decrypted method and allowlisted endpoint and samples subsequent responses. `[airplay-wire-summary]` counts attempts, upgrades, failures, records, requests and admissions even when request sampling is exhausted. Unknown targets are labeled `redacted`. Logs contain no raw envelopes, keys, query values or request bodies.

`src/direct_dispatch.h` handles native authentication and handshake routing. `src/direct_crypto.c` loads the C ABI and assembles encrypted records. `src/direct_encrypted_dispatch.h` owns decrypted control dispatch and ciphertext responses. `tools/instrument-uxplay.py` applies development-only hooks to exact pinned UxPlay source. Production app metadata and storage paths are outside the change.

## Validation

The development workflow runs `tools/test.sh` under ASan/UBSan and enables the native encrypted TCP fixture. The fixture uses the pinned reference sender on real TCP against the generated native listener and shared library. It independently verifies responses, crosses the plaintext acknowledgment boundary with buffered ciphertext, fragments HTTP across records, checks native discovery and property dispatch and keeps a concurrent RTSP/RAOP control connection open. Negative tests cover cross-socket proof and envelope reuse, replay, unexpected plaintext and malformed authenticated HTTP. The Phase 1 suite covers the remaining counter, fragmentation and malformed-record cases.

Local preflight on Windows passed the Phase 1 and pinned pure-Go reference suites, the ARMv7 host-fixture cross-compile, metadata and source-generation checks, and a standalone MSVC AddressSanitizer run of the generated parser's buffered boundary and framing cases. The optional upstream media fixtures passed with FFmpeg 9.0.2 locally. CI leaves the media fixtures opt-in because Ubuntu's default FFmpeg lacks their `extension_picky` option. The Linux sanitizer, native TCP and packaged ELF results belong to the Actions build.

The packaging job follows the native tests and builds one development Starfish IPK. Its artifact includes `BUILD_COMMIT.txt`, `SHA256SUMS`, `ELF_REPORT.txt`, `IPK_REPORT.txt` and matching source. Physical startup, secure-store access and Safari behavior are hardware checks. The clear-video transport and future media-session handling are separate milestones.

## LG B7 acceptance

1. Install the Dev IPK over the existing `com.github.depius.uxair.dev` application. Launch **UxAir 1.4.0-dev** from the TV Home launcher. Confirm the complete interface, Magic Remote navigation, discovery and PIN pairing if prompted.
2. In iPhone Safari, play an unprotected H.264/AAC video. Use the AirPlay button inside the player and choose UxAir Dev. End the Safari attempt once the result is visible. Successful Phase 2 hardware negotiation means the same connection alias reports an authenticated envelope, `result=encrypted`, and a first authenticated `[video-control]` request. Record the endpoint. A visible menu is expected while media admission is deferred.
3. Verify H.264 mirroring, HEVC mirroring if enabled, audio-only AirPlay, disconnect and reconnect. Stop UxAir Dev to capture the final listener summary. Copy the complete Dev log from PowerShell.

```powershell
$tv = Read-Host 'LG B7 IP address'; scp -O "root@${tv}:/media/developer/apps/usr/palm/applications/com.github.depius.uxair.dev/state/uxair.log" ".\uxair-dev-phase2.log"
```

If Safari omits pair-bound negotiation or produces a different envelope, the logs will show where it stopped. The B7 trace from Phase 1 established a 120-byte body but did not identify the pairing mode or envelope discriminator. Confirm these on hardware before extending the handshake.

For Phase 3, use the first real encrypted requests to define command and reverse socket ownership. Admit authenticated `/play` only after its session association and URL have been validated, then measure whether Starfish needs native URI, BUFFERSTREAM or a separate FCUP relay for the observed media.
