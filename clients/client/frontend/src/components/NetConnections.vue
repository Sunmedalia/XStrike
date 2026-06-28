<template>
  <div class="net-manager">
    <div class="net-toolbar">
      <div class="search-box">
        <Search :size="12" />
        <input v-model="filter" placeholder="Search connections (proto, local, remote, pid, state)..." />
      </div>
      <div class="proto-filter">
        <button class="chip" :class="{ active: protoFilter === 'all' }" @click="protoFilter = 'all'">All</button>
        <button class="chip" :class="{ active: protoFilter === 'TCP' }" @click="protoFilter = 'TCP'">TCP</button>
        <button class="chip" :class="{ active: protoFilter === 'UDP' }" @click="protoFilter = 'UDP'">UDP</button>
      </div>
      <div class="spacer"></div>
      <button class="btn" @click="refreshNet(true)" :disabled="loading">
        <RefreshCw :size="12" :class="{ spinning: loading }" /> Refresh
      </button>
    </div>

    <div class="net-list-container">
      <table class="net-table">
        <thead>
          <tr>
            <th style="width: 60px">Proto</th>
            <th>Local</th>
            <th>Remote</th>
            <th style="width: 90px">PID</th>
            <th style="width: 120px">State</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="(c, idx) in filteredConns" :key="idx">
            <td><span class="badge" :class="c.proto.toLowerCase()">{{ c.proto }}</span></td>
            <td class="mono-cell">{{ c.local }}</td>
            <td class="mono-cell">{{ c.remote }}</td>
            <td>{{ c.pid }}</td>
            <td><span class="state" :class="stateClass(c.state)">{{ c.state || '-' }}</span></td>
          </tr>
        </tbody>
      </table>
      <div v-if="!filteredConns.length && !loading" class="empty">No connections. Hit Refresh.</div>
    </div>

    <div class="net-footer">
      Total: {{ conns.length }} | Filtered: {{ filteredConns.length }}
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { Search, RefreshCw } from 'lucide-vue-next'
import api from '../services/api'
import { useToastStore } from '../stores/toast'
import { useAppStore } from '../stores/app'
import { auditTaskInput } from '../services/taskAudit'

const netCache: Map<string, any[]> = (() => {
  const g = globalThis as any
  if (!g.__ghostNetConnCache) {
    g.__ghostNetConnCache = new Map<string, any[]>()
  }
  return g.__ghostNetConnCache as Map<string, any[]>
})()

const props = defineProps<{ targetId: string }>()
const toast = useToastStore()
const appStore = useAppStore()

const filter = ref('')
const protoFilter = ref<'all' | 'TCP' | 'UDP'>('all')
const conns = ref<any[]>([])
const loading = ref(false)

const findBof = (pattern: RegExp) => appStore.bofs.find(b => pattern.test(b.name))

const updateCache = () => {
  netCache.set(props.targetId, [...conns.value])
}

const pollTaskResult = async (taskId: string, maxRetry = 60): Promise<any> => {
  for (let i = 0; i < maxRetry; i++) {
    try {
      const res = await api.get(`/tasks/${taskId}`, {
        silentError: true,
        validateStatus: (s: number) => s === 200 || s === 404
      } as any)
      if (res.status === 404) {
        await new Promise(resolve => setTimeout(resolve, 1000))
        continue
      }
      if (res.data.success && res.data.data) {
        return res.data.data
      }
    } catch (err: any) {
      throw err
    }
    await new Promise(resolve => setTimeout(resolve, 1000))
  }
  throw new Error('Task timeout')
}

const parseRows = (raw: string) =>
  (raw || '')
    .trim()
    .split('\n')
    .map((line: string) => line.trim())
    .filter((line: string) => line.length > 0)
    .map((line: string) => {
      const parts = line.split('\t')
      if (parts.length < 4) return null
      return {
        proto: parts[0] || '?',
        local: parts[1] || '',
        remote: parts[2] || '',
        pid: parts[3] || '-',
        state: parts[4] || ''
      }
    })
    .filter(Boolean)

const filteredConns = computed(() => {
  const s = filter.value.toLowerCase()
  return conns.value.filter(c => {
    if (protoFilter.value !== 'all' && c.proto !== protoFilter.value) return false
    if (!s) return true
    return (
      c.proto.toLowerCase().includes(s) ||
      c.local.toLowerCase().includes(s) ||
      c.remote.toLowerCase().includes(s) ||
      String(c.pid).includes(s) ||
      c.state.toLowerCase().includes(s)
    )
  })
})

const stateClass = (s: string) => {
  if (!s) return ''
  if (s === 'ESTABLISHED') return 'st-established'
  if (s === 'LISTEN') return 'st-listen'
  if (s === 'TIME_WAIT' || s === 'CLOSE_WAIT' || s === 'CLOSING') return 'st-closing'
  if (s === 'SYN_SENT' || s === 'SYN_RCVD') return 'st-syn'
  return ''
}

const refreshNet = async (force = false) => {
  if (!force) {
    const cached = netCache.get(props.targetId)
    if (cached) {
      conns.value = [...cached]
      return
    }
  }

  loading.value = true
  try {
    const bof = findBof(/^netstat\b/i)
    if (!bof) {
      toast.error('No netstat BOF found. Upload netstat.o first.')
      return
    }

    await auditTaskInput({
      source: 'net:list',
      nodeId: props.targetId,
      input: '(netstat)'
    })
    const res = await api.post('/bof/execute', {
      node_id: props.targetId,
      bof_name: bof.name,
      plugin_name: bof.plugin_name || ''
    })
    const result = await pollTaskResult(res.data.data, 60)
    if (!result.success) {
      toast.error(result.error || 'Network enumeration failed')
      return
    }

    conns.value = parseRows(result.output || '')
    updateCache()
  } catch (err: any) {
    toast.error(err.message || 'Failed to enumerate network connections')
  } finally {
    loading.value = false
  }
}

// Hydrate the last network snapshot from the persisted store (survives restart).
const hydrateFromDb = async () => {
  const id = Number(props.targetId)
  if (!id) return
  try {
    const res = await api.get(`/agents/${id}/artifacts`, { params: { kind: 'net_list', limit: 1 }, silentError: true } as any)
    const arts: any[] = res.data?.data || []
    if (!arts.length) return
    const meta = arts[0].meta
    if (!meta) return
    conns.value = parseRows(meta)
    if (conns.value.length) updateCache()
  } catch { /* silent */ }
}

const onSync = async () => {
  await refreshNet(true)
}

onMounted(async () => {
  window.addEventListener('ghost:sync', onSync as EventListener)
  await refreshNet(false)
  if (!conns.value.length) {
    await hydrateFromDb()
  }
})
onUnmounted(() => {
  window.removeEventListener('ghost:sync', onSync as EventListener)
})
</script>

<style scoped>
.net-manager { display: flex; flex-direction: column; height: 100%; background: var(--bg); }
.net-toolbar { height: 36px; display: flex; align-items: center; gap: 8px; padding: 0 12px; background: var(--bg-2); border-bottom: 1px solid var(--bd); }
.search-box { display: flex; align-items: center; gap: 8px; background: var(--bg-3); border: 1px solid var(--bd); border-radius: 4px; padding: 0 8px; height: 24px; width: 280px; }
.search-box input { background: transparent; border: none; color: var(--tx); font-size: 11px; width: 100%; outline: none; }
.proto-filter { display: inline-flex; gap: 4px; }
.chip { border: 1px solid var(--bd); background: var(--bg-3); color: var(--tx-3); border-radius: 999px; padding: 3px 10px; font-size: 10px; cursor: pointer; }
.chip.active { color: var(--tx); border-color: color-mix(in srgb, var(--pri) 45%, var(--bd)); background: color-mix(in srgb, var(--pri) 16%, var(--bg-3)); }
.btn { display: flex; align-items: center; gap: 6px; padding: 4px 10px; border-radius: 3px; border: 1px solid var(--bd); background: var(--bg-3); color: var(--tx-2); cursor: pointer; font-size: 11px; }
.btn:disabled { opacity: 0.4; cursor: not-allowed; }

.net-list-container { flex: 1; overflow-y: auto; position: relative; }
.net-table { width: 100%; border-collapse: collapse; font-size: 11px; font-family: var(--font-mono); table-layout: fixed; }
.net-table th { text-align: left; padding: 8px 12px; background: var(--bg-3); position: sticky; top: 0; color: var(--tx-3); text-transform: uppercase; font-size: 10px; z-index: 1; border-right: 1px solid var(--bd); }
.net-table td { padding: 5px 12px; border-bottom: 1px solid var(--bd); border-right: 1px solid var(--bd); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.net-table tr:hover { background: var(--bg-4); }
.mono-cell { color: var(--tx-2); }

.badge { font-size: 9px; padding: 1px 5px; border-radius: 3px; border: 1px solid var(--bd); background: var(--bg-4); color: var(--tx-3); font-weight: 600; }
.badge.tcp { color: var(--blue); border-color: color-mix(in srgb, var(--blue) 40%, var(--bd)); }
.badge.udp { color: var(--amber); border-color: color-mix(in srgb, var(--amber) 40%, var(--bd)); }

.state { font-size: 10px; color: var(--tx-3); }
.state.st-established { color: var(--green, #6aad7e); font-weight: 600; }
.state.st-listen { color: var(--blue); }
.state.st-closing { color: var(--tx-4); }
.state.st-syn { color: var(--amber); }

.net-footer { height: 24px; padding: 0 12px; display: flex; align-items: center; font-size: 10px; color: var(--tx-4); background: var(--bg-2); border-top: 1px solid var(--bd); }
.spacer { flex: 1; }
.empty { padding: 24px; text-align: center; color: var(--tx-4); font-size: 11px; }
.spinning { animation: spin 1s linear infinite; }
@keyframes spin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }
</style>
