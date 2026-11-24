package phase1

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/ecdh"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha512"
	"crypto/subtle"
	"errors"
)

var errPair = errors.New("pair-proof")

func pairCipher(secret []byte) cipher.Stream {
	derive := func(label string) [64]byte {
		input := append([]byte(label), secret...)
		defer clear(input)
		return sha512.Sum512(input)
	}
	key := derive("Pair-Verify-AES-Key")
	iv := derive("Pair-Verify-AES-IV")
	defer clear(key[:])
	defer clear(iv[:])
	block, _ := aes.NewCipher(key[:16])
	return cipher.NewCTR(block, iv[:16])
}

func (s *connection) verify() ([]byte, error) {
	first, err := s.expect("POST", "/pair-verify")
	if err != nil {
		return nil, err
	}
	defer clear(first.body)
	if !first.mixed || len(first.body) != 68 || !bytes.Equal(first.body[:4], []byte{1, 0, 0, 0}) {
		return nil, errPair
	}
	clientSigning := ed25519.PublicKey(first.body[36:])
	registered := 0
	for _, key := range s.receiver.registered {
		registered |= subtle.ConstantTimeCompare(key[:], clientSigning)
	}
	if registered != 1 {
		return nil, errPair
	}
	curve := ecdh.X25519()
	client, err := curve.NewPublicKey(first.body[4:36])
	if err != nil {
		return nil, errPair
	}
	our, err := curve.GenerateKey(rand.Reader)
	if err != nil {
		return nil, errors.New("pair-entropy")
	}
	secret, err := our.ECDH(client)
	if err != nil {
		return nil, errPair
	}
	stream := pairCipher(secret)
	var response [32 + ed25519.SignatureSize]byte
	ourPublic := our.PublicKey().Bytes()
	copy(response[:32], ourPublic)
	transcript := append(append([]byte(nil), ourPublic...), first.body[4:36]...)
	signature := ed25519.Sign(s.receiver.signingKey, transcript)
	stream.XORKeyStream(response[32:], signature)
	clear(signature)
	err = s.respond(response[:])
	clear(response[:])
	if err != nil {
		clear(secret)
		return nil, err
	}
	s.stage = "pair-challenge"

	err = s.checkProof(clientSigning, stream, ourPublic, first.body[4:36])
	if err != nil {
		clear(secret)
		return nil, err
	}
	if err := s.respond(nil); err != nil {
		clear(secret)
		return nil, err
	}
	return secret, nil
}

func (s *connection) checkProof(signing ed25519.PublicKey, stream cipher.Stream, ourPublic, clientPublic []byte) error {
	proof, err := s.expect("POST", "/pair-verify")
	if err != nil {
		return err
	}
	defer clear(proof.body)
	if len(proof.body) != 68 || !bytes.Equal(proof.body[:4], []byte{0, 0, 0, 0}) {
		return errPair
	}
	stream.XORKeyStream(proof.body[4:], proof.body[4:])
	transcript := append(append([]byte(nil), clientPublic...), ourPublic...)
	if !ed25519.Verify(signing, transcript, proof.body[4:]) {
		return errPair
	}
	return nil
}
