package phase1_test

import (
	"bufio"
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/ecdh"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha512"
	"encoding/binary"
	"encoding/xml"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/jkiddo/go-atv/internal/fairplay"
	"github.com/jkiddo/go-atv/internal/fpcontrol"
	phase1 "github.com/jkiddo/go-atv/uxairphase1"
)

const query = "GET /playback-info HTTP/1.1\r\nHost: localhost\r\n\r\n"
const testSession = "test-session-id-should-not-appear-in-logs"

type harness struct {
	listener   *net.TCPListener
	receiver   *phase1.Receiver
	signingKey ed25519.PrivateKey
	peerKey    ed25519.PublicKey
	mu         sync.Mutex
	log        []string
}

type peer struct {
	*harness
	t      *testing.T
	socket *net.TCPConn
	wire   *bufio.Reader
	result <-chan error
	sap    *fairplay.Sender
	secret []byte
	tx, rx *fpcontrol.Records
}

func newHarness(t *testing.T) *harness {
	t.Helper()
	listener, err := net.ListenTCP("tcp4", &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1)})
	must(t, err)
	t.Cleanup(func() { listener.Close() })
	peerKey, receiverKey, err := ed25519.GenerateKey(rand.Reader)
	must(t, err)
	registered, signingKey, err := ed25519.GenerateKey(rand.Reader)
	must(t, err)
	h := &harness{listener: listener, signingKey: signingKey, peerKey: peerKey}
	h.receiver, err = phase1.NewReceiver(receiverKey.Seed(), [][32]byte{[32]byte(registered)}, func(s string) {
		h.mu.Lock()
		defer h.mu.Unlock()
		h.log = append(h.log, s)
	})
	must(t, err)
	clear(receiverKey)
	return h
}

func (h *harness) dial(t *testing.T) *peer {
	t.Helper()
	client, err := net.DialTCP("tcp4", nil, h.listener.Addr().(*net.TCPAddr))
	must(t, err)
	t.Cleanup(func() { client.Close() })
	must(t, client.SetDeadline(time.Now().Add(8*time.Second)))
	must(t, h.listener.SetDeadline(time.Now().Add(8*time.Second)))
	server, err := h.listener.AcceptTCP()
	must(t, err)
	result := make(chan error, 1)
	go func() { result <- h.receiver.Serve(server) }()
	p := &peer{harness: h, t: t, socket: client, wire: bufio.NewReader(client), result: result}
	t.Cleanup(func() { clear(p.secret) })
	return p
}

func must(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatal(err)
	}
}

func (p *peer) write(part []byte) {
	p.t.Helper()
	n, err := p.socket.Write(part)
	must(p.t, err)
	if n != len(part) {
		p.t.Fatal("short client write")
	}
}

func post(path string, body []byte) []byte {
	header := fmt.Sprintf("POST %s HTTP/1.1\r\nHost: localhost\r\nX-Apple-PD: 1\r\nX-Apple-Session-ID: %s\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n", path, testSession, len(body))
	return append([]byte(header), body...)
}

func readResponse(t *testing.T, r *bufio.Reader) (*http.Response, []byte) {
	t.Helper()
	reply, err := http.ReadResponse(r, nil)
	must(t, err)
	defer reply.Body.Close()
	body, err := io.ReadAll(reply.Body)
	must(t, err)
	if reply.StatusCode != 200 || reply.ContentLength != int64(len(body)) {
		t.Fatalf("response status=%d content-length=%d body-length=%d", reply.StatusCode, reply.ContentLength, len(body))
	}
	return reply, body
}

func (p *peer) post(path string, data []byte) []byte {
	p.t.Helper()
	p.write(post(path, data))
	_, reply := readResponse(p.t, p.wire)
	return reply
}

// Independently construct the client transcript and CTR stream. The receiver
// signature is checked using the key the client registered before this test.
func (p *peer) challenge() []byte {
	p.t.Helper()
	client, err := ecdh.X25519().GenerateKey(rand.Reader)
	must(p.t, err)
	return p.challengeWith(client)
}

func (p *peer) challengeWith(client *ecdh.PrivateKey) []byte {
	p.t.Helper()
	clientPublic := client.PublicKey().Bytes()
	request := append([]byte{1, 0, 0, 0}, clientPublic...)
	request = append(request, p.signingKey.Public().(ed25519.PublicKey)...)
	reply := p.post("/pair-verify", request)
	if len(reply) != 96 {
		p.t.Fatalf("pair-challenge length=%d", len(reply))
	}
	remote, err := ecdh.X25519().NewPublicKey(reply[:32])
	must(p.t, err)
	p.secret, err = client.ECDH(remote)
	must(p.t, err)
	k := sha512.Sum512(append([]byte("Pair-Verify-AES-Key"), p.secret...))
	iv := sha512.Sum512(append([]byte("Pair-Verify-AES-IV"), p.secret...))
	block, err := aes.NewCipher(k[:16])
	must(p.t, err)
	stream := cipher.NewCTR(block, iv[:16])
	stream.XORKeyStream(reply[32:], reply[32:])
	if !ed25519.Verify(p.peerKey, append(remote.Bytes(), clientPublic...), reply[32:]) {
		p.t.Fatal("receiver Pair-Verify signature did not authenticate")
	}
	signature := ed25519.Sign(p.signingKey, append(clientPublic, remote.Bytes()...))
	stream.XORKeyStream(signature, signature)
	return append([]byte{0, 0, 0, 0}, signature...)
}

func (p *peer) pair() {
	p.t.Helper()
	if body := p.post("/pair-verify", p.challenge()); len(body) != 0 {
		p.t.Fatal("pair verify acknowledgment was not empty")
	}
}

func (p *peer) completeSAP() {
	p.t.Helper()
	p.pair()
	p.exchangeSAP()
}

func (p *peer) exchangeSAP() {
	p.t.Helper()
	var err error
	p.sap, err = fairplay.NewSender(rand.Reader)
	must(p.t, err)
	m3, err := p.sap.Challenge(p.post("/fp-setup", p.sap.Start()))
	must(p.t, err)
	must(p.t, p.sap.Confirm(p.post("/fp-setup", m3)))
}

func (p *peer) envelope(material [64]byte) [120]byte {
	p.t.Helper()
	defer clear(material[:])
	envelope, err := p.sap.Wrap(material, rand.Reader)
	must(p.t, err)
	p.tx, p.rx, err = fpcontrol.Sender(material, p.secret, true)
	must(p.t, err)
	return envelope
}

func randomMaterial(t *testing.T) (material [64]byte) {
	t.Helper()
	_, err := rand.Read(material[:])
	must(t, err)
	return material
}

func (p *peer) upgrade() {
	p.t.Helper()
	p.completeSAP()
	envelope := p.envelope(randomMaterial(p.t))
	p.write(post("/fp-setup2", envelope[:]))
	p.acknowledgment()
}

func (p *peer) acknowledgment() {
	p.t.Helper()
	_, body := readResponse(p.t, p.wire)
	if len(body) != 0 {
		p.t.Fatal("upgrade acknowledgment was not empty")
	}
}

func (p *peer) seal(text string) []byte {
	p.t.Helper()
	record, err := p.tx.Seal([]byte(text))
	must(p.t, err)
	return record
}

func (p *peer) readPlayback() {
	p.t.Helper()
	decoded, err := p.rx.Read(p.wire)
	must(p.t, err)
	reply, data := readResponse(p.t, bufio.NewReader(bytes.NewReader(decoded)))
	if reply.Header.Get("Content-Type") != "text/x-apple-plist+xml" {
		p.t.Fatal("wrong playback info content type")
	}
	type plist struct {
		XMLName xml.Name   `xml:"plist"`
		Keys    []string   `xml:"dict>key"`
		Numbers []float64  `xml:"dict>real"`
		False   []struct{} `xml:"dict>false"`
	}
	var status plist
	must(p.t, xml.Unmarshal(data, &status))
	if strings.Join(status.Keys, ",") != "duration,position,rate,readyToPlay" ||
		len(status.Numbers) != 3 || status.Numbers[0] != 0 || status.Numbers[1] != 0 || status.Numbers[2] != 0 ||
		len(status.False) != 1 {
		p.t.Fatal("authenticated plaintext did not report idle playback")
	}
}

func (p *peer) finish(want string) {
	p.t.Helper()
	var err error
	select {
	case err = <-p.result:
	case <-time.After(9 * time.Second):
		p.t.Fatal("receiver did not close")
	}
	if want == "" && err != nil || want != "" && (err == nil || err.Error() != want) {
		p.t.Fatalf("receiver returned %v, want %q", err, want)
	}
	if _, err := p.wire.ReadByte(); err != io.EOF {
		p.t.Fatalf("expected connection closed with no additional HTTP response, got %v", err)
	}
}

func TestTCPInteroperability(t *testing.T) {
	h := newHarness(t)
	p := h.dial(t)
	p.upgrade()
	a, b, c := p.seal(query[:17]), p.seal(query[17:]), p.seal(query)
	// Fragment a record header and body across writes. Coalesce the next two
	// records in one write, one finishing a request and one starting another.
	p.write(a[:2])
	p.write(a[2:11])
	p.write(a[11:])
	p.write(append(b, c...))
	p.readPlayback()
	p.readPlayback()
	must(t, p.socket.CloseWrite())
	p.finish("")
	log := strings.Join(h.log, "\n")
	for _, diagnostic := range []string{
		"connection=1 opened", "pair-verified", "sap-m1-accepted", "sap-m3-accepted",
		"fp-setup2 shape=fply-v1-2 length=120", "upgrade-accepted",
		"first-request method=GET endpoint=/playback-info", "closed stage=encrypted reason=eof rx=3 tx=2",
	} {
		if !strings.Contains(log, diagnostic) {
			t.Errorf("missing diagnostic %q", diagnostic)
		}
	}
	if strings.Contains(log, testSession) {
		t.Error("diagnostics included a supplied session identifier")
	}
}

func TestTCPCoalescedUpgrade(t *testing.T) {
	p := newHarness(t).dial(t)
	p.completeSAP()
	envelope := p.envelope(randomMaterial(t))
	p.write(append(post("/fp-setup2", envelope[:]), p.seal(query)...))
	p.acknowledgment()
	p.readPlayback()
	must(t, p.socket.CloseWrite())
	p.finish("")
}

func TestTCPRejectsPairing(t *testing.T) {
	t.Run("invalid proof", func(t *testing.T) {
		p := newHarness(t).dial(t)
		proof := p.challenge()
		proof[4] ^= 1
		p.write(post("/pair-verify", proof))
		p.finish("pair-proof")
	})
	t.Run("unregistered key", func(t *testing.T) {
		p := newHarness(t).dial(t)
		ephemeral, err := ecdh.X25519().GenerateKey(rand.Reader)
		must(t, err)
		unknown, _, err := ed25519.GenerateKey(rand.Reader)
		must(t, err)
		body := append(append([]byte{1, 0, 0, 0}, ephemeral.PublicKey().Bytes()...), unknown...)
		p.write(post("/pair-verify", body))
		p.finish("pair-proof")
	})
	t.Run("proof reuse across sockets", func(t *testing.T) {
		h := newHarness(t)
		a, b := h.dial(t), h.dial(t)
		ephemeral, err := ecdh.X25519().GenerateKey(rand.Reader)
		must(t, err)
		proof := a.challengeWith(ephemeral)
		a.post("/pair-verify", proof)
		b.challengeWith(ephemeral)
		b.write(post("/pair-verify", proof))
		b.finish("pair-proof")
	})
	t.Run("session ID cannot authorize SAP", func(t *testing.T) {
		p := newHarness(t).dial(t)
		sap, err := fairplay.NewSender(rand.Reader)
		must(t, err)
		p.write(post("/fp-setup", sap.Start()))
		p.finish("handshake-sequence")
	})
	t.Run("mixed mode must be requested", func(t *testing.T) {
		p := newHarness(t).dial(t)
		ephemeral, err := ecdh.X25519().GenerateKey(rand.Reader)
		must(t, err)
		body := append(append([]byte{1, 0, 0, 0}, ephemeral.PublicKey().Bytes()...), p.signingKey.Public().(ed25519.PublicKey)...)
		data := post("/pair-verify", body)
		data = bytes.Replace(data, []byte("X-Apple-PD: 1\r\n"), nil, 1)
		p.write(data)
		p.finish("pair-proof")
	})
}

func TestTCPRejectsSAP(t *testing.T) {
	t.Run("upgrade before SAP", func(t *testing.T) {
		p := newHarness(t).dial(t)
		p.pair()
		p.write(post("/fp-setup2", make([]byte, 120)))
		p.finish("handshake-sequence")
	})
	t.Run("malformed M1", func(t *testing.T) {
		p := newHarness(t).dial(t)
		p.pair()
		p.write(post("/fp-setup", make([]byte, 16)))
		p.finish("sap-invalid")
	})
	t.Run("invalid M3 confirmation", func(t *testing.T) {
		p := newHarness(t).dial(t)
		p.pair()
		sap, err := fairplay.NewSender(rand.Reader)
		must(t, err)
		m3, err := sap.Challenge(p.post("/fp-setup", sap.Start()))
		must(t, err)
		m3[len(m3)-1] ^= 1
		p.write(post("/fp-setup", m3))
		p.finish("sap-invalid")
	})
}

func TestTCPRejectsUpgrade(t *testing.T) {
	for _, test := range []struct {
		name string
		edit func([]byte) []byte
	}{
		{"wrong length", func(b []byte) []byte { return b[:119] }},
		{"wrong version", func(b []byte) []byte { b[4] ^= 1; return b }},
		{"wrong encrypted length", func(b []byte) []byte { b[35] ^= 1; return b }},
		{"bad authentication", func(b []byte) []byte { b[36] ^= 1; return b }},
		{"modified CBC ciphertext", func(b []byte) []byte { b[56] ^= 1; return b }},
	} {
		t.Run(test.name, func(t *testing.T) {
			p := newHarness(t).dial(t)
			p.completeSAP()
			envelope := p.envelope(randomMaterial(t))
			p.write(post("/fp-setup2", test.edit(envelope[:])))
			p.finish("upgrade-envelope")
		})
	}
	t.Run("same directional keys", func(t *testing.T) {
		p := newHarness(t).dial(t)
		p.completeSAP()
		material := randomMaterial(t)
		copy(material[32:48], material[:16])
		envelope := p.envelope(material)
		p.write(post("/fp-setup2", envelope[:]))
		p.finish("upgrade-material")
	})
	t.Run("exhausted counter", func(t *testing.T) {
		p := newHarness(t).dial(t)
		p.completeSAP()
		material := randomMaterial(t)
		copy(material[16:32], bytes.Repeat([]byte{0xff}, 16))
		envelope := p.envelope(material)
		p.write(post("/fp-setup2", envelope[:]))
		p.finish("upgrade-material")
	})
	t.Run("envelope replay across sockets", func(t *testing.T) {
		h := newHarness(t)
		a, b := h.dial(t), h.dial(t)
		a.completeSAP()
		b.completeSAP()
		envelope := a.envelope(randomMaterial(t))
		a.write(post("/fp-setup2", envelope[:]))
		a.acknowledgment()
		b.write(post("/fp-setup2", envelope[:]))
		b.finish("upgrade-envelope")
		a.write(a.seal(query))
		a.readPlayback()
		must(t, a.socket.CloseWrite())
		a.finish("")
	})
}

func TestTCPRejectsEncryptedInput(t *testing.T) {
	for _, test := range []struct {
		name, reason string
		send         func(*peer)
	}{
		{"truncated header", "record-header-read", func(p *peer) {
			p.write(p.seal(query)[:3])
			must(p.t, p.socket.CloseWrite())
		}},
		{"truncated body", "record-body-read", func(p *peer) {
			record := p.seal(query)
			p.write(record[:len(record)-1])
			must(p.t, p.socket.CloseWrite())
		}},
		{"bad tag", "record-authentication", func(p *peer) {
			record := p.seal(query)
			record[len(record)-1] ^= 1
			p.write(record)
		}},
		{"bad ciphertext", "record-authentication", func(p *peer) {
			record := p.seal(query)
			record[4] ^= 1
			p.write(record)
		}},
		{"wrong direction", "record-authentication", func(p *peer) {
			record, err := p.rx.Seal([]byte(query))
			must(p.t, err)
			p.write(record)
		}},
		{"plaintext", "record-size", func(p *peer) { p.write([]byte(query)) }},
		{"oversize", "record-size", func(p *peer) {
			p.write(binary.BigEndian.AppendUint32(nil, fpcontrol.MaxPayload+1))
		}},
		{"record replay", "record-authentication", func(p *peer) {
			record := p.seal(query)
			p.write(record)
			p.readPlayback()
			p.write(record)
		}},
		{"skipped counter", "record-authentication", func(p *peer) {
			p.seal(query)
			p.write(p.seal(query))
		}},
		{"duplicate content length", "http-framing", func(p *peer) {
			p.write(p.seal("GET /playback-info HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 1\r\n\r\n"))
		}},
		{"transfer encoding", "http-framing", func(p *peer) {
			p.write(p.seal("GET /playback-info HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"))
		}},
		{"truncated authenticated HTTP", "http-framing", func(p *peer) {
			p.write(p.seal("GET /playback-info HTTP/1.1\r\nHost: localhost"))
			must(p.t, p.socket.CloseWrite())
		}},
		{"oversize HTTP headers", "http-framing", func(p *peer) {
			header := strings.Repeat("X-Test: AAAAAAAAAAAAAAAAAAAA\r\n", 400)
			p.write(p.seal("GET /playback-info HTTP/1.1\r\n" + header + "\r\n"))
		}},
		{"unmapped endpoint", "control-request", func(p *peer) {
			p.write(p.seal("GET /playback-info?secret=never-log HTTP/1.1\r\n\r\n"))
		}},
	} {
		t.Run(test.name, func(t *testing.T) {
			h := newHarness(t)
			p := h.dial(t)
			p.upgrade()
			test.send(p)
			p.finish(test.reason)
			if strings.Contains(strings.Join(h.log, "\n"), "never-log") {
				t.Error("diagnostics included a supplied query value")
			}
		})
	}
}

func TestTCPHandshakeResetAndConcurrentClients(t *testing.T) {
	h := newHarness(t)
	p := h.dial(t)
	p.challenge()
	must(t, p.socket.SetLinger(0))
	must(t, p.socket.Close())
	select {
	case err := <-p.result:
		if err == nil || err.Error() != "handshake-read" {
			t.Fatalf("expected read failure after reset, got %v", err)
		}
	case <-time.After(9 * time.Second):
		t.Fatal("receiver did not release reset connection")
	}
	for i := range 6 {
		t.Run(fmt.Sprintf("client-%d", i), func(t *testing.T) {
			p := h.dial(t)
			t.Parallel()
			p.upgrade()
			p.write(p.seal(query))
			p.readPlayback()
			must(t, p.socket.CloseWrite())
			p.finish("")
		})
	}
}

func TestTCPPairSecretSeparatesReusedMaterial(t *testing.T) {
	h := newHarness(t)
	a, b := h.dial(t), h.dial(t)
	material := randomMaterial(t)
	defer clear(material[:])
	for _, p := range []*peer{a, b} {
		p.completeSAP()
		envelope := p.envelope(material)
		p.write(post("/fp-setup2", envelope[:]))
		p.acknowledgment()
	}
	record := a.seal(query)
	b.write(record)
	b.finish("record-authentication")
	a.write(record)
	a.readPlayback()
	must(t, a.socket.CloseWrite())
	a.finish("")
}
