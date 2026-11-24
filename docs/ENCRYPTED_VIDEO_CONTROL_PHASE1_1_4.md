# UxAir Dev encrypted video control · Phase 1

## Architecture decision

The host reference uses an isolated Go module. It uses the same pinned receiver SAP and record primitives as iPhoneMirror, but owns the HTTP framing, registered-controller check and socket lifecycle. The transport code is outside the native build. The iPhoneMirror dependency stays in a separately checked-out, pinned repository.

| Option | Benefits | Costs and open questions |
| --- | --- | --- |
| Native C/C++ | Existing dispatcher, OpenSSL and Starfish toolchain. Explicit ownership, resource limits and buffer clearing. Avoids a second runtime on ARMv7. | Requires a careful port of the SAP implementation and auditing the target OpenSSL APIs. Needs cross-implementation vectors, ARM benchmarks, memory profiling and an ABI build against the B7 sysroot. |
| Isolated Go | Existing receiver SAP and framing implementation, separately testable on a host. Standard-library X25519, Ed25519 and AES-GCM. A future C ABI can keep keys and counters behind a connection handle. | Go garbage collection and per-process runtime cost need measurement. A cgo shared library introduces toolchain, loader, thread and libc constraints. A pure-Go ARM build provides no evidence that a cgo library can load in the webOS launcher. |

For native integration, a narrow boundary would allocate a connection context, submit bounded handshake messages and authenticated records, return owned bytes and stable errors, then destroy the context. The caller keeps the socket and emits the last plaintext response before switching parsers. Memory ownership and panic containment must be specified before exposing a C ABI.

## Pinned sources and licensing

go-atv supplies the sender path and both cryptographic primitives. Its `fairPlayVideoConnection` performs Pair-Verify, may read `/server-info`, and runs a fresh SAP exchange before the upgrade. It uses another connection for reverse events, with its own authentication and record states. iPhoneMirror supplies the receiver design, including the direction reversal in `receiver.State.Setup`, a C ABI, and separate FCUP/media and native smoke paths. Its `SOURCE.md` identifies the two primitive packages as unchanged copies from the go-atv pin. [go-atv sender source](https://github.com/jkiddo/go-atv/blob/f47a3ffcfa9458aac40562d73c314de1447df013/airplay/fairplay_control.go) · [iPhoneMirror receiver source record](https://github.com/RayrenSX/iPhoneMirror/blob/5e4443ceac899a3b0a7df4d92696cefc01673c6a/third_party/airplay-control/SOURCE.md)

The pinned subset contains the LGPLv3 text, the incorporated GPLv3 text and attribution. The iPhoneMirror adapter and doubletake-derived files have LGPL-3.0-or-later headers. The go-atv notice identifies the doubletake authors and their modified files. UxAir's tree includes GPLv3 terms. Phase 1 distributes only UxAir code and uses an external checkout for local host tests. For a distributed combined work, retain all applicable notices and supply corresponding library source including local changes. A static combination under LGPLv3 §4 requires the corresponding application code and rebuild/relink terms, or another applicable path in the license. An appropriate shared-library mechanism must permit replacement with an interface-compatible modified library. Record the selected terms and verify any required installation information before packaging. [Pinned notices](https://github.com/RayrenSX/iPhoneMirror/blob/5e4443ceac899a3b0a7df4d92696cefc01673c6a/third_party/airplay-control/THIRD_PARTY_NOTICES.md) · [LGPLv3 terms](https://www.gnu.org/licenses/lgpl-3.0.html)

The prior B7 run reached `POST /fp-setup2` with a 120-byte HTTP body after successful Pair-Verify, `/server-info` and both `/fp-setup` exchanges. The native dispatcher returned `501 protected-video-unsupported`. That body length is consistent with the reference control envelope. The B7 trace has yet to establish the envelope discriminator, authentication or negotiated pairing mode. The control envelope establishes transport keys that can carry a clear media URL. Commercial media DRM lies outside this work. [Native dispatch](../src/direct_dispatch.h) · [Reference envelope receiver](https://github.com/jkiddo/go-atv/blob/f47a3ffcfa9458aac40562d73c314de1447df013/internal/fairplay/session.go)

## Receiver state machine

Each accepted TCP socket owns one instance of this sequence. Requests use HTTP/1.1, with no `CSeq` requirement.

| State | Required input and response |
| --- | --- |
| Pair start | Accept a 68-byte legacy `POST /pair-verify` M1 with `X-Apple-PD: 1` and a registered Ed25519 public key. Generate fresh X25519 material and respond with the receiver's public key and encrypted transcript signature. |
| Pair challenge | Verify the controller's Ed25519 signature over both public keys using the continuation of the Pair-Verify AES-CTR stream. Return an empty HTTP 200 after verification. The controller key authorizes this socket only. Session ID headers carry no authority. |
| SAP M1 and M3 | Call the pinned SAP receiver for each `POST /fp-setup`. It validates the v3 message headers and lengths, selected mode and M3 confirmation. The replies are the generated M2 and M4, bound to this socket's SAP exchange. |
| Upgrade | Accept one `POST /fp-setup2`. Authenticate its envelope and derive both record states. Send an empty plaintext HTTP 200 with `Content-Length: 0`, then consume bytes from the existing buffered socket reader as encrypted records. |
| Encrypted control | Authenticate records before parsing the plaintext HTTP stream. Accept bodyless `GET /playback-info` and return an encrypted XML plist with idle state and `readyToPlay=false`. |
| Teardown | Close on EOF, deadline, malformed or out-of-sequence requests, invalid authentication, framing errors or incomplete writes. The socket never switches back to plaintext parsing. |

The host fixture exercises the pair-bound mode advertised on the pinned sender's M1. The Apple TV parent RTSP session, `/server-info`, reverse events and media session admission remain part of the existing or future native dispatcher. The isolated endpoint exposes one accepted socket at a time through `Receiver.Serve`, with concurrent calls using independent state. The caller supplies the receiver identity and registered-controller keys. There is no discovery service.

## Envelope and record validation

- The 120-byte control envelope has the 16-byte prefix `46 50 4c 59 01 02 01 00 00 00 00 6c 00 00 00 00`, a 16-byte CBC IV at offset 16 and a big-endian 64 at offset 32 for the wrapped payload length. The 20-byte HMAC-SHA1 starts at offset 36, and the 64-byte ciphertext starts at offset 56. The pinned receiver derives the unwrap and authentication parameters from the completed SAP exchange. It decrypts with AES-CBC and verifies the HMAC over the first 36 bytes and recovered material before releasing any control material. A failed envelope consumes the context. [Pinned SAP control envelope code](https://github.com/jkiddo/go-atv/blob/f47a3ffcfa9458aac40562d73c314de1447df013/internal/fairplay/session.go)
- The four 16-byte slices of control material hold the client-write key and initial nonce followed by the client-read key and initial nonce. The receiver assigns the client-write state to receive and the client-read state to transmit. The pair-bound profile takes the first 16 bytes of SHA-512 over each key followed by the fresh Pair-Verify shared secret. The host wrapper uses the iPhoneMirror direction mapping and calls the pinned primitives directly so it can also reject identical directional keys and exhausted initial nonces. [Pinned iPhoneMirror receiver mapping](https://github.com/RayrenSX/iPhoneMirror/blob/5e4443ceac899a3b0a7df4d92696cefc01673c6a/third_party/airplay-control/receiver/state.go) · [Pinned key derivation](https://github.com/jkiddo/go-atv/blob/f47a3ffcfa9458aac40562d73c314de1447df013/internal/fpcontrol/control.go)
- Each record starts with a 4-byte big-endian payload length, also used as AES-GCM associated data. It is followed by ciphertext of that length and a 16-byte authentication tag. Each direction increments its own 128-bit big-endian nonce before every record, checks for overflow and rejects reuse through the next authentication check. Payloads above 256 KiB are rejected before allocation. Socket reads consume exact record lengths and retain buffered bytes across the upgrade. An HTTP request may span records, and a TCP read can contain several records. [Pinned framing and vectors](https://github.com/jkiddo/go-atv/tree/f47a3ffcfa9458aac40562d73c314de1447df013/internal/fpcontrol)
- The socket has a 30-second host test deadline. HTTP headers are capped at 8 KiB and plaintext request bodies at 164 bytes. The parser rejects ambiguous lengths, transfer encodings, nested upgrades and malformed framing. It dispatches only the exact playback-info method and target after encryption. The first plaintext response after validation is the upgrade acknowledgment. Every successful control response is encrypted.
- Per-socket diagnostics use numeric connection IDs, fixed stage and reason codes, parsed envelope class and length, a fixed successful endpoint label and record counts. They omit headers, queries, signatures, keys and payloads. Teardown clears owned key material, request bodies and pending plaintext where feasible, and drops cryptographic objects. Go's standard crypto objects and the pinned SAP object provide no explicit zeroization API, and the Go runtime can retain copies.

## Reproduce the interoperability test

Use Go 1.25+, Python 3 and Git. Clone the two reference repositories locally with the pinned commits available, then run from the UxAir Dev worktree.

```sh
python tests/airplay_control_phase1/check.py /path/to/go-atv /path/to/iPhoneMirror \
  --arm-output build/phase1-test.arm
```

The runner verifies both commit IDs and the matching Git trees for `internal/fairplay` and `internal/fpcontrol`. It builds against an archive of the pinned iPhoneMirror subset in a temporary Go workspace with module downloads disabled. It runs the Go suites for the reference subset and the UxAir host module. Use `IPHONE_MIRROR_TEST_FFMPEG` to include the optional upstream media fixtures. A host C compiler compatible with Go's cgo toolchain enables the optional `--cgo` or `--race` run.

The test client uses real TCP on loopback, builds and validates the legacy Pair-Verify transcripts independently, then uses the pinned go-atv sender SAP and record primitives. It requires the empty plaintext upgrade response, authenticates the receiver's encrypted response with the independent client-read state, and parses the expected idle plist. It also covers optimistic coalescing at the upgrade boundary, fragmented records, multiple records per write and six simultaneous clients. Rejection cases cover an unregistered controller, invalid or cross-socket proofs, missing pair-bound negotiation, out-of-order SAP, bad SAP confirmation, malformed or cross-socket envelopes, matching keys, exhausted nonces, replay, skipped counters, wrong direction, truncated and oversized records, modified ciphertext and tags, post-upgrade plaintext, malicious HTTP framing and a reset during Pair-Verify. Another test reuses the same raw envelope material across separately authenticated sockets and verifies that the pair-bound keys still isolate the records.

## Verification results

| Check | Result |
| --- | --- |
| UxAir Phase 1 Go TCP suite | Passed on Windows amd64 with Go 1.25.5. The normal test authenticated two responses and counted three inbound records and two outbound records. All rejection and concurrency cases passed. |
| Pinned iPhoneMirror pure-Go suite | Passed with Go 1.25.5 and the host FFmpeg 9.0.2, including SAP vectors, record vectors, receiver and FCUP/media tests. The Windows host lacked a compatible cgo compiler, so the C ABI and Go race builds remain untested. |
| ARMv7 Linux cross-build | Passed with `GOARM=7,softfloat` and `CGO_ENABLED=0`. The test executable is little-endian ELF32 ARM EABI v5 and has no dynamic section or interpreter. Execution on ARM and the webOS launcher remain unverified. |
| Native portable ASan tests | Passed with MSVC for session lifecycle, now-playing, diagnostics, RAOP audio session, media-open failure, HEVC startup and configuration bounds, UI navigation, PIN pairing UI, and the AirPlay structural trace test with libplist 2.7.0. |
| Existing host scripts | Bash syntax and release metadata validation passed. The full Linux ASan/UBSan suite needs a Linux host and its compiler and multimedia development packages. This Windows host had neither WSL nor the required POSIX compiler. |

## webOS 3.9 compatibility and next phases

An ARMv7 soft-float Linux test executable exercises the host component's compile-time portability. A production decision still needs the actual B7 kernel version and ABI, launcher sandbox checks, thread and memory measurements and, for a Go shared component, a supported ARM32 cgo configuration. A native port would need the same interop vectors and ABI checks with the legacy sysroot and shipped OpenSSL. The Phase 1 change is outside the native CMake, launcher, packaging and discovery paths. [Go on Linux supported kernel versions](https://go.dev/wiki/Linux)

1. **Phase 2.** Connect the validated transport to UxAir Dev's registered-controller and dedicated video HTTP paths. Keep plaintext parser ownership, buffered bytes and the upgrade boundary on the same socket. Verify the actual Safari envelope shape, selected pairing mode and first authenticated HTTP endpoint on the B7 using structural diagnostics.
2. **Phase 3.** Implement authenticated `/play`, URL validation and video-session ownership. Determine whether command and reverse sockets need to be coordinated before admission.
3. **Phase 4.** Send an admitted clear URL to Starfish Native URI or HLS BUFFERSTREAM and verify moving picture and sound on hardware.
4. **Phase 5.** Implement required encrypted reverse events, playback status and controls, and FCUP/HLS when required by observed senders. Reverse events or FCUP may be prerequisites for playback on some senders.
