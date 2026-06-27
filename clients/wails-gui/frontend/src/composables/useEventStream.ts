import { ref } from 'vue'
import { getApiBaseUrl } from '../runtime/env'
import api from '../services/api'
import { isMockMode, isRealMode } from '../services/mockMode'
import { pushMockEvent, mockBeacons } from '../services/mockData'
import { wailsEventsOn, type WailsCoreEvent } from '../services/wailsBindings'
import { pushRealLog, eventToLog } from '../services/realBackend'

export interface SseCallbacks {
  onBeaconUpdated?: (data: any) => void
  onBeaconDeleted?: (data: any) => void
  onTaskCompleted?: (data: any) => void
  onListenerChanged?: (data: any) => void
}

const MAX_RECONNECT_DELAY = 30_000

export function useEventStream() {
  const connected = ref(false)
  let eventSource: EventSource | null = null
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null
  let reconnectDelay = 1000 // Exponential backoff from 1s

  let callbacks: SseCallbacks = {}
  let mockTimer: ReturnType<typeof setInterval> | null = null
  let unsubWails: (() => void) | null = null

  async function connect() {
    const token = localStorage.getItem('token')
    if (!token) {
      // Genuinely logged out — don't reconnect.
      connected.value = false
      return
    }

    // Real desktop mode: subscribe to the Wails `core:event` stream forwarded
    // from the Go core's WebSocket. No EventSource/ticket needed.
    if (isRealMode()) {
      connected.value = true
      if (unsubWails) return
      unsubWails = wailsEventsOn('core:event', (ev: WailsCoreEvent) => {
        const log = eventToLog(ev)
        if (log) pushRealLog(log)
        switch (ev.type) {
          case 'implant_connected':
          case 'hello':
          case 'output':
            // A session changed (or produced output) — refetch the list + logs.
            callbacks.onBeaconUpdated?.({ node_id: String(ev.implant_id) })
            callbacks.onTaskCompleted?.({ node_id: String(ev.implant_id), data: ev.data })
            break
          case 'implant_disconnected':
            callbacks.onBeaconDeleted?.({ node_id: String(ev.implant_id) })
            break
          case 'error':
            callbacks.onTaskCompleted?.({ node_id: String(ev.implant_id), data: ev.data, failed: true })
            break
          default:
            break
        }
      })
      return
    }

    // Mock mode: no real EventSource. Periodically emit a synthetic check-in
    // event so the Event Stream feels alive and the store refetches.
    if (isMockMode()) {
      connected.value = true
      if (mockTimer) return
      mockTimer = setInterval(() => {
        const b = mockBeacons[Math.floor(Math.random() * mockBeacons.length)]
        if (b) {
          pushMockEvent('info', b.node_info.node_id, `Session ${b.node_info.node_id} checked in`)
          callbacks.onBeaconUpdated?.({ node_id: b.node_info.node_id })
        }
      }, 15000)
      return
    }

    // EventSource cannot set Authorization headers, so trade the JWT for a
    // short-lived, single-use ticket and pass it as a query param. This keeps
    // the long-lived JWT out of URLs / proxy logs. A ticket-fetch failure is
    // usually transient (server restart, brief network blip, JWT freshly
    // expired) — schedule a reconnect so the stream recovers instead of dying
    // silently until page reload.
    let ticket: string
    try {
      const res = await api.post('/auth/ticket')
      if (!res.data?.success || !res.data?.data) {
        connected.value = false
        scheduleReconnect()
        return
      }
      ticket = res.data.data
    } catch {
      connected.value = false
      scheduleReconnect()
      return
    }

    const baseUrl = getApiBaseUrl()
    const url = `${baseUrl}/events?ticket=${encodeURIComponent(ticket)}`
    eventSource = new EventSource(url)

    eventSource.onopen = () => {
      connected.value = true
      reconnectDelay = 1000 // Reset on successful connection
    }

    eventSource.addEventListener('beacon_updated', (e: MessageEvent) => {
      try {
        callbacks.onBeaconUpdated?.(JSON.parse(e.data))
      } catch { /* ignore parse errors */ }
    })

    eventSource.addEventListener('beacon_deleted', (e: MessageEvent) => {
      try {
        callbacks.onBeaconDeleted?.(JSON.parse(e.data))
      } catch { /* ignore parse errors */ }
    })

    eventSource.addEventListener('task_completed', (e: MessageEvent) => {
      try {
        callbacks.onTaskCompleted?.(JSON.parse(e.data))
      } catch { /* ignore parse errors */ }
    })

    eventSource.addEventListener('listener_changed', (e: MessageEvent) => {
      try {
        callbacks.onListenerChanged?.(JSON.parse(e.data))
      } catch { /* ignore parse errors */ }
    })

    eventSource.onerror = () => {
      connected.value = false
      eventSource?.close()
      eventSource = null
      scheduleReconnect()
    }
  }

  function scheduleReconnect() {
    if (reconnectTimer) clearTimeout(reconnectTimer)
    reconnectTimer = setTimeout(() => {
      reconnectDelay = Math.min(reconnectDelay * 2, MAX_RECONNECT_DELAY)
      connect()
    }, reconnectDelay)
  }

  function disconnect() {
    if (reconnectTimer) {
      clearTimeout(reconnectTimer)
      reconnectTimer = null
    }
    if (mockTimer) {
      clearInterval(mockTimer)
      mockTimer = null
    }
    if (unsubWails) {
      unsubWails()
      unsubWails = null
    }
    eventSource?.close()
    eventSource = null
    connected.value = false
  }

  return {
    connected,
    connect,
    disconnect,
    setCallbacks(cbs: SseCallbacks) {
      callbacks = cbs
    }
  }
}
