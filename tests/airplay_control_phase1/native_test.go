package phase1_test

import (
	"bufio"
	"bytes"
	"crypto/ecdh"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"
)

func TestNativeEncryptedDispatcher(t *testing.T) {
	binary := os.Getenv("UXAIR_NATIVE_VIDEO_TEST")
	if binary == "" {
		t.Skip("set UXAIR_NATIVE_VIDEO_TEST to the native dispatcher fixture")
	}
	command := exec.Command(binary, "--encrypted-fixture")
	input, err := command.StdinPipe()
	must(t, err)
	output, err := command.StdoutPipe()
	must(t, err)
	var diagnostics bytes.Buffer
	command.Stderr = &diagnostics
	must(t, command.Start())
	t.Cleanup(func() {
		command.Process.Kill()
		command.Wait()
		if t.Failed() {
			t.Log(diagnostics.String())
		}
	})
	ready := make(chan string, 1)
	go func() {
		scanner := bufio.NewScanner(output)
		for scanner.Scan() {
			if strings.HasPrefix(scanner.Text(), "UXAIR_PORT=") {
				ready <- scanner.Text()
			}
		}
	}()
	var port int
	select {
	case line := <-ready:
		if _, err := fmt.Sscanf(line, "UXAIR_PORT=%d", &port); err != nil || port <= 0 || port > 65535 {
			t.Fatal("invalid native fixture port")
		}
	case <-time.After(15 * time.Second):
		t.Fatal("native receiver did not open its listener")
	}
	address := &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1), Port: port}
	dial := func() *net.TCPConn {
		s, err := net.DialTCP("tcp4", nil, address)
		must(t, err)
		t.Cleanup(func() { s.Close() })
		must(t, s.SetDeadline(time.Now().Add(8*time.Second)))
		return s
	}
	raop := dial()
	raopWire := bufio.NewReader(raop)
	raopOptions := func(cseq int) {
		_, err := fmt.Fprintf(raop, "OPTIONS * RTSP/1.0\r\nCSeq: %d\r\n\r\n", cseq)
		must(t, err)
		line, err := raopWire.ReadString('\n')
		must(t, err)
		if !strings.HasPrefix(line, "RTSP/1.0 200 ") {
			t.Fatal("RAOP control stopped working")
		}
		sawSequence := false
		for {
			line, err := raopWire.ReadString('\n')
			must(t, err)
			if line == "\r\n" {
				break
			}
			sawSequence = sawSequence || line == fmt.Sprintf("CSeq: %d\r\n", cseq)
		}
		if !sawSequence {
			t.Fatal("RAOP reply lost CSeq")
		}
	}
	raopOptions(1)

	signing := ed25519.NewKeyFromSeed(bytes.Repeat([]byte{0x42}, ed25519.SeedSize))
	makePeer := func() *peer {
		s := dial()
		p := &peer{harness: &harness{signingKey: signing}, t: t, socket: s, wire: bufio.NewReader(s)}
		p.peerKey = p.post("/pair-setup", make([]byte, 32))
		t.Cleanup(func() { clear(p.secret) })
		return p
	}
	checkReply := func(reader *bufio.Reader, code int) []byte {
		resp, err := http.ReadResponse(reader, nil)
		must(t, err)
		body, err := io.ReadAll(resp.Body)
		resp.Body.Close()
		must(t, err)
		if resp.StatusCode != code {
			t.Fatalf("got HTTP %d, want %d", resp.StatusCode, code)
		}
		return body
	}
	encryptedReply := func(p *peer, code int) []byte {
		reply, err := p.rx.Read(p.wire)
		must(t, err)
		return checkReply(bufio.NewReader(bytes.NewReader(reply)), code)
	}
	writeQuery := func(p *peer, request string, code int) []byte {
		p.write(p.seal(request))
		return encryptedReply(p, code)
	}
	requireClosed := func(p *peer) {
		_, err := p.wire.ReadByte()
		if err != io.EOF {
			t.Fatalf("expected a closed encrypted socket without extra response, got %v", err)
		}
	}

	a, b := makePeer(), makePeer()
	a.completeSAP()
	b.completeSAP()
	envelope := a.envelope(randomMaterial(t))
	b.write(post("/fp-setup2", envelope[:]))
	checkReply(b.wire, 400)
	requireClosed(b)
	// One send crosses the plaintext acknowledgment boundary and includes the
	// next record. Its HTTP message completes in a second authenticated record.
	first := a.seal(query[:19])
	a.write(append(post("/fp-setup2", envelope[:]), first[:7]...))
	a.acknowledgment()
	a.write(append(first[7:], a.seal(query[19:]+query)...))
	for range 2 {
		body := encryptedReply(a, 200)
		if !bytes.Contains(body, []byte("<key>readyToPlay</key><false/>")) {
			t.Fatal("native playback handler did not report idle")
		}
	}
	if !bytes.Contains(writeQuery(a, "GET /server-info HTTP/1.1\r\n\r\n", 200), []byte("<key>features</key>")) {
		t.Fatal("did not receive native server-info")
	}
	property, err := hex.DecodeString("62706c6973743030d101025576616c756508080b110000000000000101000000000000000300000000000000000000000000000012")
	must(t, err)
	propertyRequest := fmt.Sprintf("PUT /setProperty?isInterestedInDateRange HTTP/1.1\r\nContent-Type: application/x-apple-binary-plist\r\nContent-Length: %d\r\n\r\n", len(property))
	if body := writeQuery(a, propertyRequest+string(property), 200); !bytes.HasPrefix(body, []byte("bplist00")) {
		t.Fatal("did not receive native property response")
	}
	writeQuery(a, "GET /playback-info HTTP/1.1\r\nX-Apple-Session-ID: different-identity\r\n\r\n", 409)
	writeQuery(a, "POST /reverse HTTP/1.1\r\nConnection: Upgrade\r\nUpgrade: PTTH/1.0\r\n\r\n", 501)
	writeQuery(a, "POST /play HTTP/1.1\r\n\r\n", 501)
	writeQuery(a, "GET /secret-native-endpoint?token=private-native-query HTTP/1.1\r\n\r\n", 501)
	replay := a.seal(query)
	a.write(replay)
	encryptedReply(a, 200)
	a.write(replay)
	requireClosed(a)
	raopOptions(2)

	x, y := makePeer(), makePeer()
	ephemeral, err := ecdh.X25519().GenerateKey(rand.Reader)
	must(t, err)
	proof := x.challengeWith(ephemeral)
	y.challengeWith(ephemeral)
	y.write(post("/pair-verify", proof))
	checkReply(y.wire, 470)
	requireClosed(y)
	x.socket.Close()

	bad := makePeer()
	bad.upgrade()
	bad.write(bad.seal("GET /playback-info HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 1\r\n\r\n"))
	requireClosed(bad)
	plain := makePeer()
	plain.upgrade()
	plain.write([]byte(query))
	requireClosed(plain)
	raopOptions(3)
	raop.Close()
	disconnect := makePeer()
	disconnect.upgrade()
	_, err = io.WriteString(input, "d")
	must(t, err)
	requireClosed(disconnect)
	must(t, input.Close())
	must(t, command.Wait())
	log := diagnostics.String()
	for _, milestone := range []string{
		"stage=pair-verify result=verified", "stage=sap result=complete", "result=envelope-authenticated",
		"stage=upgrade result=encrypted", "encrypted_records_rx=1",
		"path=/playback-info authenticated=1 first=1 route=direct status=200",
		"path=/play authenticated=1 first=0 route=play-deferred status=501",
		"reason=record-authentication", "reason=encrypted-http-framing",
		"admitted=0 encrypted_attempts=5 encrypted_upgrades=4 envelope_failures=1 gcm_auth_failures=1",
	} {
		if !strings.Contains(log, milestone) {
			t.Errorf("missing native diagnostic %q", milestone)
		}
	}
	for _, private := range []string{testSession, "different-identity", "secret-native-endpoint", "private-native-query"} {
		if strings.Contains(log, private) {
			t.Errorf("native diagnostics leaked test input %q", private)
		}
	}
}
