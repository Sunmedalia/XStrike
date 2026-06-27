<template>
  <div class="process-manager">
    <div class="proc-toolbar">
      <div class="search-box">
        <Search :size="12" />
        <input v-model="filter" placeholder="Search processes (name, pid, user)..." />
      </div>
      <div class="spacer"></div>
      <button class="btn" @click="refreshProcs(true)" :disabled="loading">
        <RefreshCw :size="12" :class="{ spinning: loading }" /> Refresh
      </button>
      <button class="btn danger" :disabled="!selectedProc || loading" @click="confirmKillProc">
        <XCircle :size="12" /> Kill Process
      </button>
    </div>

    <div class="proc-list-container">
      <table class="proc-table">
        <thead>
          <tr>
            <th style="width: 80px">PID</th>
            <th style="width: 80px">PPID</th>
            <th>Name</th>
            <th style="width: 100px">Arch</th>
            <th style="width: 180px">User</th>
            <th>Path</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="p in filteredProcs" :key="p.pid" 
              :class="{ selected: selectedProc?.pid === p.pid }"
              @click="selectedProc = p">
            <td>{{ p.pid }}</td>
            <td>{{ p.ppid }}</td>
            <td class="proc-name">{{ p.name }}</td>
            <td><span class="badge">{{ p.arch }}</span></td>
            <td>{{ p.user }}</td>
            <td class="proc-path">{{ p.path }}</td>
          </tr>
        </tbody>
      </table>
    </div>
    
    <div class="proc-footer">
      Total Processes: {{ procs.length }} | Filtered: {{ filteredProcs.length }}
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { Search, RefreshCw, XCircle } from 'lucide-vue-next'
import api from '../services/api'
import { useToastStore } from '../stores/toast'
import { useAppStore } from '../stores/app'
import { useModalStore } from '../stores/modal'
import ConfirmModal from './ConfirmModal.vue'
import { auditTaskInput } from '../services/taskAudit'

const processListCache: Map<string, any[]> = (() => {
  const g = globalThis as any
  if (!g.__ghostProcessListCache) {
    g.__ghostProcessListCache = new Map<string, any[]>()
  }
  return g.__ghostProcessListCache as Map<string, any[]>
})()

const props = defineProps<{ targetId: string }>()
const toast = useToastStore()
const appStore = useAppStore()
const modalStore = useModalStore()

const filter = ref('')
const selectedProc = ref<any>(null)
const procs = ref<any[]>([])
const loading = ref(false)

const encodeBeaconString = (value: string): number[] => {
  const bytes = Array.from(new TextEncoder().encode(value))
  const len = bytes.length + 1
  return [len & 0xff, (len >> 8) & 0xff, ...bytes, 0]
}

const findBof = (pattern: RegExp) => appStore.bofs.find(b => pattern.test(b.name))

const updateCache = () => {
  processListCache.set(props.targetId, [...procs.value])
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

const filteredProcs = computed(() => {
  const s = filter.value.toLowerCase()
  if (!s) return procs.value
  return procs.value.filter(p => 
    p.name.toLowerCase().includes(s) || 
    p.pid.toString().includes(s) || 
    p.user.toLowerCase().includes(s)
  )
})

const refreshProcs = async (force = false) => {
  if (!force) {
    const cached = processListCache.get(props.targetId)
    if (cached) {
      procs.value = [...cached]
      return
    }
  }

  loading.value = true
  try {
    const procListBof = findBof(/^proc_list\b/i)
    if (!procListBof) {
      toast.error('No proc_list BOF found. Upload proc_list.o first.')
      return
    }

    await auditTaskInput({
      source: 'process:list',
      nodeId: props.targetId,
      input: '(proc_list)'
    })
    const res = await api.post('/bof/execute', {
      node_id: props.targetId,
      bof_name: procListBof.name,
      plugin_name: procListBof.plugin_name || ''
    })
    const result = await pollTaskResult(res.data.data, 60)
    if (!result.success) {
      toast.error(result.error || 'Process list failed')
      return
    }

    procs.value = (result.output || '')
      .trim()
      .split('\n')
      .map((line: string) => line.trim())
      .filter((line: string) => line.length > 0)
      .map((line: string) => {
        const parts = line.split('\t')
        if (parts.length < 4) return null
        return {
          name: parts[0],
          pid: Number(parts[1]) || 0,
          ppid: Number(parts[2]) || 0,
          arch: '-',
          user: '-',
          path: '-',
          threads: Number(parts[3]) || 0
        }
      })
      .filter(Boolean)
    updateCache()
    selectedProc.value = null
  } catch (err: any) {
    toast.error(err.message || 'Failed to query process list')
  } finally {
    loading.value = false
  }
}

const killProc = async () => {
  if (!selectedProc.value) {
    return
  }
  try {
    const procKillBof = findBof(/^proc_kill\b/i)
    if (!procKillBof) {
      toast.error('No proc_kill BOF found. Upload proc_kill.o first.')
      return
    }
    await auditTaskInput({
      source: 'process:kill',
      nodeId: props.targetId,
      input: `pid=${selectedProc.value.pid}`
    })
    const res = await api.post('/bof/execute', {
      node_id: props.targetId,
      bof_name: procKillBof.name,
      plugin_name: procKillBof.plugin_name || '',
      args: encodeBeaconString(String(selectedProc.value.pid))
    })
    const result = await pollTaskResult(res.data.data, 60)
    if (!result.success) {
      toast.error(result.error || 'Kill process failed')
      return
    }
    toast.success(`Killed ${selectedProc.value.name} (${selectedProc.value.pid})`)
    await refreshProcs()
  } catch (err: any) {
    toast.error(err.message || 'Kill process failed')
  }
}

const confirmKillProc = () => {
  if (!selectedProc.value) return
  const p = selectedProc.value
  modalStore.open(ConfirmModal, {
    title: 'Kill Process',
    message: `Kill process "${p.name}" (PID ${p.pid}) on target ${props.targetId}?`,
    confirmText: 'Kill',
    type: 'danger',
    onResolve: async () => {
      await killProc()
    }
  })
}

// Hydrate the last process snapshot from the persisted store (survives restart).
// Used when the in-memory cache is empty (first open after restart) and the
// live BOF didn't return rows (implant offline).
const hydrateFromDb = async () => {
  const id = Number(props.targetId)
  if (!id) return
  try {
    const res = await api.get(`/agents/${id}/artifacts`, { params: { kind: 'proc_list', limit: 1 }, silentError: true } as any)
    const arts: any[] = res.data?.data || []
    if (!arts.length) return
    const meta = arts[0].meta
    if (!meta) return
    procs.value = meta
      .trim().split('\n').map((l: string) => l.trim()).filter(Boolean)
      .map((line: string) => {
        const parts = line.split('\t')
        if (parts.length < 4) return null
        return { name: parts[0], pid: Number(parts[1]) || 0, ppid: Number(parts[2]) || 0,
                 arch: '-', user: '-', path: '-', threads: Number(parts[3]) || 0 }
      }).filter(Boolean)
    if (procs.value.length) updateCache()
  } catch { /* silent */ }
}

onMounted(async () => {
  window.addEventListener('ghost:sync', onSync as EventListener)
  await refreshProcs(false)
  // If the live refresh came up empty (no cache + implant offline), restore
  // the last snapshot from the persisted store.
  if (!procs.value.length) {
    await hydrateFromDb()
  }
})
onUnmounted(() => {
  window.removeEventListener('ghost:sync', onSync as EventListener)
})

const onSync = async () => {
  await refreshProcs(true)
}
</script>

<style scoped>
.process-manager { display: flex; flex-direction: column; height: 100%; background: var(--bg); }
.proc-toolbar { height: 36px; display: flex; align-items: center; gap: 8px; padding: 0 12px; background: var(--bg-2); border-bottom: 1px solid var(--bd); }
.search-box { display: flex; align-items: center; gap: 8px; background: var(--bg-3); border: 1px solid var(--bd); border-radius: 4px; padding: 0 8px; height: 24px; width: 280px; }
.search-box input { background: transparent; border: none; color: var(--tx); font-size: 11px; width: 100%; outline: none; }
.btn { display: flex; align-items: center; gap: 6px; padding: 4px 10px; border-radius: 3px; border: 1px solid var(--bd); background: var(--bg-3); color: var(--tx-2); cursor: pointer; font-size: 11px; }
.btn.danger:not(:disabled) { color: var(--red); border-color: var(--red); }
.btn:disabled { opacity: 0.4; cursor: not-allowed; }

.proc-list-container { flex: 1; overflow-y: auto; }
.proc-table { width: 100%; border-collapse: collapse; font-size: 11px; font-family: var(--font-mono); table-layout: fixed; }
.proc-table th { text-align: left; padding: 8px 12px; background: var(--bg-3); position: sticky; top: 0; color: var(--tx-3); text-transform: uppercase; font-size: 10px; z-index: 1; border-right: 1px solid var(--bd); }
.proc-table td { padding: 6px 12px; border-bottom: 1px solid var(--bd); border-right: 1px solid var(--bd); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.proc-table tr:hover { background: var(--bg-4); cursor: pointer; }
.proc-table tr.selected { background: rgba(64, 196, 99, 0.15); }

.proc-name { color: var(--blue); font-weight: 600; }
.proc-path { color: var(--tx-4); font-size: 10px; }
.badge { font-size: 9px; padding: 1px 4px; background: var(--bg-4); color: var(--tx-3); border-radius: 3px; border: 1px solid var(--bd); }

.proc-footer { height: 24px; padding: 0 12px; display: flex; align-items: center; font-size: 10px; color: var(--tx-4); background: var(--bg-2); border-top: 1px solid var(--bd); }
.spacer { flex: 1; }
.spinning { animation: spin 1s linear infinite; }
@keyframes spin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }
</style>
