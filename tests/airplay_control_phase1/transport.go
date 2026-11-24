package phase1

import (
	"bytes"
	"crypto/rand"
	"errors"

	"github.com/jkiddo/go-atv/internal/fairplay"
	"github.com/jkiddo/go-atv/internal/fpcontrol"
)

const (
	MaxRecordPayload = fpcontrol.MaxPayload
	RecordOverhead   = 20
)

var (
	ErrEnvelope = errors.New("upgrade-envelope")
	ErrMaterial = errors.New("upgrade-material")
	errState    = errors.New("transport-state")
)

// Transport owns the SAP and record state of one already verified socket.
// Its caller supplies the socket's Pair-Verify secret and serializes calls.
type Transport struct {
	sap           *fairplay.Receiver
	input, output *fpcontrol.Records
}

func NewTransport() (*Transport, error) {
	sap, err := fairplay.NewReceiver(rand.Reader)
	if err != nil {
		return nil, err
	}
	return &Transport{sap: sap}, nil
}

func (t *Transport) Exchange(body []byte) ([]byte, error) {
	if t.sap == nil {
		return nil, errState
	}
	reply, err := t.sap.Exchange(body)
	if err != nil {
		t.Close()
	}
	return reply, err
}

func (t *Transport) Upgrade(body, secret []byte) error {
	if t.sap == nil || len(secret) != 32 {
		t.Close()
		return errState
	}
	material, err := t.sap.Unwrap(body)
	t.sap = nil
	defer clear(material[:])
	if err != nil {
		return ErrEnvelope
	}
	var exhausted [16]byte
	for i := range exhausted {
		exhausted[i] = 0xff
	}
	if bytes.Equal(material[:16], material[32:48]) ||
		bytes.Equal(material[16:32], exhausted[:]) || bytes.Equal(material[48:], exhausted[:]) {
		return ErrMaterial
	}
	// The client's first/write state is the receiver's inbound state.
	t.input, t.output, err = fpcontrol.Sender(material, secret, true)
	return err
}

func (t *Transport) Open(record []byte) ([]byte, error) {
	if t.input == nil {
		return nil, errState
	}
	plain, err := t.input.Open(record)
	if err != nil {
		t.Close()
	}
	return plain, err
}

func (t *Transport) Seal(plain []byte) ([]byte, error) {
	if t.output == nil {
		return nil, errState
	}
	record, err := t.output.Seal(plain)
	if err != nil {
		t.Close()
	}
	return record, err
}

func (t *Transport) Close() {
	t.sap, t.input, t.output = nil, nil, nil
}
