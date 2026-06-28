/**
 * Runtime network utilities.
 *
 * Wraps fetch calls and download behaviors to work consistently
 * in both web and desktop environments.
 */

import { getApiUrl, getAuthHeaders } from './env'

/**
 * Performs an authenticated fetch to an API path.
 * Use this instead of raw `fetch('/api/...')` in components.
 */
export async function apiFetch(
  path: string,
  init?: RequestInit
): Promise<Response> {
  const url = getApiUrl(path)
  const headers = {
    ...getAuthHeaders(),
    ...(init?.headers as Record<string, string> | undefined),
  }
  return fetch(url, { ...init, headers })
}

/**
 * Triggers a file download from a Blob.
 * Works in both web and desktop (webview) environments.
 */
export function downloadBlob(blob: Blob, filename: string): void {
  const url = window.URL.createObjectURL(blob)
  const link = document.createElement('a')
  link.href = url
  link.setAttribute('download', filename)
  document.body.appendChild(link)
  link.click()
  document.body.removeChild(link)
  window.URL.revokeObjectURL(url)
}
