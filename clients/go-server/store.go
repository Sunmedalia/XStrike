package main

// SQLite-backed persistence for the Go service core.
//
// First external dependency (modernc.org/sqlite, pure-Go, no cgo). Holds:
//   - logs:          every bus event (durable log the GUI hydrates from)
//   - listeners:     listener config (so the set survives a core restart)
//   - agents:        per-implant roster (first/last seen, addr, note)
//   - artifacts:     per-agent captured BOF output (file_list/proc_list text,
//                     screenshot/file_download blobs)
//
// Concurrency: the bus subscriber feeds a buffered channel; a single writer
// goroutine drains it into SQLite so the implant reader is never blocked on
// disk. Reads (REST handlers) go through *sql.DB directly (sqlite3 handles
// concurrent reads under WAL — we enable it).

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"

	_ "modernc.org/sqlite"
)

// Store wraps a SQLite database for durable state.
type Store struct {
	db *sql.DB
	// log ingestion: non-blocking best-effort. Buffered; if the writer falls
	// behind, drops (like the bus) rather than stalling the implant reader.
	logCh   chan logRow
	closeCh chan struct{}
}

type logRow struct {
	TS        int64
	ImplantID uint64
	Type      string
	Data      string
}

// ArtifactKind enumerates the BOF outputs we persist per-agent.
type ArtifactKind string

const (
	KindFileList   ArtifactKind = "file_list"
	KindProcList   ArtifactKind = "proc_list"
	KindScreenshot ArtifactKind = "screenshot"
	KindDownload   ArtifactKind = "download"
)

// Artifact is a captured BOF output row. Meta holds parseable text (file/proc
// listings); Blob holds binary (screenshot BMP, downloaded file bytes). At
// most one is populated per row.
type Artifact struct {
	ID        int64        `json:"id"`
	TS        int64        `json:"ts"`
	ImplantID uint64       `json:"implant_id"`
	Kind      ArtifactKind `json:"kind"`
	Path      string       `json:"path,omitempty"`
	Meta      string       `json:"meta,omitempty"`
	HasBlob   bool         `json:"has_blob"`
}

// ListenerRow is the persisted listener config.
type ListenerRow struct {
	ID        string `json:"id"`
	Name      string `json:"name"`
	Protocol  string `json:"protocol"`
	BindIP    string `json:"bind_ip"`
	Port      string `json:"port"`
	Active    bool   `json:"active"`
	CreatedTS int64  `json:"created_ts"`
}

// AgentRow is the persisted agent roster entry.
type AgentRow struct {
	ImplantID uint64 `json:"implant_id"`
	FirstSeen int64  `json:"first_seen"`
	LastSeen  int64  `json:"last_seen"`
	Addr      string `json:"addr"`
	Note      string `json:"note,omitempty"`
}

// NewStore opens (or creates) the SQLite DB at path and runs migrations.
func NewStore(path string) (*Store, error) {
	if path == "" {
		// default: next to the exe
		if exe, err := os.Executable(); err == nil {
			path = filepath.Join(filepath.Dir(exe), "ruststrike.db")
		} else {
			path = "ruststrike.db"
		}
	}
	dsn := "file:" + path + "?_pragma=journal_mode(WAL)&_pragma=busy_timeout(5000)&_pragma=foreign_keys(1)"
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, fmt.Errorf("open sqlite %s: %w", path, err)
	}
	// sqlite3 driver from modernc uses a single connection for writes under
	// WAL; allow a few readers.
	db.SetMaxOpenConns(8)
	if err := migrate(db); err != nil {
		db.Close()
		return nil, err
	}
	s := &Store{
		db:      db,
		logCh:   make(chan logRow, 1024),
		closeCh: make(chan struct{}),
	}
	go s.logWriter()
	return s, nil
}

func migrate(db *sql.DB) error {
	stmts := []string{
		`CREATE TABLE IF NOT EXISTS logs (
			id         INTEGER PRIMARY KEY AUTOINCREMENT,
			ts         INTEGER NOT NULL,
			implant_id INTEGER NOT NULL,
			type       TEXT NOT NULL,
			data       TEXT NOT NULL DEFAULT ''
		)`,
		`CREATE INDEX IF NOT EXISTS idx_logs_implant_ts ON logs(implant_id, ts)`,
		`CREATE TABLE IF NOT EXISTS listeners (
			id         TEXT PRIMARY KEY,
			name       TEXT NOT NULL DEFAULT '',
			protocol   TEXT NOT NULL DEFAULT 'tcp',
			bind_ip    TEXT NOT NULL DEFAULT '0.0.0.0',
			port       TEXT NOT NULL DEFAULT '',
			active     INTEGER NOT NULL DEFAULT 1,
			created_ts INTEGER NOT NULL DEFAULT 0
		)`,
		`CREATE TABLE IF NOT EXISTS agents (
			implant_id INTEGER PRIMARY KEY,
			first_seen INTEGER NOT NULL DEFAULT 0,
			last_seen  INTEGER NOT NULL DEFAULT 0,
			addr       TEXT NOT NULL DEFAULT '',
			note       TEXT NOT NULL DEFAULT ''
		)`,
		`CREATE TABLE IF NOT EXISTS artifacts (
			id         INTEGER PRIMARY KEY AUTOINCREMENT,
			ts         INTEGER NOT NULL,
			implant_id INTEGER NOT NULL,
			kind       TEXT NOT NULL,
			path       TEXT NOT NULL DEFAULT '',
			meta       TEXT NOT NULL DEFAULT '',
			blob       BLOB
		)`,
		`CREATE INDEX IF NOT EXISTS idx_artifacts_implant_kind_ts ON artifacts(implant_id, kind, ts)`,
	}
	for _, s := range stmts {
		if _, err := db.Exec(s); err != nil {
			return fmt.Errorf("migrate: %w (stmt: %s)", err, s)
		}
	}
	return nil
}

func (s *Store) Close() {
	close(s.closeCh)
	s.db.Close()
}

// logWriter drains the log channel into SQLite. Single writer goroutine —
// keeps the implant reader (which Publishes events) off the disk path.
func (s *Store) logWriter() {
	batch := make([]logRow, 0, 64)
	flush := func() {
		if len(batch) == 0 {
			return
		}
		tx, err := s.db.Begin()
		if err != nil {
			batch = batch[:0]
			return
		}
		stmt, _ := tx.Prepare(`INSERT INTO logs(ts, implant_id, type, data) VALUES(?,?,?,?)`)
		if stmt != nil {
			for _, r := range batch {
				_, _ = stmt.Exec(r.TS, r.ImplantID, r.Type, r.Data)
			}
			stmt.Close()
		}
		_ = tx.Commit()
		batch = batch[:0]
	}
	tick := time.NewTicker(500 * time.Millisecond)
	defer tick.Stop()
	for {
		select {
		case r, ok := <-s.logCh:
			if !ok {
				flush()
				return
			}
			batch = append(batch, r)
			if len(batch) >= 64 {
				flush()
			}
		case <-tick.C:
			flush()
		case <-s.closeCh:
			// drain remaining then exit
			for {
				select {
				case r := <-s.logCh:
					batch = append(batch, r)
				default:
					flush()
					return
				}
			}
		}
	}
}

// LogEnqueue is called by the bus subscriber. Non-blocking; drops on overflow.
func (s *Store) LogEnqueue(ts int64, implantID uint64, typ, data string) {
	select {
	case s.logCh <- logRow{TS: ts, ImplantID: implantID, Type: typ, Data: data}:
	default:
	}
}

// LogEntry is a row returned to the GUI.
type LogEntry struct {
	ID        int64  `json:"id"`
	TS        int64  `json:"ts"`
	ImplantID uint64 `json:"implant_id"`
	Type      string `json:"type"`
	Data      string `json:"data"`
}

// Logs returns the most recent `limit` log rows (optionally filtered by
// implant), newest first.
func (s *Store) Logs(implantID uint64, limit int) ([]LogEntry, error) {
	if limit <= 0 || limit > 5000 {
		limit = 500
	}
	q := `SELECT id, ts, implant_id, type, data FROM logs`
	var args []any
	if implantID > 0 {
		q += ` WHERE implant_id = ?`
		args = append(args, implantID)
	}
	q += ` ORDER BY id DESC LIMIT ?`
	args = append(args, limit)
	rows, err := s.db.Query(q, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := make([]LogEntry, 0, limit)
	for rows.Next() {
		var e LogEntry
		if err := rows.Scan(&e.ID, &e.TS, &e.ImplantID, &e.Type, &e.Data); err != nil {
			return nil, err
		}
		out = append(out, e)
	}
	return out, rows.Err()
}

// ---- listeners ----

func (s *Store) PutListener(l ListenerRow) error {
	_, err := s.db.Exec(`INSERT INTO listeners(id,name,protocol,bind_ip,port,active,created_ts)
		VALUES(?,?,?,?,?,?,?)
		ON CONFLICT(id) DO UPDATE SET name=excluded.name, protocol=excluded.protocol,
			bind_ip=excluded.bind_ip, port=excluded.port, active=excluded.active`,
		l.ID, l.Name, l.Protocol, l.BindIP, l.Port, boolToInt(l.Active), l.CreatedTS)
	return err
}

func (s *Store) SetListenerActive(id string, active bool) error {
	_, err := s.db.Exec(`UPDATE listeners SET active=? WHERE id=?`, boolToInt(active), id)
	return err
}

func (s *Store) DeleteListener(id string) error {
	_, err := s.db.Exec(`DELETE FROM listeners WHERE id=?`, id)
	return err
}

func (s *Store) ListListeners() ([]ListenerRow, error) {
	rows, err := s.db.Query(`SELECT id,name,protocol,bind_ip,port,active,created_ts FROM listeners ORDER BY created_ts`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []ListenerRow{}
	for rows.Next() {
		var l ListenerRow
		var act int
		if err := rows.Scan(&l.ID, &l.Name, &l.Protocol, &l.BindIP, &l.Port, &act, &l.CreatedTS); err != nil {
			return nil, err
		}
		l.Active = act != 0
		out = append(out, l)
	}
	return out, rows.Err()
}

// ---- agents ----

// TouchAgent records a sighting of an implant (upserts last_seen).
func (s *Store) TouchAgent(implantID uint64, addr string, ts int64) {
	now := ts
	if now == 0 {
		now = time.Now().Unix()
	}
	_, err := s.db.Exec(`INSERT INTO agents(implant_id, first_seen, last_seen, addr)
		VALUES(?,?,?,?)
		ON CONFLICT(implant_id) DO UPDATE SET last_seen=excluded.last_seen, addr=excluded.addr`,
		implantID, now, now, addr)
	if err != nil {
		// best-effort; don't let a DB hiccup break the session.
		_ = err
	}
}

func (s *Store) ListAgents() ([]AgentRow, error) {
	rows, err := s.db.Query(`SELECT implant_id, first_seen, last_seen, addr, note FROM agents ORDER BY last_seen DESC`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []AgentRow{}
	for rows.Next() {
		var a AgentRow
		if err := rows.Scan(&a.ImplantID, &a.FirstSeen, &a.LastSeen, &a.Addr, &a.Note); err != nil {
			return nil, err
		}
		out = append(out, a)
	}
	return out, rows.Err()
}

// ---- artifacts ----

// PutArtifact persists a captured BOF output. meta is text (file/proc listings);
// blob is binary (screenshot/download). path is the subject path for
// file_list/file_download.
func (s *Store) PutArtifact(implantID uint64, kind ArtifactKind, path, meta string, blob []byte) (int64, error) {
	res, err := s.db.Exec(`INSERT INTO artifacts(ts, implant_id, kind, path, meta, blob) VALUES(?,?,?,?,?,?)`,
		time.Now().Unix(), implantID, string(kind), path, meta, blob)
	if err != nil {
		return 0, err
	}
	return res.LastInsertId()
}

// ListArtifacts returns artifact metadata (no blob) for an agent, optionally
// filtered by kind, newest first.
func (s *Store) ListArtifacts(implantID uint64, kind ArtifactKind, limit int) ([]Artifact, error) {
	if limit <= 0 || limit > 500 {
		limit = 50
	}
	q := `SELECT id, ts, implant_id, kind, path, meta, (blob IS NOT NULL) FROM artifacts WHERE implant_id=?`
	args := []any{implantID}
	if kind != "" {
		q += ` AND kind=?`
		args = append(args, string(kind))
	}
	q += ` ORDER BY id DESC LIMIT ?`
	args = append(args, limit)
	rows, err := s.db.Query(q, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []Artifact{}
	for rows.Next() {
		var a Artifact
		var k string
		var hasBlob int
		if err := rows.Scan(&a.ID, &a.TS, &a.ImplantID, &k, &a.Path, &a.Meta, &hasBlob); err != nil {
			return nil, err
		}
		a.Kind = ArtifactKind(k)
		a.HasBlob = hasBlob != 0
		out = append(out, a)
	}
	return out, rows.Err()
}

// GetArtifactBlob returns the blob bytes for one artifact (screenshots/downloads).
func (s *Store) GetArtifactBlob(id int64) ([]byte, string, error) {
	var kind, meta string
	var blob []byte
	err := s.db.QueryRow(`SELECT kind, meta, blob FROM artifacts WHERE id=?`, id).Scan(&kind, &meta, &blob)
	if err != nil {
		return nil, "", err
	}
	return blob, kind, nil
}

// GetArtifactMeta returns the text meta for one artifact (file/proc listings).
func (s *Store) GetArtifactMeta(id int64) (string, string, error) {
	var kind, meta string
	err := s.db.QueryRow(`SELECT kind, meta FROM artifacts WHERE id=?`, id).Scan(&kind, &meta)
	if err != nil {
		return "", "", err
	}
	return meta, kind, nil
}

func boolToInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

// keep json imported (used for artifact meta in callers if needed)
var _ = json.Marshal
var _ sync.Mutex
