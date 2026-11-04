# Milestone 4D: development-only `/info` handshake experiment

> Historical development record. See [UxAir 1.2.0 scope](UXAIR_1_2_SCOPE.md) for current release and validation status.

## What the B7 capture proves

The 4C capture shows a bodyless `HTTP/1.1 GET` in the `info` path category for an ordinary photo, a 4K photo and a 4K HDR video. The SDR video produced no probe record. The old `result=0` means UxPlay returned at its no-`CSeq`/no-HLS gate without building a response. Since the 4C path label intentionally hid queries, that capture does **not** prove the URL was exactly `/info`. It also does not show whether the sender would proceed after an answer.

The pinned [UxPlay dispatcher](https://github.com/FDH2/UxPlay/blob/9f3c2bbc658645533fa1057e76c29ec8c947fa0f/lib/raop.c#L223-L249) treats requests with `CSeq` as RAOP/RTSP, and rejects HTTP direct-media requests before routing when HLS is disabled. Its [RTSP `/info` branch](https://github.com/FDH2/UxPlay/blob/9f3c2bbc658645533fa1057e76c29ec8c947fa0f/lib/raop.c#L377-L450) uses a binary-plist writer; the HTTP branch instead recognizes `/server-info`, `/playback-info` and HLS-related routes, but not `/info`. The pinned [RTSP info writer](https://github.com/FDH2/UxPlay/blob/9f3c2bbc658645533fa1057e76c29ec8c947fa0f/lib/raop_handlers.h#L32-L151) already has a no-`CSeq` response form with the DNS-SD feature mask, receiver identity, public key, model and source version. This provides a limited, existing response shape for the experiment. Its ancillary keep-alive/status fields are inherited from UxPlay and have **not** been independently verified on the B7. The legacy [HTTP `/server-info` writer](https://github.com/FDH2/UxPlay/blob/9f3c2bbc658645533fa1057e76c29ec8c947fa0f/lib/http_handlers.h#L44-L104) is unsuitable: it hardcodes `0x27F`, including direct video and HLS features this app does not support.

The app registers `_raop._tcp` and `_airplay._tcp` on the same UxPlay HTTP server port. Mirroring and audio use the RTSP/RAOP path and existing pairing; the observed Photos requests use HTTP/1.1 without `CSeq` or a session header. The development receiver's DNS-SD direct-video, HLS and HEVC feature bits stay clear. This experiment does **not** claim full AirPlay 2 support or direct-media playback. A sender may stop after reading the feature mask, require other protocol exchanges, or never send a request for a particular media item. The inherited mask's photo-related bits are examined in the 4E audit below.

## Exact development-build change

Only the separately identified `.dev` package generates an altered copy of the checksum-pinned dispatcher. Before the existing HLS rejection, it answers **only** `GET /info HTTP/1.1` with no body, no `Content-Type`, no `CSeq` and no `X-Apple-Session-ID`. It uses UxPlay's existing binary-plist info writer and responds `HTTP/1.1 200 OK` with `Content-Type: application/x-apple-binary-plist`, a body length and `Server`. If response creation fails, the old rejection remains; if serialization fails, it returns 500. Other methods, URLs (including queries), body-bearing requests and RTSP requests keep their old routes. The handler does not enable HLS, fetch remote URLs, receive photo bytes or decode direct media. Production packages do not compile the experiment.

`[direct-probe]` output remains limited to 192 fixed-vocabulary records per process. `result=200` means a response was **built and handed to the server**, not that the iPhone received or accepted it. `result=0` means no response was built at the unsupported request gate. `result=500` means info serialization failed. The new `info_query` field is one of `none`, `txt-airplay`, `txt-raop`, `other` or `na`; it never contains a raw query. Do not share the full application log, which can contain older non-probe messages.

## Repeat the four iPhone Photos tests

1. Download the 4D development artifact from the successful ARMv7 workflow run, verify its `SHA256SUMS` and `ELF_REPORT.txt`, and keep a copy of the [known-good 4C development IPK and checksum](https://github.com/depius/uxair-webos/actions/runs/38771751448) for rollback. The 4C artifact is retained by GitHub Actions for 14 days from its run; save it locally before expiry. Close the current app and install the 4D `.dev` IPK manually through webOS Dev Manager. The app ID and private pairing data path are unchanged; avoid uninstalling the app unless you have backed up its private state.
2. Confirm the iPhone and TV are on the same network. Download iCloud assets to the phone before testing. Use **Photos → Share → AirPlay** or the Photos video AirPlay control, never Control Center Screen Mirroring. Use one fresh app launch per case: ordinary JPEG/HEIC photograph, photograph whose source is at least 4K, SDR video, and 4K HDR video. Wait for success, failure or sender timeout and note which occurred. Source metadata does not prove what was transmitted.
3. Extract only the sanitized markers after each case. On a Bash host with rooted-TV SSH access, `TV_HOST=... TV_KEY=... tools/tv.sh direct-probe-logs` writes `build/logs/direct-probe.log`; copy that file to a distinct name before relaunching. On Windows PowerShell, set `$devLog` as below and run the command after each case, changing the output filename:

   ```powershell
   $devLog = '/var/palm/jail/com.github.danlflh.uxplaywebos.dev/media/developer/apps/usr/palm/applications/com.github.danlflh.uxplaywebos.dev/uxplay.log'
   ssh -i $env:TV_KEY -o StrictHostKeyChecking=yes "root@$($env:TV_HOST)" "grep '^\[direct-probe\]' '$devLog' || true" | Set-Content -Encoding utf8 .\direct-probe-photo.txt
   ```

4. For each case, report whether Photos listed the receiver, whether the share attempt ended or fell back, and the `[direct-probe]` lines. In particular, look for `method=GET path=info ... result=200 info_query=none`. If `info_query` is anything else, this intentionally strict experiment did not answer that URL. Do not interpret absent markers as proof that Photos made no network request.
5. Separately confirm known-good 1080p/30 H.264 Screen Mirroring, stereo audio, pairing and remote controls. If the experiment changes behavior adversely, close the dev app and reinstall the saved 4C `.dev` IPK with webOS Dev Manager, then relaunch. Do not deploy either package automatically. Keep and verify the rollback package checksum independently.

Stop here and review the four captures before any direct-media decoder work or 4K/HDR enablement.

## Owner follow-up and 4E audit

On the B7, the 4D package logged `result=200 info_query=none` for a 4K photo, SDR video and 4K HDR video, with no later direct-media probe records. This proves that the server built those `/info` responses; it does not prove sender acceptance or identify a photo upload. The [4E transport and output audit](MILESTONE4E.md) also found that the inherited DNS-SD and `/info` mask retains photo-related bits 1, 5 and 13 even though this fork has no direct-photo handler.
