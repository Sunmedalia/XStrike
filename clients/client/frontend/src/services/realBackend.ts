/**
 * Real-backend shared state.
 *
 * In real (desktop) mode, BOF output and session events arrive asynchronously
 * via the Wails `core:event` stream — there is no `/logs` REST endpoint to poll
 * like the old server contract had. So we keep a small client-side event log here that
 * the `core:event` subscriber appends to and the axios adapter's `/logs` handler
 * returns. This lets the existing store/UI (which calls `GET /logs`) work
 * unchanged against the real backend.
 */

export interface RealLog {
  created_at: number
  level: 'info' | 'task' | 'warn' | 'error' | 'system'
  node_id: string
  data: string
}

const realLogs: RealLog[] = []
const MAX_LOGS = 500

export function getRealLogs(): RealLog[] {
  return realLogs
}

export function pushRealLog(entry: RealLog) {
  realLogs.unshift(entry)
  if (realLogs.length > MAX_LOGS) realLogs.length = MAX_LOGS
}

/** Map a core:event type to a log level + human message. */
export function eventToLog(ev: { type: string; implant_id: number; data: string }): RealLog | null {
  const node = ev.implant_id ? String(ev.implant_id) : '-'
  const ts = Math.floor(Date.now() / 1000)
  switch (ev.type) {
    case 'implant_connected':
      return { created_at: ts, level: 'info', node_id: node, data: `Implant #${ev.implant_id} connected (${ev.data})` }
    case 'implant_disconnected':
      return { created_at: ts, level: 'warn', node_id: node, data: `Implant #${ev.implant_id} disconnected` }
    case 'hello':
      return { created_at: ts, level: 'info', node_id: node, data: `hello: ${ev.data}` }
    case 'output':
      return { created_at: ts, level: 'task', node_id: node, data: ev.data || '(empty output)' }
    case 'error':
      return { created_at: ts, level: 'error', node_id: node, data: ev.data }
    case 'sysinfo': {
      // Recon BOF result — ev.data is a JSON field map. Surface a one-liner.
      let summary = ev.data
      try {
        const f = JSON.parse(ev.data)
        summary = `recon: ${f.user || '?'}@${f.computer || '?'} (${f.os || '?'}/${f.arch || '?'}) ext=${f.external_ip || '?'}`
      } catch { /* keep raw */ }
      return { created_at: ts, level: 'info', node_id: node, data: summary }
    }
    default:
      return null
  }
}
