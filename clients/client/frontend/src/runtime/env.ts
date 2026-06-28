/**
 * Runtime environment configuration.
 *
 * Provides unified access to base URLs, default hosts, and other
 * environment-dependent settings for web, desktop, and remote-client modes.
 *
 * URL resolution strategy:
 *   local  mode → relative paths ("/api"), window.location for WS
 *   remote mode → absolute URLs built from connection store
 *
 * The connection store is accessed lazily (not at import time) so that
 * Pinia is guaranteed to be initialised before first use.
 */

import { useConnectionStore } from '../stores/connection'

// ---------------------------------------------------------------------------
// Base path (random URL prefix injected by the server)
// ---------------------------------------------------------------------------

/** Runtime base path injected by the server (e.g., "/a3f8b2c1"). */
const rawBasePath = String((window as any).__BASE_PATH__ || '').trim()

/** Prefix without trailing slash, safe for API and asset URLs. */
export const BASE_PATH: string = rawBasePath.replace(/\/+$/, '')

/** Prefix with trailing slash, required by vue-router history base. */
export const ROUTER_BASE: string = BASE_PATH ? `${BASE_PATH}/` : '/'

/** Resolve a static asset path with the base prefix. */
export function asset(path: string): string {
  return `${BASE_PATH}${path}`
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Lazy accessor — safe to call after createPinia(). */
function conn() {
  return useConnectionStore()
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Returns the origin of the backend server.
 * - local:  window.location.origin  (relative URLs also work)
 * - remote: "http://host:port"
 */
export function getServerOrigin(): string {
  const c = conn()
  return c.isRemote ? c.serverOrigin : window.location.origin
}

/**
 * Base URL for REST API calls.
 * - local:  "/api"         (relative — browser resolves against page origin)
 * - remote: "http://h:p/api" (absolute)
 */
export function getApiBaseUrl(): string {
  const c = conn()
  return c.isRemote ? `${c.serverOrigin}${c.remotePath}/api` : `${BASE_PATH}/api`
}

/**
 * Constructs a full API endpoint URL (for raw fetch, not axios).
 */
export function getApiUrl(path: string): string {
  const base = getApiBaseUrl()
  const cleanPath = path.startsWith('/') ? path : `/${path}`
  return `${base}${cleanPath}`
}

/**
 * Constructs a WebSocket URL from a relative path.
 * e.g. getWsUrl('/api/vnc/abc/ws?token=xxx')
 *   local  → "ws://current-host/api/vnc/abc/ws?token=xxx"
 *   remote → "ws://remote-host:port/api/vnc/abc/ws?token=xxx"
 */
export function getWsUrl(path: string): string {
  const c = conn()
  const cleanPath = path.startsWith('/') ? path : `/${path}`

  if (c.isRemote) {
    return `${c.wsOrigin}${c.remotePath}${cleanPath}`
  }
  const proto = window.location.protocol === 'https:' ? 'wss' : 'ws'
  return `${proto}://${window.location.host}${BASE_PATH}${cleanPath}`
}

/**
 * Returns a sensible default host for agent callback (LHOST).
 * - remote mode: returns the configured server host
 * - local  mode: returns window.location.hostname
 */
export function getDefaultCallbackHost(): string {
  const c = conn()
  if (c.isRemote) return c.host
  return window.location.hostname
}

/**
 * Returns an authorization header object using the stored JWT.
 */
export function getAuthHeaders(): Record<string, string> {
  const token = localStorage.getItem('token') ?? ''
  return token ? { Authorization: `Bearer ${token}` } : {}
}
