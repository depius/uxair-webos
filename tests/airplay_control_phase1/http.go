package phase1

import (
	"bufio"
	"bytes"
	"errors"
	"io"
	"strconv"
	"strings"
)

const (
	maxHTTPHeader = 8 << 10
	maxHTTPBody   = 164
)

var errHTTP = errors.New("http-framing")

type request struct {
	method, target string
	mixed          bool
	body           []byte
}

func readLine(r *bufio.Reader, budget *int, first bool) (string, error) {
	line, err := r.ReadSlice('\n')
	switch {
	case err == io.EOF && first && len(line) == 0:
		return "", err
	case err == bufio.ErrBufferFull || err == io.EOF:
		return "", errHTTP
	case err != nil:
		return "", err
	}
	*budget -= len(line)
	if *budget < 0 || !bytes.HasSuffix(line, []byte("\r\n")) {
		return "", errHTTP
	}
	return string(line[:len(line)-2]), nil
}

func token(s string) bool {
	if s == "" {
		return false
	}
	for _, c := range s {
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' ||
			strings.ContainsRune("!#$%&'*+-.^_`|~", c)) {
			return false
		}
	}
	return true
}

func readRequest(r *bufio.Reader) (request, error) {
	budget := maxHTTPHeader
	line, err := readLine(r, &budget, true)
	if err != nil {
		return request{}, err
	}
	method, rest, _ := strings.Cut(line, " ")
	target, version, _ := strings.Cut(rest, " ")
	if !token(method) || version != "HTTP/1.1" || !strings.HasPrefix(target, "/") {
		return request{}, errHTTP
	}
	for _, c := range target {
		if c < '!' || c > '~' {
			return request{}, errHTTP
		}
	}
	req := request{method: method, target: target}
	var size int
	var sawLength bool
	for {
		line, err := readLine(r, &budget, false)
		if err != nil {
			return request{}, err
		}
		if line == "" {
			break
		}
		name, value, ok := strings.Cut(line, ":")
		if !ok || !token(name) {
			return request{}, errHTTP
		}
		value = strings.Trim(value, " \t")
		for _, c := range value {
			if c != '\t' && (c < ' ' || c > '~') {
				return request{}, errHTTP
			}
		}
		switch strings.ToLower(name) {
		case "transfer-encoding", "expect", "upgrade":
			return request{}, errHTTP
		case "content-length":
			if sawLength || value == "" || strings.IndexFunc(value, func(c rune) bool { return c < '0' || c > '9' }) >= 0 {
				return request{}, errHTTP
			}
			n, err := strconv.ParseUint(value, 10, 16)
			if err != nil || n > maxHTTPBody {
				return request{}, errHTTP
			}
			size, sawLength = int(n), true
		case "x-apple-pd":
			if req.mixed || value != "1" {
				return request{}, errHTTP
			}
			req.mixed = true
		}
	}
	if size > 0 {
		req.body = make([]byte, size)
		if _, err := io.ReadFull(r, req.body); err != nil {
			clear(req.body)
			if err == io.EOF || err == io.ErrUnexpectedEOF {
				return request{}, errHTTP
			}
			return request{}, err
		}
	}
	return req, nil
}

func response(contentType string, body []byte) []byte {
	header := "HTTP/1.1 200 OK\r\nContent-Length: " + strconv.Itoa(len(body)) + "\r\n"
	if contentType != "" {
		header += "Content-Type: " + contentType + "\r\n"
	}
	return append([]byte(header+"\r\n"), body...)
}
