/**
 * Connection state store.
 *
 * Manages the current connection mode (local vs remote) and remote server
 * coordinates.  All URL-generation functions in runtime/env.ts read from
 * this store so that API / WebSocket / download calls are automatically
 * routed to the correct server.
 *
 * Persistence: host, port, username, mode, and remote path are saved to
 * localStorage so the user doesn't have to re-enter them on every launch.
 * The JWT token is stored separately (also in localStorage) by the
 * existing auth flow.
 */

import { defineStore } from 'pinia'

export type ConnectionMode = 'local' | 'remote'

export function normalizeRemotePath(path: string): string {
  const trimmed = path.trim()
  if (!trimmed) return ''
  const withLeadingSlash = trimmed.startsWith('/') ? trimmed : `/${trimmed}`
  return withLeadingSlash.replace(/\/+$/, '')
}

export interface ConnectionInfo {
  mode: ConnectionMode
  host: string
  port: number
  path: string
  username: string
  /** Whether the user chose "remember this connection" */
  remember: boolean
}

const STORAGE_KEY = 'ghost-connection'

function loadSaved(): Partial<ConnectionInfo> {
  try {
    const raw = localStorage.getItem(STORAGE_KEY)
    if (!raw) return {}
    return JSON.parse(raw) as Partial<ConnectionInfo>
  } catch {
    return {}
  }
}

function persistConnection(info: ConnectionInfo) {
  if (!info.remember && info.mode === 'remote') {
    // Still save mode + host + port so the form is pre-filled, but
    // strip username when remember is off.
    const { username: _, ...rest } = info
    localStorage.setItem(STORAGE_KEY, JSON.stringify(rest))
    return
  }
  // Never persist passwords — only coordinates + username.
  localStorage.setItem(STORAGE_KEY, JSON.stringify(info))
}

export const useConnectionStore = defineStore('connection', {
  state: (): ConnectionInfo & { connected: boolean } => {
    const saved = loadSaved()
    return {
      mode: (saved.mode as ConnectionMode) || 'local',
      host: saved.host || '',
      port: saved.port || 8080,
      path: normalizeRemotePath(saved.path || ''),
      username: saved.username || '',
      remember: saved.remember ?? true,
      /** True once a remote login has succeeded in this session */
      connected: false,
    }
  },

  getters: {
    isRemote: (s) => s.mode === 'remote',
    isLocal: (s) => s.mode === 'local',

    /**
     * The full origin of the target server.
     * - local  → '' (empty string — use relative URLs)
     * - remote → 'http://host:port'
     */
    serverOrigin(s): string {
      if (s.mode === 'local') return ''
      const scheme = s.port === 443 ? 'https' : 'http'
      return `${scheme}://${s.host}:${s.port}`
    },

    /**
     * WebSocket origin for the target server.
     * - local  → '' (derive from window.location)
     * - remote → 'ws://host:port' or 'wss://host:port'
     */
    wsOrigin(s): string {
      if (s.mode === 'local') return ''
      const scheme = s.port === 443 ? 'wss' : 'ws'
      return `${scheme}://${s.host}:${s.port}`
    },

    remotePath(s): string {
      return normalizeRemotePath(s.path)
    },
  },

  actions: {
    /**
     * Set remote connection info (called from Connect page on success).
     */
    setRemote(host: string, port: number, username: string, remember: boolean, path = '') {
      this.mode = 'remote'
      this.host = host
      this.port = port
      this.path = normalizeRemotePath(path)
      this.username = username
      this.remember = remember
      this.connected = true
      persistConnection(this.$state)
    },

    /**
     * Switch to local mode (web UI against the current origin).
     */
    setLocal() {
      this.mode = 'local'
      this.host = ''
      this.port = 8080
      this.path = ''
      this.connected = false
      persistConnection(this.$state)
    },

    /**
     * Disconnect from remote server: clear token, mark disconnected.
     * Does NOT reset host/port so the form is pre-filled next time.
     */
    disconnect() {
      this.connected = false
      localStorage.removeItem('token')
    },

    /**
     * Full reset: clear everything and go back to initial state.
     */
    reset() {
      this.mode = 'local'
      this.host = ''
      this.port = 8080
      this.path = ''
      this.username = ''
      this.connected = false
      localStorage.removeItem('token')
      localStorage.removeItem(STORAGE_KEY)
    },

    /**
     * Mark as connected (called when restoring a session with a valid token).
     */
    markConnected() {
      this.connected = true
    },
  },
})
