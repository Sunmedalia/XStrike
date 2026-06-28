import axios from 'axios'
import { useToastStore } from '../stores/toast'
import { useConnectionStore } from '../stores/connection'
import { getApiBaseUrl } from '../runtime/env'
import { isMockMode, MOCK_TOKEN } from './mockMode'
import { installBackendAdapter } from './mockAdapter'
import type { Router } from 'vue-router'

/**
 * Lazy router reference — set by each entry point (main.ts / main.client.ts)
 * via setRouter(). This avoids a top-level import of router/index.ts which
 * creates a circular dependency that breaks the client build.
 */
let _router: Router | null = null
export function setRouter(r: Router) { _router = r }

/**
 * Shared axios instance.
 *
 * baseURL is set to a sensible default at creation time, but the request
 * interceptor below overrides it on every request so that switching from
 * local to remote mode (or vice-versa) takes effect immediately without
 * recreating the instance.
 */
const api = axios.create({
  baseURL: '/api', // fallback — overridden per-request
})

// ── Request interceptor: dynamic baseURL + auth token ──────────────────────
api.interceptors.request.use(config => {
  // Re-evaluate baseURL on every request so remote ↔ local switches
  // are picked up without needing to recreate the axios instance.
  config.baseURL = getApiBaseUrl()

  const token = localStorage.getItem('token')
  if (token) {
    config.headers.Authorization = `Bearer ${token}`
  }
  return config
})

api.interceptors.response.use(
  response => {
    const body = response?.data
    if (
      body &&
      typeof body === 'object' &&
      'success' in body &&
      body.success === false
    ) {
      const toast = useToastStore()
      const msg = body.error || 'Request failed'
      toast.error(msg)
      return Promise.reject(new Error(msg))
    }
    return response
  },
  async error => {
    const toast = useToastStore()
    const silent = Boolean((error as any)?.config?.silentError)
    
    // 处理 Blob 类型的错误响应
    if (!silent && error.response?.data instanceof Blob && error.response.data.type === 'application/json') {
      const text = await error.response.data.text()
      try {
        const json = JSON.parse(text)
        toast.error(json.error || 'Server error')
        return Promise.reject(json)
      } catch (e) {}
    }

    if (error.response?.status === 401) {
      localStorage.removeItem('token')
      const connStore = useConnectionStore()
      if (_router) {
        if (connStore.isRemote) {
          connStore.disconnect()
          _router.push('/connect')
        } else {
          _router.push('/login')
        }
      }
      if (!silent) toast.error('Session expired')
    } else if (!silent) {
      const msg = error.response?.data?.error || error.message
      toast.error(msg)
    }
    return Promise.reject(error)
  }
)

export default api

// ── Backend adapter ────────────────────────────────────────────────────────
// Install the unified adapter in BOTH modes:
//   - mock  → answers from services/mockData.ts (browser demo / `?demo=1`)
//   - real  → calls the Wails Go bindings that drive the Rust implant
// The adapter branches per-request on isMockMode(), so the operator can flip
// between real and demo at runtime via the `ghost-demo` localStorage flag.
installBackendAdapter(api)

// Ensure a token only for demo mode. Real desktop mode now authenticates
// against the configured core account from the Login page.
if (isMockMode() && !localStorage.getItem('token')) {
  localStorage.setItem('token', MOCK_TOKEN)
}

