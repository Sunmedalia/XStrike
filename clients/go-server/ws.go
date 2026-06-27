package main

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"io"
	"net/http"
	"strings"
)

// Minimal RFC 6455 WebSocket server (no external deps): upgrade + text-frame
// write/read. Sufficient for pushing JSON Events to a GUI client.

var wsKeyGUID = []byte("258EAFA5-E914-47DA-95CA-C5AB0DC85B11")

// wsAccept computes the Sec-WebSocket-Accept value (RFC 6455 §1.3).
func wsAccept(key string) string {
	h := sha1.New()
	h.Write([]byte(key))
	h.Write(wsKeyGUID)
	return base64.StdEncoding.EncodeToString(h.Sum(nil))
}

func handleWS(w http.ResponseWriter, r *http.Request) {
	if strings.ToLower(r.Header.Get("Upgrade")) != "websocket" {
		http.Error(w, "not a websocket upgrade", http.StatusBadRequest)
		return
	}
	key := r.Header.Get("Sec-WebSocket-Key")
	if key == "" {
		http.Error(w, "missing Sec-WebSocket-Key", http.StatusBadRequest)
		return
	}
	hj, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "hijack unsupported", http.StatusInternalServerError)
		return
	}
	conn, rw, err := hj.Hijack()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	defer conn.Close()

	accept := wsAccept(key)
	resp := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + accept + "\r\n\r\n"
	if _, err := rw.WriteString(resp); err != nil {
		return
	}
	if err := rw.Flush(); err != nil {
		return
	}

	// Subscribe this connection to the event bus.
	ch := bus.Subscribe()
	defer bus.Unsubscribe(ch)

	// Reader: drain/close on client frames (we don't expect content; ping ok).
	go func() {
		for {
			if _, _, err := wsReadFrame(rw); err != nil {
				conn.Close()
				return
			}
		}
	}()

	// Writer: push Events as text frames.
	enc := json.NewEncoder(rw.Writer)
	for e := range ch {
		b, err := json.Marshal(e)
		if err != nil {
			continue
		}
		if err := wsWriteFrame(rw.Writer, opcodeText, b); err != nil {
			return
		}
		if err := rw.Flush(); err != nil {
			return
		}
	}
	_ = enc // (kept for clarity; we marshal manually above to frame raw bytes)
}

const (
	opcodeText   = 0x1
	opcodeClose  = 0x8
	opcodePing   = 0x9
	opcodePong   = 0xA
)

// wsWriteFrame writes a single (unfragmented) frame. masked=false (server->client).
func wsWriteFrame(w io.Writer, opcode byte, payload []byte) error {
	hdr := []byte{0x80 | opcode} // FIN + opcode
	masked := false
	n := len(payload)
	switch {
	case n < 126:
		hdr = append(hdr, byte(n))
	case n < 65536:
		hdr = append(hdr, 126)
		var lb [2]byte
		binary.BigEndian.PutUint16(lb[:], uint16(n))
		hdr = append(hdr, lb[:]...)
	default:
		hdr = append(hdr, 127)
		var lb [8]byte
		binary.BigEndian.PutUint64(lb[:], uint64(n))
		hdr = append(hdr, lb[:]...)
	}
	if masked {
		hdr[1] |= 0x80
	}
	if _, err := w.Write(hdr); err != nil {
		return err
	}
	_, err := w.Write(payload)
	return err
}

// wsReadFrame reads one frame from a bufio.Reader (client->server, masked).
func wsReadFrame(rw *bufio.ReadWriter) (byte, []byte, error) {
	br := rw.Reader
	h0, err := br.ReadByte()
	if err != nil {
		return 0, nil, err
	}
	h1, err := br.ReadByte()
	if err != nil {
		return 0, nil, err
	}
	opcode := h0 & 0x0f
	masked := h1&0x80 != 0
	length := int(h1 & 0x7f)
	switch length {
	case 126:
		var lb [2]byte
		if _, err := io.ReadFull(br, lb[:]); err != nil {
			return 0, nil, err
		}
		length = int(binary.BigEndian.Uint16(lb[:]))
	case 127:
		var lb [8]byte
		if _, err := io.ReadFull(br, lb[:]); err != nil {
			return 0, nil, err
		}
		length = int(binary.BigEndian.Uint64(lb[:]))
	}
	var mask [4]byte
	if masked {
		if _, err := io.ReadFull(br, mask[:]); err != nil {
			return 0, nil, err
		}
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(br, payload); err != nil {
		return 0, nil, err
	}
	if masked {
		for i := range payload {
			payload[i] ^= mask[i%4]
		}
	}
	return opcode, payload, nil
}
