package main

// RelayRegistry tracks the pivot/relay listeners running on each implant.
//
// Relay state is transient — it lives in the parent implant's process and dies
// when that implant disconnects. It is NOT persisted to SQLite (a persisted
// "running" relay after a core restart would be a lie: the listener is gone).
// The registry is the in-memory source of truth for the GUI's Pivot panel and
// the GET /api/implants/{id}/relays surface.
//
// Lifecycle: the core sends relay_listen (minting the relay_id) and records a
// "requested" entry; the implant's async relay_started/stopped/error reply
// transitions the state. On implant disconnect the whole per-implant set is
// dropped (see Cleanup).

import "sync"

// RelayState enumerates a relay listener's lifecycle.
type RelayState string

const (
	RelayRequested RelayState = "requested" // relay_listen sent, awaiting relay_started
	RelayRunning   RelayState = "running"   // relay_started received
	RelayFailed    RelayState = "failed"    // relay_error received (e.g. port taken)
	RelayStopped   RelayState = "stopped"   // relay_stopped received / unknown id
)

// RelayInfo is one relay listener on one implant.
type RelayInfo struct {
	ID        string     `json:"id"`
	ImplantID uint64     `json:"implant_id"`
	BindIP    string     `json:"bind_ip"`
	Port      uint16     `json:"port"` // actual bound port (0 while "requested")
	State     RelayState `json:"state"`
	Error     string     `json:"error,omitempty"`
}

// RelayRegistry holds the live relay set, keyed by implant id then relay id.
type RelayRegistry struct {
	mu sync.Mutex
	m  map[uint64]map[string]*RelayInfo
}

func NewRelayRegistry() *RelayRegistry {
	return &RelayRegistry{m: map[uint64]map[string]*RelayInfo{}}
}

// Request records a freshly-minted relay_id for an implant (relay_listen just
// sent). The actual port arrives later via OnStarted.
func (r *RelayRegistry) Request(implantID uint64, relayID, bindIP string, port uint16) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.ensure(implantID)[relayID] = &RelayInfo{
		ID: relayID, ImplantID: implantID, BindIP: bindIP, Port: port, State: RelayRequested,
	}
}

// OnStarted transitions a relay to running with the actual bound port.
func (r *RelayRegistry) OnStarted(implantID uint64, relayID, bindIP string, port uint16) {
	r.mu.Lock()
	defer r.mu.Unlock()
	info, ok := r.ensure(implantID)[relayID]
	if !ok {
		// Implant reported a start we never requested — record it anyway so the
		// GUI can show + stop it.
		info = &RelayInfo{ID: relayID, ImplantID: implantID}
		r.m[implantID][relayID] = info
	}
	info.BindIP = bindIP
	info.Port = port
	info.State = RelayRunning
	info.Error = ""
}

// OnStopped transitions a relay to stopped and removes it (a stopped relay has
// no live listener, so don't clutter the list).
func (r *RelayRegistry) OnStopped(implantID uint64, relayID string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if set, ok := r.m[implantID]; ok {
		delete(set, relayID)
		if len(set) == 0 {
			delete(r.m, implantID)
		}
	}
}

// OnError transitions a relay to failed (keeps it in the list so the operator
// sees why the start failed).
func (r *RelayRegistry) OnError(implantID uint64, relayID, msg string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	info, ok := r.ensure(implantID)[relayID]
	if !ok {
		info = &RelayInfo{ID: relayID, ImplantID: implantID}
		r.m[implantID][relayID] = info
	}
	info.State = RelayFailed
	info.Error = msg
}

// List returns the relays for one implant (snapshot, ordered for stable UI).
func (r *RelayRegistry) List(implantID uint64) []RelayInfo {
	r.mu.Lock()
	defer r.mu.Unlock()
	set, ok := r.m[implantID]
	if !ok {
		return []RelayInfo{}
	}
	out := make([]RelayInfo, 0, len(set))
	for _, info := range set {
		out = append(out, *info)
	}
	return out
}

// Cleanup drops all relays for an implant (called on implant disconnect).
func (r *RelayRegistry) Cleanup(implantID uint64) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.m, implantID)
}

func (r *RelayRegistry) ensure(implantID uint64) map[string]*RelayInfo {
	set, ok := r.m[implantID]
	if !ok {
		set = map[string]*RelayInfo{}
		r.m[implantID] = set
	}
	return set
}
