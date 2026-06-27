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
