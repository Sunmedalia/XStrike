/**
 * Mock / demo mode.
 *
 * Mode resolution:
 *   - desktop (Wails)        → REAL backend (drives the Rust implant via the
 *                               Go core). Mock is opt-in via the demo flag.
 *   - browser                 → mock (the Ghost server the axios layer expects
 *                               isn't running here; mock keeps the UI usable).
 *   - `?demo=1` / ghost-demo  → mock (forces demo even in the desktop shell, so
 *                               the operator can preview the sample dataset).
 *
 * The real Wails Go backend is wired in via services/wailsBindings + the axios
 * adapter in services/api.ts, so the desktop runs against live data by default.
 */

import { isDesktop } from '../runtime/platform'

const DEMO_FLAG = 'ghost-demo'

function demoForced(): boolean {
  try {
    if (localStorage.getItem(DEMO_FLAG) === '1') return true
    if (typeof window !== 'undefined' && window.location) {
      return new URLSearchParams(window.location.search).has('demo')
    }
  } catch {
    /* ignore */
  }
  return false
}

export function isMockMode(): boolean {
  if (demoForced()) return true
  // Desktop = real backend. Browser = mock demo.
  return !isDesktop()
}

/** Real backend reachable (desktop, not forced into demo). */
export function isRealMode(): boolean {
  return isDesktop() && !demoForced()
}

/** Force mock mode on/off from the UI (the Login "Enter demo console" button). */
export function setMockMode(on: boolean) {
  try {
    if (on) localStorage.setItem(DEMO_FLAG, '1')
    else localStorage.removeItem(DEMO_FLAG)
  } catch {
    /* ignore */
  }
}

/** A throwaway token so the router auth guard lets mock/real sessions through. */
export const MOCK_TOKEN = 'ruststrike-desktop-token'
