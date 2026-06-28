<template>
  <div class="task-log">
    <div class="toolbar">
      <div class="search-box">
        <Search :size="14" />
        <input v-model="search" placeholder="Filter logs..." />
      </div>
      <div class="dbg-toggle" :class="{ on: debugState }" @click="toggleDebug" title="Toggle server debug logging">
        <span>DEBUG</span>
        <div class="sw"></div>
      </div>
      <span class="dbg-level">LEVEL: {{ debugLevel }}</span>
      <div class="spacer"></div>
      <button class="btn" @click="appStore.fetchLogs()">
        <RefreshCw :size="12" /> Refresh
      </button>
      <button class="btn danger" @click="clearLogs">
        <Trash2 :size="12" /> Clear All
      </button>
    </div>

    <div class="log-container">
      <table class="log-table">
        <thead>
          <tr>
            <th style="width: 160px">Timestamp</th>
            <th style="width: 100px">Node ID</th>
            <th style="width: 120px">Type</th>
            <th>Summary / Result</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="log in filteredLogs" :key="log.id" class="log-row" :class="{ 'node-event': log.log_type === 'node_online' || log.log_type === 'node_offline' }">
            <td class="time">{{ formatTime(log.timestamp) }}</td>
            <td class="node">{{ log.node_id }}</td>
            <td><span class="badge" :class="log.log_type">{{ log.log_type }}</span></td>
            <td class="message">
              <div class="summary">{{ log.message }}</div>
              <pre v-if="log.data" class="details">{{ log.data }}</pre>
            </td>
          </tr>
          <tr v-if="filteredLogs.length === 0">
            <td colspan="4" class="empty">No logs found</td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { useAppStore } from '../stores/app'
import { useModalStore } from '../stores/modal'
import { Search, RefreshCw, Trash2 } from 'lucide-vue-next'
import ConfirmModal from './ConfirmModal.vue'
import api from '../services/api'
import { useToastStore } from '../stores/toast'

const appStore = useAppStore()
const toast = useToastStore()
const modalStore = useModalStore()
const search = ref('')
const debugState = ref(true)
const debugLevel = ref('DEBUG')
const debugLoading = ref(false)

const filteredLogs = computed(() => {
  if (!search.value) return appStore.logs
  const s = search.value.toLowerCase()
  return appStore.logs.filter(l => 
    l.message.toLowerCase().includes(s) || 
    l.node_id.toLowerCase().includes(s) ||
    l.log_type.toLowerCase().includes(s)
  )
})

const formatTime = (ts: number) => {
  const d = new Date(ts)
  const hms = d.toLocaleTimeString([], { hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit' })
  const ms = String(d.getMilliseconds()).padStart(3, '0')
  return `${d.toLocaleDateString()} ${hms}.${ms}`
}

const clearLogs = () => {
  modalStore.open(ConfirmModal, {
    title: 'Clear All Logs',
    message: 'This will permanently delete all task logs. This action cannot be undone.',
    confirmText: 'Clear All',
    type: 'danger',
    onResolve: async () => {
      await api.post('/logs/clear')
      await appStore.fetchLogs()
      modalStore.close()
    }
  })
}

const loadDebugStatus = async () => {
  try {
    const res = await api.get('/debug', { silentError: true } as any)
    const data = res?.data?.data || {}
    debugState.value = Boolean(data.debug)
    debugLevel.value = String(data.level || (debugState.value ? 'DEBUG' : 'INFO')).toUpperCase()
  } catch {
    // best effort
  }
}

const toggleDebug = async () => {
  if (debugLoading.value) return
  debugLoading.value = true
  const next = !debugState.value
  try {
    const res = await api.put('/debug', { debug: next })
    const data = res?.data?.data || {}
    debugState.value = Boolean(data.debug)
    debugLevel.value = String(data.level || (debugState.value ? 'DEBUG' : 'INFO')).toUpperCase()
    toast.success(`Log level: ${debugLevel.value}`)
  } catch (err) {
    // interceptor already toasts error
  } finally {
    debugLoading.value = false
  }
}

onMounted(() => {
  loadDebugStatus()
})
</script>

<style scoped>
.task-log {
  display: flex;
  flex-direction: column;
  height: 100%;
  padding: 12px;
  background: var(--bg);
}
.toolbar {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-bottom: 12px;
}
.dbg-toggle {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  height: 24px;
  padding: 0 8px;
  border: 1px solid var(--bd);
  border-radius: 999px;
  background: var(--bg-3);
  color: var(--tx-3);
  cursor: pointer;
  user-select: none;
}
.dbg-toggle span {
  font-size: 10px;
  font-weight: 700;
  letter-spacing: 0.4px;
}
.dbg-toggle .sw {
  width: 22px;
  height: 12px;
  border-radius: 999px;
  background: var(--bg-4);
  position: relative;
}
.dbg-toggle .sw::after {
  content: '';
  position: absolute;
  top: 1px;
  left: 1px;
  width: 10px;
  height: 10px;
  border-radius: 50%;
  background: var(--tx-4);
  transition: all .14s ease;
}
.dbg-toggle.on {
  border-color: var(--pri);
  color: var(--pri);
  background: rgba(64, 196, 99, 0.08);
}
.dbg-toggle.on .sw::after {
  left: 11px;
  background: var(--pri);
}
.dbg-level {
  font-size: 10px;
  color: var(--tx-4);
  font-family: var(--font-mono);
}
.search-box {
  display: flex;
  align-items: center;
  gap: 8px;
  background: var(--bg-3);
  border: 1px solid var(--bd);
  padding: 4px 10px;
  border-radius: 4px;
  width: 240px;
}
.search-box input {
  background: transparent;
  border: none;
  color: var(--tx);
  font-size: 12px;
  outline: none;
  width: 100%;
}
.btn {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 4px 10px;
  border-radius: 3px;
  border: 1px solid var(--bd);
  background: var(--bg-3);
  color: var(--tx-2);
  cursor: pointer;
  font-size: 11px;
}
.btn:hover { border-color: var(--tx-3); color: var(--tx); }
.btn.danger:hover { color: var(--red); border-color: var(--red); }
.spacer { flex: 1; }

.log-container {
  flex: 1;
  overflow-y: auto;
  border: 1px solid var(--bd);
  border-radius: 4px;
}
.log-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 11px;
}
.log-table th {
  text-align: left;
  padding: 8px 12px;
  background: var(--bg-2);
  border-bottom: 1px solid var(--bd);
  color: var(--tx-3);
  position: sticky;
  top: 0;
}
.log-row td {
  padding: 8px 12px;
  border-bottom: 1px solid var(--bd);
  vertical-align: top;
}
.time { color: var(--tx-3); font-family: var(--font-mono); }
.node { font-family: var(--font-mono); color: var(--blue); }
.badge {
  padding: 2px 6px;
  border-radius: 3px;
  font-size: 10px;
  font-weight: 700;
  text-transform: uppercase;
  background: var(--bg-4);
}
.badge.task { color: var(--amber); }
.badge.check { color: var(--purple); }
.badge.system { color: var(--blue); }
.badge.node_online { color: var(--pri); background: rgba(64, 196, 99, 0.12); }
.badge.node_offline { color: var(--red); background: rgba(220, 60, 60, 0.12); }
.log-row.node-event td { background: rgba(64, 196, 99, 0.04); }

.message {
  word-break: break-all;
}
.summary {
  font-weight: 500;
  margin-bottom: 4px;
}
.details {
  background: var(--bg-2);
  padding: 8px;
  border-radius: 4px;
  margin: 4px 0;
  white-space: pre-wrap;
  color: var(--tx-2);
  font-family: var(--font-mono);
}
.empty {
  text-align: center;
  padding: 40px;
  color: var(--tx-4);
}
</style>
