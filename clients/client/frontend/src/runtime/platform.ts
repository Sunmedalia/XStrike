/**
 * Runtime platform detection.
 *
 * Detects whether the app is running inside a desktop shell (Tauri or Wails)
 * or as a regular web page served by the Ghost server.
 *
 * Wails v2 injects `window.go` (generated bindings) and `window.runtime`
 * (the runtime shim). Tauri injects `__TAURI_INTERNALS__` / `__TAURI__`.
 */

export function isWails(): boolean {
  return Boolean(
    (window as any).__wails__ ||
      (window as any).runtime ||
      (window as any).go
  )
}

export function isDesktop(): boolean {
  return Boolean(
    (window as any).__TAURI_INTERNALS__ ||
      (window as any).__TAURI__ ||
      isWails()
  )
}

export function isWeb(): boolean {
  return !isDesktop()
}

export type Platform = 'desktop' | 'web'

export function getPlatform(): Platform {
  return isDesktop() ? 'desktop' : 'web'
}
