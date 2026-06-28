package main

import (
	"sync"
)

// Event is anything worth pushing to a GUI subscriber over WebSocket.
type Event struct {
	Type      string `json:"type"`        // implant_connected|implant_disconnected|hello|output|error
	ImplantID uint64 `json:"implant_id"`  // 0 for server-wide events
	Data      string `json:"data"`        // payload text (addr, output text, …)
}

// EventBus fans out Events to all current subscribers (WebSocket clients).
type EventBus struct {
	mu          sync.RWMutex
	subscribers map[chan Event]struct{}
}

func NewEventBus() *EventBus {
	return &EventBus{subscribers: make(map[chan Event]struct{})}
}

// Subscribe returns a buffered channel receiving every published Event.
// Unsubscribe by closing the returned channel (the bus drops it on send-fail).
func (b *EventBus) Subscribe() chan Event {
	ch := make(chan Event, 64)
	b.mu.Lock()
	b.subscribers[ch] = struct{}{}
	b.mu.Unlock()
	return ch
}

func (b *EventBus) Unsubscribe(ch chan Event) {
	b.mu.Lock()
	delete(b.subscribers, ch)
	b.mu.Unlock()
	close(ch)
}

func (b *EventBus) Publish(e Event) {
	b.mu.RLock()
	defer b.mu.RUnlock()
	for ch := range b.subscribers {
		select {
		case ch <- e:
		default:
			// subscriber is slow / full; drop to avoid blocking the implant reader.
			// (A real platform would persist missed output; for now, live push wins.)
		}
	}
}
