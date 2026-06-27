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
  since: string
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
