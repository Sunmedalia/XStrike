package main

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"time"

	"github.com/wailsapp/wails/v2/pkg/runtime"
)

// coreBaseURL returns the Go service core HTTP base, configurable via
// RUSTSTRIKE_CORE (default http://127.0.0.1:8091).
func coreBaseURL() string {
	if v := os.Getenv("RUSTSTRIKE_CORE"); v != "" {
		return v
	}
	return "http://127.0.0.1:8091"
}

// forwardEvents connects to the core's /ws and re-emits every event to the
// frontend as a Wails "core:event". Reconnects on drop so the UI stays live
// across core restarts.
func (a *App) forwardEvents() {
	wsURL := wsURLFromCore(a.coreURL)
	for {
		if a.ctx == nil {
			time.Sleep(500 * time.Millisecond)
			continue
		}
		err := a.dialAndForward(wsURL)
		if err != nil {
			runtime.LogErrorf(a.ctx, "core ws: %v (reconnecting)", err)
		}
		time.Sleep(2 * time.Second)
	}
}

func (a *App) dialAndForward(wsURL wsAddr) error {
	conn, err := net.DialTimeout("tcp", wsURL.host, 5*time.Second)
	if err != nil {
		return err
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(5 * time.Second))
	key := randWSKey()
	req := "GET " + wsURL.path + " HTTP/1.1\r\n" +
		"Host: " + wsURL.host + "\r\n" +
		"Upgrade: websocket\r\nConnection: Upgrade\r\n" +
		"Sec-WebSocket-Key: " + key + "\r\n" +
		"Sec-WebSocket-Version: 13\r\n\r\n"
	if _, err := conn.Write([]byte(req)); err != nil {
		return err
	}
	br := bufio.NewReader(conn)
	// read handshake
	respLine, err := br.ReadString('\n')
	if err != nil {
		return err
	}
	if respLine != "HTTP/1.1 101 Switching Protocols\r\n" {
		return fmt.Errorf("ws upgrade failed: %q", respLine)
	}
	for {
		line, err := br.ReadString('\n')
		if err != nil {
			return err
		}
		if line == "\r\n" {
			break
		}
	}
	_ = conn.SetDeadline(time.Time{}) // no deadline on the stream

	rw := bufio.NewReadWriter(br, bufio.NewWriter(conn))
	for {
		op, data, err := wsReadFrame(rw)
		if err != nil {
			return err
		}
		if op == 0x8 { // close
			return io.EOF
		}
		if op != 0x1 { // only text frames carry events
			continue
		}
		var ev CoreEvent
		if err := json.Unmarshal(data, &ev); err == nil {
			runtime.EventsEmit(a.ctx, "core:event", ev)
		}
	}
}

type wsAddr struct {
	host string
	path string
}

func wsURLFromCore(core string) wsAddr {
	// core like http://127.0.0.1:8091 -> host 127.0.0.1:8091, path /ws
	s := core
	s = trimProto(s)
	return wsAddr{host: s, path: "/ws"}
}

func trimProto(s string) string {
	for _, p := range []string{"http://", "https://", "ws://", "wss://"} {
		if len(s) > len(p) && s[:len(p)] == p {
			return s[len(p):]
		}
	}
	return s
}

// ---- minimal WS client framing (mirrors the core's ws.go) ----

func randWSKey() string {
	var b [16]byte
	for i := range b {
		b[i] = byte(time.Now().UnixNano() >> uint(i))
	}
	return base64.StdEncoding.EncodeToString(b[:])
}

var wsGUID = []byte("258EAFA5-E914-47DA-95CA-C5AB0DC85B11")

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

// keep imports used
var _ = http.Get
var _ = sha1.New
