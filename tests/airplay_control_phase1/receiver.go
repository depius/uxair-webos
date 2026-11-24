// Package phase1 implements the isolated UxAir encrypted video control host endpoint.
package phase1

import (
	"bufio"
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"sync/atomic"
	"time"

	"github.com/jkiddo/go-atv/internal/fairplay"
	"github.com/jkiddo/go-atv/internal/fpcontrol"
)

const controlEnvelopeSize = 120

var errEnvelope = errors.New("upgrade-envelope")

const idlePlayback = `<?xml version="1.0"?><plist version="1.0"><dict><key>duration</key><real>0</real><key>position</key><real>0</real><key>rate</key><real>0</real><key>readyToPlay</key><false/></dict></plist>`

type Receiver struct {
	signingKey ed25519.PrivateKey
	registered [][ed25519.PublicKeySize]byte
	log        func(string)
	nextID     atomic.Uint64
}

// NewReceiver copies the receiver seed and the already registered signing keys.
// If supplied, log must support concurrent calls. It receives fixed diagnostic
// codes and counts with no peer-supplied values other than bounded lengths.
func NewReceiver(seed []byte, registered [][ed25519.PublicKeySize]byte, log func(string)) (*Receiver, error) {
	if len(seed) != ed25519.SeedSize || len(registered) == 0 {
		return nil, errors.New("invalid registered pairing configuration")
	}
	return &Receiver{
		signingKey: ed25519.NewKeyFromSeed(seed),
		registered: append([][ed25519.PublicKeySize]byte(nil), registered...),
		log:        log,
	}, nil
}

type connection struct {
	receiver *Receiver
	socket   net.Conn
	wire     *bufio.Reader
	id       uint64
	stage    string
	rx, tx   uint64
}

// Serve owns one accepted socket until EOF, deadline expiry or protocol failure.
// Each call uses independent cryptographic state and can run concurrently.
func (r *Receiver) Serve(socket net.Conn) (err error) {
	s := connection{
		receiver: r,
		socket:   socket,
		wire:     bufio.NewReaderSize(socket, maxHTTPHeader),
		id:       r.nextID.Add(1),
		stage:    "pair-start",
	}
	s.log("opened")
	defer func() {
		socket.Close()
		reason := "eof"
		if err != nil {
			reason = err.Error()
		}
		s.log(fmt.Sprintf("closed stage=%s reason=%s rx=%d tx=%d", s.stage, reason, s.rx, s.tx))
		clearBuffered(s.wire)
	}()
	// Bound the lifetime of this host fixture, including partial requests.
	if err := socket.SetDeadline(time.Now().Add(30 * time.Second)); err != nil {
		return errors.New("socket-deadline")
	}
	secret, err := s.verify()
	if err != nil {
		return err
	}
	defer clear(secret)
	s.stage = "pair-verified"
	s.log(s.stage)

	input, output, err := s.upgrade(secret)
	clear(secret)
	if err != nil {
		return err
	}
	return s.control(input, output)
}

func (s *connection) log(event string) {
	if s.receiver.log != nil {
		s.receiver.log(fmt.Sprintf("airplay-phase1 connection=%d %s", s.id, event))
	}
}

func (s *connection) expect(method, target string) (request, error) {
	req, err := readRequest(s.wire)
	if err != nil {
		if err == errHTTP {
			return request{}, err
		}
		return request{}, errors.New("handshake-read")
	}
	if req.method != method || req.target != target {
		clear(req.body)
		return request{}, errors.New("handshake-sequence")
	}
	return req, nil
}

func (s *connection) write(data []byte) error {
	if n, err := s.socket.Write(data); err != nil || n != len(data) {
		return errors.New("socket-write")
	}
	return nil
}

func (s *connection) respond(body []byte) error {
	contentType := ""
	if len(body) > 0 {
		contentType = "application/octet-stream"
	}
	data := response(contentType, body)
	defer clear(data)
	return s.write(data)
}

func (s *connection) upgrade(secret []byte) (*fpcontrol.Records, *fpcontrol.Records, error) {
	sap, err := fairplay.NewReceiver(rand.Reader)
	if err != nil {
		return nil, nil, errors.New("sap-entropy")
	}
	for _, stage := range []string{"sap-m1", "sap-m3"} {
		s.stage = stage
		req, err := s.expect("POST", "/fp-setup")
		if err != nil {
			return nil, nil, err
		}
		reply, err := sap.Exchange(req.body)
		clear(req.body)
		if err != nil {
			return nil, nil, errors.New("sap-invalid")
		}
		err = s.respond(reply)
		clear(reply)
		if err != nil {
			return nil, nil, err
		}
		s.log(stage + "-accepted")
	}
	s.stage = "fp-setup2"
	req, err := s.expect("POST", "/fp-setup2")
	if err != nil {
		return nil, nil, err
	}
	defer clear(req.body)
	shape := "unrecognized"
	if len(req.body) == controlEnvelopeSize &&
		bytes.Equal(req.body[:16], []byte("FPLY\x01\x02\x01\x00\x00\x00\x00\x6c\x00\x00\x00\x00")) &&
		binary.BigEndian.Uint32(req.body[32:36]) == 64 {
		shape = "fply-v1-2"
	}
	s.log(fmt.Sprintf("fp-setup2 shape=%s length=%d", shape, len(req.body)))
	material, err := sap.Unwrap(req.body)
	defer clear(material[:])
	if err != nil {
		return nil, nil, errEnvelope
	}
	// Separate directional keys even if a registered peer supplies weak material.
	var exhausted [16]byte
	for i := range exhausted {
		exhausted[i] = 0xff
	}
	if bytes.Equal(material[:16], material[32:48]) ||
		bytes.Equal(material[16:32], exhausted[:]) || bytes.Equal(material[48:], exhausted[:]) {
		return nil, nil, errors.New("upgrade-material")
	}
	// The client's first/write state is the receiver's inbound state.
	input, output, err := fpcontrol.Sender(material, secret, true)
	if err != nil {
		return nil, nil, errors.New("upgrade-derive")
	}
	if err := s.respond(nil); err != nil {
		return nil, nil, err
	}
	s.stage = "encrypted"
	s.log("upgrade-accepted")
	return input, output, nil
}

func clearBuffered(reader *bufio.Reader) {
	pending, _ := reader.Peek(reader.Buffered())
	clear(pending)
}

type recordsReader struct {
	connection *connection
	input      *fpcontrol.Records
	pending    []byte
}

func (r *recordsReader) Read(dst []byte) (int, error) {
	for len(r.pending) == 0 {
		var header [4]byte
		if _, err := io.ReadFull(r.connection.wire, header[:]); err != nil {
			if err == io.EOF {
				return 0, err
			}
			return 0, errors.New("record-header-read")
		}
		n := binary.BigEndian.Uint32(header[:])
		if n > fpcontrol.MaxPayload {
			return 0, errors.New("record-size")
		}
		record := make([]byte, int(n)+20)
		copy(record, header[:])
		if _, err := io.ReadFull(r.connection.wire, record[4:]); err != nil {
			return 0, errors.New("record-body-read")
		}
		pending, err := r.input.Open(record)
		if err != nil {
			return 0, errors.New("record-authentication")
		}
		r.pending = pending
		r.connection.rx++
	}
	n := copy(dst, r.pending)
	clear(r.pending[:n])
	r.pending = r.pending[n:]
	return n, nil
}

func (s *connection) control(input, output *fpcontrol.Records) error {
	records := &recordsReader{connection: s, input: input}
	plaintext := bufio.NewReaderSize(records, maxHTTPHeader)
	defer func() {
		clear(records.pending)
		clearBuffered(plaintext)
	}()
	for {
		req, err := readRequest(plaintext)
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}
		if req.method != "GET" || req.target != "/playback-info" || len(req.body) != 0 {
			clear(req.body)
			return errors.New("control-request")
		}
		if s.tx == 0 {
			s.log("first-request method=GET endpoint=/playback-info")
		}
		data := response("text/x-apple-plist+xml", []byte(idlePlayback))
		record, err := output.Seal(data)
		clear(data)
		if err != nil {
			return errors.New("record-seal")
		}
		if err := s.write(record); err != nil {
			return err
		}
		s.tx++
	}
}
