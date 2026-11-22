# UxAir 1.4.0-dev B7 acceptance

Use the Starfish development IPK from the GitHub Actions artifact and the matching source archive for the scripts. Close any active UxAir receiver from the TV UI to give the development app exclusive use of the decoder. Installation uses the separate application ID com.github.depius.uxair.dev and its own state directory.

Set the fixture address to your PC's LAN IPv4. The example uses 192.168.1.89 for the PC and 192.168.1.27 for the B7. Allow inbound TCP 8080 on the private LAN for the fixture. You need a configured ares device named b7, root SSH, Python, and FFmpeg with libx264 and AAC.

In the extracted source directory, run the fixture in one PowerShell terminal.

```powershell
$fixtureIp = '192.168.1.89'
python tools/direct-hls-fixture.py --bind $fixtureIp --port 8080
```

In a second terminal, verify and install the development IPK. Compare the hash with SHA256SUMS in the artifact.

```powershell
$tvIp = '192.168.1.27'
$fixtureIp = '192.168.1.89'
$ipk = 'com.github.depius.uxair.dev_1.4.0_arm-webos3-4.ipk'
Get-FileHash $ipk -Algorithm SHA256
ares-install -d b7 $ipk
scp tools/run-direct-b7.sh "root@${tvIp}:/tmp/uxair-direct-test.sh"
ssh "root@$tvIp" "sh /tmp/uxair-direct-test.sh $fixtureIp auto"
```

Leave the launcher running. Its default tries Native URI and falls back to BUFFERSTREAM when it has no confirmed startup. The same IPK accepts the launcher argument bufferstream for a direct hardware-feed check, or native for an isolated URI check.

1. In a third PowerShell terminal, read the AirPlay receiver port from the sanitized startup line and run the control client.

   ```powershell
   $tvIp = '192.168.1.27'
   $fixtureIp = '192.168.1.89'
   ssh "root@$tvIp" 'for r in /media/developer/apps/usr/palm/applications /media/cryptofs/apps/usr/palm/applications /media/developer/apps/var/lib; do p=$r/com.github.depius.uxair.dev/state/uxair.log; if [ -f "$p" ]; then grep "UxAir ready port=" "$p" | tail -1; fi; done'
   $port = 45607 # Replace with the port from the current startup line.
   python tools/direct-control-test.py --receiver "http://${tvIp}:$port" --source "http://${fixtureIp}:8080/master.m3u8"
   ```

2. Check for moving color bars and a steady tone. Check pause, resume, seek and stop. BUFFERSTREAM seeks to a segment boundary, so its landing position is approximate. Repeat with the same app and check that Magic Remote OK toggles diagnostics and Back returns to the app UI.
3. On the iPhone, open the fixture's web page at http://192.168.1.89:8080/ using your PC address. Start the video, open its native AirPlay control and choose **UxAir Dev**. Check picture, sound and sync. Try locking the phone, then pause, seek, resume and stop. Check that screen mirroring and AirPlay music still start afterward.
4. If playback fails, report the backend, what you saw and heard, the fixture's role/status lines and the output below. This filter selects bounded, URL-free Direct Video diagnostic records.

   ```powershell
   ssh "root@$tvIp" 'for r in /media/developer/apps/usr/palm/applications /media/cryptofs/apps/usr/palm/applications /media/developer/apps/var/lib; do p=$r/com.github.depius.uxair.dev/state/uxair.log; if [ -f "$p" ]; then grep -E "^\[(direct-player|direct-bufferstream|direct-protocol|hls-relay)\]" "$p" | tail -100; fi; done'
   ```

The optional six-case native probe compares three loopback HLS contracts, two direct-LAN HLS contracts and one direct-LAN MP4 fixture. Close the running development receiver before launching it. Match the candidate to the visible picture and audible tone, since firmware callbacks alone cannot confirm either.

```powershell
scp tools/run-native-uri-probe-b7.sh "root@${tvIp}:/tmp/uxair-native-uri-probe.sh"
ssh "root@$tvIp" "sh /tmp/uxair-native-uri-probe.sh $fixtureIp"
```
