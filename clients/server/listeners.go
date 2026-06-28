package main

// ListenerManager owns the runtime start/stop of TCP implant listeners.
//
// The operator HTTP/WS listener is NOT managed here (it's the control plane —
// stopping it would lock the operator out of the core). Only TCP implant
// transport listeners are managed. The default boot listener (port from CLI
// arg) is created through here too, so the whole TCP set is uniform.
//
// Each Listener holds the net.Listener + a done channel so its accept loop can
// be stopped cleanly (ln.Close() makes Accept() return an error; the loop
// checks done to distinguish "stopped" from a real error).

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"net"
	"sync"
	"time"
)

// Listener is one managed TCP implant listener.
type Listener struct {
	ID       string
	Name     string
	Protocol string
	BindIP   string
	Port     string
	Active   bool

	ln   net.Listener
	done chan struct{}
}

// ListenerManager holds the live set of implant listeners.
type ListenerManager struct {
	mu      sync.Mutex
	byID    map[string]*Listener
	manager *SessionManager // to hand accepted conns to
	store   *Store          // persist config (may be nil if persistence off)
	bus     *EventBus       // publish listener_changed
}

func NewListenerManager(sm *SessionManager, st *Store, bus *EventBus) *ListenerManager {
	return &ListenerManager{
		byID:    map[string]*Listener{},
		manager: sm,
		store:   st,
		bus:     bus,
	}
}

// listenerChanged publishes a listener_changed event so the GUI refetches.
func (lm *ListenerManager) listenerChanged() {
	if lm.bus == nil {
		return
	}
	lm.bus.Publish(Event{Type: "listener_changed", ImplantID: 0, Data: "listeners changed"})
}

// newID returns a short hex id (8 chars).
func newListenerID() string {
	var b [4]byte
	_, _ = rand.Read(b[:])
	return "ln-" + hex.EncodeToString(b[:])
}

// Create starts a new TCP listener and registers it. Returns the new listener.
func (lm *ListenerManager) Create(name, bindIP, port string) (*Listener, error) {
	if bindIP == "" {
		bindIP = "0.0.0.0"
	}
	if port == "" {
		return nil, fmt.Errorf("port required")
	}
	addr := net.JoinHostPort(bindIP, port)
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return nil, fmt.Errorf("listen %s: %w", addr, err)
	}
	l := &Listener{
		ID:       newListenerID(),
		Name:     name,
		Protocol: "tcp",
		BindIP:   bindIP,
		Port:     port,
		Active:   true,
		ln:       ln,
		done:     make(chan struct{}),
	}
	lm.mu.Lock()
	lm.byID[l.ID] = l
	lm.mu.Unlock()

	go lm.acceptLoop(l)

	if lm.store != nil {
		_ = lm.store.PutListener(ListenerRow{
			ID: l.ID, Name: l.Name, Protocol: l.Protocol,
			BindIP: l.BindIP, Port: l.Port, Active: true, CreatedTS: nowUnix(),
		})
	}
	lm.listenerChanged()
	return l, nil
}

// acceptLoop hands accepted implant conns to the SessionManager. Stops when
// the listener is closed (either via Stop, or a fatal Accept error).
func (lm *ListenerManager) acceptLoop(l *Listener) {
	for {
		conn, err := l.ln.Accept()
		if err != nil {
			select {
			case <-l.done:
				// clean stop — exit silently
				return
			default:
				fmt.Fprintf(logf(), "[core] listener %s accept: %v\n", l.ID, err)
				return
			}
		}
		lm.manager.Accept(conn)
	}
}

// Stop closes a listener's socket (new dials fail) but keeps its config so it
// can be Start-ed again.
func (lm *ListenerManager) Stop(id string) error {
	lm.mu.Lock()
	l, ok := lm.byID[id]
	lm.mu.Unlock()
	if !ok {
		return fmt.Errorf("no such listener %s", id)
	}
	lm.closeLocked(l)
	l.Active = false
	if lm.store != nil {
		_ = lm.store.SetListenerActive(id, false)
	}
	lm.listenerChanged()
	return nil
}

// closeLocked closes the listener socket + signals the accept loop. Caller
// manages l.Active + persistence + event (so Stop/Start/Delete can share it).
func (lm *ListenerManager) closeLocked(l *Listener) {
	if l.ln == nil {
		return
	}
	select {
	case <-l.done:
		// already closed
	default:
		close(l.done)
	}
	_ = l.ln.Close()
	l.ln = nil
}

// Start re-opens a stopped listener on its configured port.
func (lm *ListenerManager) Start(id string) error {
	lm.mu.Lock()
	l, ok := lm.byID[id]
	if !ok {
		lm.mu.Unlock()
		return fmt.Errorf("no such listener %s", id)
	}
	if l.Active || l.ln != nil {
		lm.mu.Unlock()
		return fmt.Errorf("listener %s already running", id)
	}
	addr := net.JoinHostPort(l.BindIP, l.Port)
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		lm.mu.Unlock()
		return fmt.Errorf("listen %s: %w", addr, err)
	}
	l.ln = ln
	l.done = make(chan struct{})
	l.Active = true
	lm.mu.Unlock()

	go lm.acceptLoop(l)

	if lm.store != nil {
		_ = lm.store.SetListenerActive(id, true)
	}
	lm.listenerChanged()
	return nil
}

// Update changes name/bind_ip/port. If the port or bind_ip changed and the
// listener was active, it's restarted on the new address.
func (lm *ListenerManager) Update(id, name, bindIP, port string) (*Listener, error) {
	lm.mu.Lock()
	l, ok := lm.byID[id]
	lm.mu.Unlock()
	if !ok {
		return nil, fmt.Errorf("no such listener %s", id)
	}
	wasActive := l.Active
	addrChanged := (port != "" && port != l.Port) || (bindIP != "" && bindIP != l.BindIP)
	if name != "" {
		l.Name = name
	}
	if bindIP != "" {
		l.BindIP = bindIP
	}
	if port != "" {
		l.Port = port
	}
	if addrChanged && wasActive {
		lm.closeLocked(l)
		l.Active = false
		// re-open on new address
		addr := net.JoinHostPort(l.BindIP, l.Port)
		ln, err := net.Listen("tcp", addr)
		if err != nil {
			if lm.store != nil {
				_ = lm.store.PutListener(ListenerRow{ID: l.ID, Name: l.Name, Protocol: l.Protocol, BindIP: l.BindIP, Port: l.Port, Active: false, CreatedTS: nowUnix()})
			}
			lm.listenerChanged()
			return l, fmt.Errorf("listen %s: %w", addr, err)
		}
		l.ln = ln
		l.done = make(chan struct{})
		l.Active = true
		go lm.acceptLoop(l)
	}
	if lm.store != nil {
		_ = lm.store.PutListener(ListenerRow{ID: l.ID, Name: l.Name, Protocol: l.Protocol, BindIP: l.BindIP, Port: l.Port, Active: l.Active, CreatedTS: nowUnix()})
	}
	lm.listenerChanged()
	return l, nil
}

// Delete stops + removes a listener entirely.
func (lm *ListenerManager) Delete(id string) error {
	lm.mu.Lock()
	l, ok := lm.byID[id]
	if ok {
		delete(lm.byID, id)
	}
	lm.mu.Unlock()
	if !ok {
		return fmt.Errorf("no such listener %s", id)
	}
	lm.closeLocked(l)
	if lm.store != nil {
		_ = lm.store.DeleteListener(id)
	}
	lm.listenerChanged()
	return nil
}

// ListInfo returns the current set for the REST surface.
func (lm *ListenerManager) ListInfo() []ListenerRow {
	lm.mu.Lock()
	defer lm.mu.Unlock()
	out := make([]ListenerRow, 0, len(lm.byID))
	for _, l := range lm.byID {
		out = append(out, ListenerRow{
			ID: l.ID, Name: l.Name, Protocol: l.Protocol,
			BindIP: l.BindIP, Port: l.Port, Active: l.Active, CreatedTS: 0,
		})
	}
	return out
}

// Restore re-opens listeners from persisted config at boot. Active ones are
// re-listened; inactive ones are registered but not opened.
func (lm *ListenerManager) Restore(rows []ListenerRow) {
	for _, r := range rows {
		l := &Listener{
			ID: r.ID, Name: r.Name, Protocol: r.Protocol,
			BindIP: r.BindIP, Port: r.Port, Active: false,
			done: make(chan struct{}),
		}
		if r.Active {
			addr := net.JoinHostPort(r.BindIP, r.Port)
			ln, err := net.Listen("tcp", addr)
			if err != nil {
				fmt.Fprintf(logf(), "[core] restore listener %s on %s: %v (registered inactive)\n", r.ID, addr, err)
				if lm.store != nil {
					_ = lm.store.SetListenerActive(r.ID, false)
				}
			} else {
				l.ln = ln
				l.Active = true
				go lm.acceptLoop(l)
			}
		}
		lm.mu.Lock()
		lm.byID[l.ID] = l
		lm.mu.Unlock()
	}
}

func nowUnix() int64 {
	return time.Now().Unix()
}
