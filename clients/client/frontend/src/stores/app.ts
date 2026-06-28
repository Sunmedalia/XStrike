import { defineStore } from 'pinia'
import api from '../services/api'

const normalizeBeacon = (item: any) => {
  const node = item?.node_info ? { ...item.node_info, engine: item.engine } : item
  const nodeId = node?.node_id || node?.id || ''
  const hostname = node?.hostname || node?.computer || '-'
  const ip = node?.ip || node?.internal_ip || '-'
  const processName = node?.process || node?.process_name || '-'
  const os = node?.os || node?.os_version || '-'

  const rawLastSeen = Number(node?.last_seen || 0)
  // Server design uses Unix seconds; tolerate ms input from non-standard agents.
  const lastSeen = rawLastSeen > 10_000_000_000 ? Math.floor(rawLastSeen / 1000) : rawLastSeen

  return {
    ...node,
    id: nodeId,
    node_id: nodeId,
    computer: hostname,
    hostname,
    internal_ip: ip,
    ip,
    process_name: processName,
    process: processName,
    os_version: `${os}${node?.arch ? ` / ${node.arch}` : ''}`,
    selected: Boolean(node?.selected),
    last_seen: lastSeen
  }
}

const normalizeListener = (item: any) => ({
  ...item,
  active: item?.status === 'running'
})

const normalizeLog = (item: any) => ({
  ...item,
  timestamp: item?.created_at ?? item?.timestamp ?? Date.now(),
  log_type: item?.level || item?.log_type || 'system',
  node_id: item?.node_id || '-',
  data: item?.data || ''
})

export const useAppStore = defineStore('app', {
  state: () => ({
    beacons: [] as any[],
    listeners: [] as any[],
    bofs: [] as any[],
    logs: [] as any[],
    loading: false,
    stats: {
      beacons: 0,
      bofs: 0,
      tasks: 0
    }
  }),
  actions: {
    async fetchBeacons() {
      try {
        const selectedIds = new Set(
          this.beacons
            .filter((b: any) => b.selected)
            .map((b: any) => b.node_id || b.id)
            .filter(Boolean)
        )
        const res = await api.get('/nodes', { silentError: true } as any)
        this.beacons = (res.data.data || []).map((item: any) => {
          const b = normalizeBeacon(item)
          const id = b.node_id || b.id
          b.selected = selectedIds.has(id)
          return b
        })
        this.stats.beacons = this.beacons.length
      } catch (err) {}
    },
    async fetchListeners() {
      try {
        const res = await api.get('/listeners', { silentError: true } as any)
        this.listeners = (res.data.data || []).map(normalizeListener)
      } catch (err) {}
    },
    async fetchBofs() {
      try {
        const res = await api.get('/bof', { silentError: true } as any)
        this.bofs = res.data.data || []
        this.stats.bofs = this.bofs.length
      } catch (err) {}
    },
    async fetchLogs() {
      try {
        const res = await api.get('/logs?limit=500', { silentError: true } as any)
        this.logs = (res.data.data || []).map(normalizeLog)
        this.stats.tasks = this.logs.length
      } catch (err) {}
    },
    async refreshAll() {
      this.loading = true
      await Promise.all([
        this.fetchBeacons(),
        this.fetchListeners(),
        this.fetchBofs(),
        this.fetchLogs()
      ])
      this.loading = false
    },

    // ── SSE event handlers for real-time updates ──

    handleBeaconUpdated(_data: any) {
      // The event tells us a beacon changed — refetch for fresh data
      this.fetchBeacons()
    },

    handleBeaconDeleted(data: any) {
      const id = data?.node_id
      if (id) {
        this.beacons = this.beacons.filter(
          (b: any) => (b.node_id || b.id) !== id
        )
        this.stats.beacons = this.beacons.length
      }
    },

    handleListenerChanged(_data: any) {
      this.fetchListeners()
    },

    handleTaskCompleted(_data: any) {
      this.fetchLogs()
    }
  }
})
