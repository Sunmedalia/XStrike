<template>
  <div class="pivot-manager">
    <div class="pivot-toolbar">
      <div class="title"><Network :size="13" /> <span>Pivot / Relay Listeners</span></div>
      <div class="spacer"></div>
      <button class="btn" @click="fetchRelays" :disabled="loading">
        <RefreshCw :size="12" :class="{ spinning: loading }" /> Refresh
      </button>
    </div>

    <div class="pivot-start">
      <div class="row">
        <label>Bind IP</label>
        <select v-model="form.bind_ip" class="inp">
          <option value="0.0.0.0">0.0.0.0 (all interfaces)</option>
          <option v-if="parentInternal" :value="parentInternal">{{ parentInternal }} (internal)</option>
          <option v-if="parentExternal && parentExternal !== '-'" :value="parentExternal">{{ parentExternal }} (external)</option>
          <option value="__custom">— custom —</option>
        </select>
        <input v-if="form.bind_ip === '__custom'" v-model="customBind" placeholder="enter IP" class="inp" />
        <label>Port</label>
        <input v-model.number="form.port" type="number" min="0" max="65535" placeholder="0 = auto" class="inp port" />
        <button class="btn primary" @click="startRelay" :disabled="starting">
          <Plus :size="12" /> {{ starting ? 'Starting…' : 'Start Relay' }}
        </button>
      </div>
      <p class="hint">
        The implant opens a TCP listener on the chosen bind IP and splices each child connection onto a fresh link to the core.
        Pick the parent's <strong>internal</strong> IP for same-LAN children, or its <strong>external</strong> IP for reachable-from-WAN pivots.
        After it's running, generate the child from the <strong>Listener's Generate Agent</strong> dialog — pick this relay there.
      </p>
    </div>

    <div class="relay-list-container">
      <table class="relay-table">
        <thead>
          <tr>
            <th style="width: 120px">ID</th>
            <th>Listen</th>
            <th style="width: 110px">State</th>
            <th style="width: 110px">Action</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="r in relays" :key="r.id">
            <td class="mono-cell">{{ r.id }}</td>
            <td class="mono-cell">
              <span class="listen-addr">{{ r.bind_ip }}:{{ r.port }}</span>
              <button class="copy" @click="copyConnect(r)" title="Copy connect string">
                <Copy :size="11" />
              </button>
              <span v-if="r.error" class="err">— {{ r.error }}</span>
            </td>
            <td><span class="state" :class="stateClass(r.state)">{{ r.state }}</span></td>
            <td class="action-cell">
              <button class="btn danger sm" @click="stopRelay(r)" :disabled="r.state === 'stopping' || r.state === 'stopped'">
                <Square :size="11" /> Stop
              </button>
            </td>
          </tr>
        </tbody>
      </table>
      <div v-if="!relays.length && !loading" class="empty">No relay listeners. Start one above, then generate the child from a Listener.</div>
    </div>

    <div class="pivot-footer">
      {{ relays.length }} relay(s) · child agents are generated from the Listener's Generate Agent dialog (pick the relay as callback)
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { Network, RefreshCw, Plus, Square, Copy } from 'lucide-vue-next'
import api from '../services/api'
import { useToastStore } from '../stores/toast'
import { useAppStore } from '../stores/app'

const props = defineProps<{ targetId: string }>()
const toast = useToastStore()
const appStore = useAppStore()

const form = ref({ bind_ip: '0.0.0.0', port: 0 })
const customBind = ref('')
const relays = ref<any[]>([])
const loading = ref(false)
const starting = ref(false)
let pollTimer: ReturnType<typeof setInterval> | null = null

// Parent implant's detected IPs (from the auto-run sysinfo BOF). Used to
// populate the bind-IP dropdown so the operator can pick internal vs external
// explicitly instead of guessing.
const parent = computed(() => appStore.beacons.find((x: any) => String(x.id) === String(props.targetId)))
const parentInternal = computed(() => (parent.value?.internal_ip && parent.value.internal_ip !== '-') ? parent.value.internal_ip : '')
const parentExternal = computed(() => (parent.value?.external_ip && parent.value.external_ip !== '-') ? parent.value.external_ip : '')

// The address a child should dial to reach this relay: the bind IP the operator
// chose (the parent listens there), or the parent's detected IP as a fallback.
const relayReachable = (r: any) => {
  const b = r.bind_ip
  if (b && b !== '0.0.0.0') return b
  return parentInternal.value || parentExternal.value || ''
}

const stateClass = (s: string) => {
  if (s === 'running') return 'st-running'
  if (s === 'requested') return 'st-requested'
  if (s === 'failed') return 'st-failed'
  if (s === 'stopped') return 'st-stopped'
  return ''
}

const fetchRelays = async () => {
  loading.value = true
  try {
    const res = await api.get(`/nodes/${props.targetId}/relays`, { silentError: true } as any)
    relays.value = res.data?.data || []
  } catch {
    /* silent — fetched on a timer */
  } finally {
    loading.value = false
  }
}

const startRelay = async () => {
  let bindIP = form.value.bind_ip
  if (bindIP === '__custom') {
    bindIP = customBind.value.trim()
    if (!bindIP) { toast.error('Enter a custom bind IP'); return }
  }
  starting.value = true
  try {
    await api.post(`/nodes/${props.targetId}/relay`, {
      bind_ip: bindIP || '0.0.0.0',
      port: Number(form.value.port) || 0
    })
    toast.success('Relay requested — listening port will appear shortly')
    await fetchRelays()
    // The implant replies relay_started within milliseconds; re-fetch shortly
    // so the requested→running transition (and the bound port) shows up.
    setTimeout(fetchRelays, 600)
  } catch (err: any) {
    toast.error(err?.message || 'Failed to start relay')
  } finally {
    starting.value = false
  }
}

const stopRelay = async (r: any) => {
  try {
    r.state = 'stopping'
    await api.delete(`/nodes/${props.targetId}/relays/${r.id}`)
    toast.success(`Relay ${r.id} stopping`)
    await fetchRelays()
  } catch (err: any) {
    toast.error(err?.message || 'Failed to stop relay')
    await fetchRelays()
  }
}

const copyConnect = async (r: any) => {
  const host = relayReachable(r)
  const s = `${host}:${r.port}`
  try {
    await navigator.clipboard.writeText(s)
    toast.success(`Copied ${s}`)
  } catch {
    toast.error('Copy failed — select the address manually')
  }
}

const onSync = () => fetchRelays()
// React instantly to relay_changed events so the table picks up the
// requested→running transition the moment the implant's relay_started reply
// lands — instead of waiting up to 2s for the poll.
const onRelayChanged = () => fetchRelays()

onMounted(async () => {
  window.addEventListener('ghost:sync', onSync as EventListener)
  window.addEventListener('ghost:relay-changed', onRelayChanged as EventListener)
  await fetchRelays()
  pollTimer = setInterval(fetchRelays, 2000)
})
onUnmounted(() => {
  window.removeEventListener('ghost:sync', onSync as EventListener)
  window.removeEventListener('ghost:relay-changed', onRelayChanged as EventListener)
  if (pollTimer) clearInterval(pollTimer)
})
</script>

<style scoped>
.pivot-manager { display: flex; flex-direction: column; height: 100%; background: var(--bg); }
.pivot-toolbar { height: 36px; display: flex; align-items: center; gap: 8px; padding: 0 12px; background: var(--bg-2); border-bottom: 1px solid var(--bd); }
.title { display: flex; align-items: center; gap: 6px; font-size: 11px; font-weight: 700; color: var(--tx); text-transform: uppercase; letter-spacing: 0.06em; }
.spacer { flex: 1; }

.pivot-start { padding: 12px; border-bottom: 1px solid var(--bd); background: var(--bg-2); }
.row { display: flex; align-items: center; gap: 8px; margin-top: 6px; }
.row:first-child { margin-top: 0; }
.row label { font-size: 10px; color: var(--tx-3); text-transform: uppercase; letter-spacing: 0.05em; }
.inp { background: var(--bg-3); border: 1px solid var(--bd); color: var(--tx); padding: 5px 8px; border-radius: 3px; font-size: 11px; outline: none; width: 180px; }
.inp.port { width: 90px; }
.inp:focus { border-color: var(--pri); }
.hint { margin: 8px 0 0; font-size: 10px; color: var(--tx-4); line-height: 1.5; }
.hint strong { color: var(--tx-2); }

.btn { display: flex; align-items: center; gap: 6px; padding: 4px 10px; border-radius: 3px; border: 1px solid var(--bd); background: var(--bg-3); color: var(--tx-2); cursor: pointer; font-size: 11px; }
.btn:disabled { opacity: 0.4; cursor: not-allowed; }
.btn.primary { background: var(--pri); color: var(--on-pri, #062235); border: none; }
.btn.danger { color: #e57373; border-color: color-mix(in srgb, #e57373 35%, var(--bd)); }
.btn.sm { padding: 3px 8px; font-size: 10px; }

.relay-list-container { flex: 1; overflow-y: auto; }
.relay-table { width: 100%; border-collapse: collapse; font-size: 11px; font-family: var(--font-mono); table-layout: fixed; }
.relay-table th { text-align: left; padding: 8px 12px; background: var(--bg-3); position: sticky; top: 0; color: var(--tx-3); text-transform: uppercase; font-size: 10px; z-index: 1; border-right: 1px solid var(--bd); }
.relay-table td { padding: 6px 12px; border-bottom: 1px solid var(--bd); border-right: 1px solid var(--bd); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.relay-table tr:hover { background: var(--bg-4); }
.mono-cell { color: var(--tx-2); }
.action-cell { display: flex; align-items: center; gap: 6px; }
.listen-addr { color: var(--pri); font-weight: 600; }
.copy { background: transparent; border: none; color: var(--tx-4); cursor: pointer; padding: 0 4px; vertical-align: middle; }
.copy:hover { color: var(--tx); }
.err { color: #e57373; font-family: var(--font-sans); font-size: 10px; }

.state { font-size: 10px; padding: 1px 6px; border-radius: 3px; border: 1px solid var(--bd); }
.state.st-running { color: var(--green, #6aad7e); border-color: color-mix(in srgb, var(--green, #6aad7e) 40%, var(--bd)); background: color-mix(in srgb, var(--green, #6aad7e) 12%, var(--bg-3)); }
.state.st-requested { color: var(--amber); border-color: color-mix(in srgb, var(--amber) 40%, var(--bd)); }
.state.st-failed { color: #e57373; border-color: color-mix(in srgb, #e57373 40%, var(--bd)); }
.state.st-stopped, .state.st-stopping { color: var(--tx-4); }

.pivot-footer { height: 24px; padding: 0 12px; display: flex; align-items: center; font-size: 10px; color: var(--tx-4); background: var(--bg-2); border-top: 1px solid var(--bd); }
.empty { padding: 24px; text-align: center; color: var(--tx-4); font-size: 11px; }
.spinning { animation: spin 1s linear infinite; }
@keyframes spin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }
</style>
