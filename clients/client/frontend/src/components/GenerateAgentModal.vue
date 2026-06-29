<template>
  <div class="modal">
    <div class="modal-header">
      <h3><Download :size="14" /> Generate Agent</h3>
      <button @click="modalStore.close" class="close-btn"><X :size="18" /></button>
    </div>
    <div class="modal-body">
      <form @submit.prevent="submit">
        <div class="field">
          <label>Listener</label>
          <input :value="listener.name + ' (' + listener.protocol + ')'" readonly class="readonly" />
        </div>
        <div class="field">
          <label>Callback via</label>
          <select v-model="form.callbackMode" @change="onCallbackModeChange">
            <option value="direct">Direct (this listener, port {{ listener.port }})</option>
            <option v-for="t in relayTargets" :key="t.relayId" :value="t.relayId">
              Relay {{ t.relayId }} on #{{ t.implantId }} ({{ t.implantLabel }}) → port {{ t.port }}
            </option>
          </select>
          <p class="hint" v-if="relayTargets.length">
            Chain through a running relay: the agent dials the relay implant's reachable IP + the relay port instead of the core directly. Start relays from an agent's Pivot tab.
          </p>
          <p class="hint" v-else>
            No running relays available. Start a relay from an agent's <strong>Pivot</strong> tab to chain through it.
          </p>
        </div>
        <div class="field">
          <label>Agent Template</label>
          <select v-model="form.agent_type" required @change="onTemplateChange">
            <option v-for="agent in availableAgents" :key="agent.id" :value="agent.id">
              {{ agent.name }} ({{ agent.id }})
            </option>
          </select>
          <p class="hint" v-if="selectedTemplate">
            {{ selectedTemplate.description }}
          </p>
        </div>
        <div class="field">
          <label>Callback Host (LHOST)</label>
          <input v-model="form.host" :placeholder="hostPlaceholder" :readonly="isRelayMode" :class="{ readonly: isRelayMode }" required />
          <p class="hint" v-if="isRelayMode">
            Auto-set to the relay implant's reachable IP ({{ form.host }}). The relay listens on the parent's bind IP — children dial that IP + the relay port.
          </p>
          <p class="hint" v-else>
            The IP address the agent will connect to. To chain through another agent, start a relay on it (Pivot tab) and pick that relay under "Callback via" above.
          </p>
        </div>
        <div class="field" v-if="supportsSleep">
          <label>Sleep Time (seconds)</label>
          <input v-model.number="form.sleep_time" type="number" min="1" max="3600" placeholder="e.g. 5" required />
          <p class="hint" v-if="isCycleTemplate">
            Time between check-ins: the agent connects, drains commands for the Dwell window, closes, sleeps this long, and reconnects — periodic short pulses rather than a persistent channel.
          </p>
          <p class="hint" v-else>
            Callback interval for <strong>Beacon</strong> agents (1-3600 seconds) — baked into the stub so the agent checks in at this cadence. Ignored by the stock implant, which holds a persistent channel.
          </p>
        </div>
        <div class="field" v-if="supportsDwell">
          <label>Dwell Time (seconds)</label>
          <input v-model.number="form.dwell_time" type="number" min="1" max="600" placeholder="e.g. 2" required />
          <p class="hint">
            How long each connection stays open to receive commands before closing. Shorter = stealthier (shorter pulses); longer = more time to queue commands per check-in.
          </p>
        </div>
        <div class="field field-check">
          <label class="check">
            <input v-model="form.beacon" type="checkbox" :disabled="!canToggleBeacon" />
            <span>Beacon (auto-reconnect)</span>
          </label>
          <p class="hint">
            Generate a beacon agent instead of the stock implant: it reverse-connects, and when the server closes or is unreachable it sleeps the Sleep Time (±jitter-free) and checks in again — forever. Restart or take down the server without losing the agent. Relay/pivot is disabled on a beacon (intermittent link).
          </p>
        </div>
        <div class="field field-check">
          <label class="check">
            <input v-model="form.silent" type="checkbox" />
            <span>Silent (no console window)</span>
          </label>
          <p class="hint">GUI-subsystem agent — runs hidden in the background, no cmd window on launch. Command-exec BOFs already spawn their children hidden, so the whole agent is silent.</p>
        </div>
      </form>
    </div>
    <div class="modal-footer">
      <button class="btn" @click="modalStore.close">Cancel</button>
      <button class="btn primary" @click="submit" :disabled="loading || (isRelayMode && !form.host)">
        {{ loading ? 'Compiling Agent...' : 'Generate & Download' }}
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { reactive, ref, computed, onMounted, watch } from 'vue'
import { X, Download } from 'lucide-vue-next'
import { useModalStore } from '../stores/modal'
import { useToastStore } from '../stores/toast'
import { useAppStore } from '../stores/app'
import { getDefaultCallbackHost } from '../runtime/env'
import api from '../services/api'

const props = defineProps<{
  listener: any
}>()

const modalStore = useModalStore()
const toast = useToastStore()
const appStore = useAppStore()
const loading = ref(false)
// Agent template entry — mirrors the core's GET /api/agent/templates response.
interface AgentTemplate {
  id: string
  name: string
  description: string
  base: string
  variant: string
  supports: string[]
  default_sleep: number
  default_dwell: number
}
const availableAgents = ref<AgentTemplate[]>([])
const cacheKey = `ghost-generate-agent:${props.listener.id}`

// Relay targets discovered across all online implants. Each entry lets the
// operator route the new agent's callback through a running pivot listener
// instead of dialing the core directly.
interface RelayTarget {
  relayId: string
  implantId: string
  implantLabel: string
  port: number
  reachableHost: string
}
const relayTargets = ref<RelayTarget[]>([])

const form = reactive({
  agent_type: '',
  host: getDefaultCallbackHost(),
  sleep_time: 5,
  dwell_time: 2,
  silent: true,
  beacon: false,
  callbackMode: 'direct'
})

const isRelayMode = computed(() => form.callbackMode !== 'direct')
const hostPlaceholder = computed(() => isRelayMode.value ? form.host : 'e.g. 192.168.1.100')

// The currently-selected template object (lookup by id), or null.
const selectedTemplate = computed(() =>
  availableAgents.value.find((t) => t.id === form.agent_type) || null
)
const supportsSleep = computed(() => !!selectedTemplate.value?.supports.includes('sleep'))
const supportsDwell = computed(() => !!selectedTemplate.value?.supports.includes('dwell'))
const isCycleTemplate = computed(() => selectedTemplate.value?.variant === 'beacon-cycle')
// The beacon checkbox only makes sense for the stock implant template (the
// beacon & short-cycle templates are inherently beacon/cycle). Disabled
// otherwise so the operator can't request a nonsensical combo.
const canToggleBeacon = computed(() => selectedTemplate.value?.variant === 'implant')

const loadCachedForm = () => {
  try {
    const raw = localStorage.getItem(cacheKey)
    if (!raw) return
    const cached = JSON.parse(raw)
    form.host = cached.host || form.host
    form.sleep_time = Number(cached.sleep_time || form.sleep_time)
    form.dwell_time = Number(cached.dwell_time || form.dwell_time)
    form.agent_type = cached.agent_type || form.agent_type
    form.silent = cached.silent ?? form.silent
    form.beacon = cached.beacon ?? form.beacon
    // callbackMode is session-scoped (relays are transient) — don't restore it.
  } catch {}
}

const saveCachedForm = () => {
  localStorage.setItem(cacheKey, JSON.stringify({
    host: form.host,
    sleep_time: form.sleep_time,
    dwell_time: form.dwell_time,
    agent_type: form.agent_type,
    silent: form.silent,
    beacon: form.beacon
  }))
}

const fetchAvailableAgents = async () => {
  try {
    const res = await api.get('/agent/templates')
    const list: AgentTemplate[] = res.data?.data || []
    if (list.length) {
      availableAgents.value = list
    } else {
      // No templates from core (e.g. older core without the endpoint) — fall
      // back to the stock implant so the modal still works.
      availableAgents.value = [{
        id: 'ruststrike-implant', name: 'RustStrike Implant',
        description: 'BOF implant with baked-in callback', base: 'ruststrike-implant.exe',
        variant: 'implant', supports: ['silent'], default_sleep: 0, default_dwell: 0,
      }]
    }
  } catch {
    availableAgents.value = [{
      id: 'ruststrike-implant', name: 'RustStrike Implant',
      description: 'BOF implant with baked-in callback', base: 'ruststrike-implant.exe',
      variant: 'implant', supports: ['silent'], default_sleep: 0, default_dwell: 0,
    }]
  }
  // Keep a valid selection: preserve cached agent_type if still present, else
  // the first template, else ruststrike-implant.
  const ids = availableAgents.value.map((t) => t.id)
  if (!ids.includes(form.agent_type)) {
    form.agent_type = ids[0] || 'ruststrike-implant'
  }
  applyTemplateDefaults()
}

// When the operator picks a template, apply its default sleep/dwell and sync
// the beacon checkbox to the template's variant (implant template = user's
// choice; beacon/cycle templates force the flag on).
const onTemplateChange = () => {
  applyTemplateDefaults(true)
}

const applyTemplateDefaults = (forceCadence = false) => {
  const t = selectedTemplate.value
  if (!t) return
  if (forceCadence || form.sleep_time === 0) {
    form.sleep_time = t.default_sleep || (t.supports.includes('sleep') ? 5 : 0)
  }
  if (t.supports.includes('dwell')) {
    if (forceCadence || !form.dwell_time) {
      form.dwell_time = t.default_dwell || 2
    }
  }
  // The beacon checkbox is meaningful only for the implant template; for a
  // beacon/cycle template the variant is implied, so turn the flag off (the
  // submit path derives beacon/cycle from the template, not this checkbox).
  if (t.variant !== 'implant') {
    form.beacon = false
  }
}

// Discover running relays across all online implants so the operator can pick
// one as the callback target. Relays are transient (in-implant), so this is
// fetched fresh each time the modal opens — never cached.
const fetchRelayTargets = async () => {
  const beacons = appStore.beacons
  const targets: RelayTarget[] = []
  await Promise.all(beacons.map(async (b: any) => {
    const id = String(b.node_id || b.id)
    if (!id) return
    try {
      const res = await api.get(`/nodes/${id}/relays`, { silentError: true } as any)
      const relays: any[] = res.data?.data || []
      for (const r of relays) {
        if (r.state !== 'running' || !r.port) continue
        // Reachable host: the relay's bind IP (if not wildcard), else the
        // implant's detected internal/external IP.
        const bind = r.bind_ip && r.bind_ip !== '0.0.0.0' ? r.bind_ip : ''
        const host = bind || b.internal_ip || b.external_ip || ''
        if (!host || host === '-') return
        targets.push({
          relayId: r.id,
          implantId: id,
          implantLabel: b.computer || b.hostname || id,
          port: r.port,
          reachableHost: host
        })
      }
    } catch { /* per-implant fetch is best-effort */ }
  }))
  relayTargets.value = targets
}

const onCallbackModeChange = () => {
  if (form.callbackMode === 'direct') {
    // Restore a manual host (last cached or the default).
    form.host = getDefaultCallbackHost()
    return
  }
  const t = relayTargets.value.find((x) => x.relayId === form.callbackMode)
  if (t) form.host = t.reachableHost
}

const submit = async () => {
  if (!form.host) return
  loading.value = true
  try {
    // Port: the relay port when chaining, else the listener's port.
    const port = isRelayMode.value
      ? String(relayTargets.value.find((x) => x.relayId === form.callbackMode)?.port ?? '')
      : String(props.listener?.port ?? '')
    const safeName = String(props.listener?.name || 'agent').replace(/[^a-z0-9_-]/gi, '_')
    const via = isRelayMode.value ? `_via_${form.callbackMode}` : ''
    // Derive the filename variant marker from the actual resolved variant so it
    // always reflects what the server builds: the cycle template → _cycle, the
    // beacon template OR the implant template with the beacon checkbox → _beacon.
    // (The server lets beacon/cycle booleans override the template's variant
    // only for the implant template, so isCycleTemplate covers the template path
    // and form.beacon covers the checkbox path.)
    const variant = isCycleTemplate.value ? '_cycle'
      : (form.beacon || selectedTemplate.value?.variant === 'beacon' ? '_beacon' : '')
    const name = `ruststrike${variant}_${safeName}_${form.host}_${port}${via}`
    const res = await api.post('/stub/build', {
      host: form.host,
      port,
      name,
      silent: form.silent,
      beacon: form.beacon,
      cycle: isCycleTemplate.value,
      sleep: form.sleep_time,
      dwell: supportsDwell.value ? form.dwell_time : 0,
      template: form.agent_type,
    })
    const p = res.data?.data?.path
    if (!p) {
      // empty path = operator cancelled the Save As dialog
      modalStore.close()
      return
    }
    toast.success(`Agent stub saved: ${p}`)
    modalStore.close()
  } catch (err: any) {
    toast.error(err?.message || 'Agent generation failed')
  } finally {
    loading.value = false
  }
}

onMounted(() => {
  loadCachedForm()
  fetchAvailableAgents()
  fetchRelayTargets()
})

watch(form, () => {
  saveCachedForm()
}, { deep: true })
</script>

<style scoped>
.modal { width: 380px; background: var(--bg-2); border: 1px solid var(--bd); border-radius: 8px; box-shadow: 0 30px 60px rgba(0,0,0,0.6); }
.modal-header { padding: 16px; border-bottom: 1px solid var(--bd); display: flex; justify-content: space-between; align-items: center; }
.modal-header h3 { font-size: 14px; color: var(--pri); margin: 0; display: flex; align-items: center; gap: 8px; }
.close-btn { background: transparent; border: none; color: var(--tx-3); cursor: pointer; }
.modal-body { padding: 20px; }
.field { margin-bottom: 16px; }
.field label { display: block; font-size: 11px; color: var(--tx-2); margin-bottom: 8px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; }
.field input, .field select { width: 100%; background: var(--bg-3); border: 1px solid var(--bd); color: var(--tx); padding: 10px; border-radius: 4px; font-size: 12px; outline: none; transition: border-color 0.2s; }
.field input:focus, .field select:focus { border-color: var(--pri); }
.field input.readonly { background: var(--bg-4); cursor: default; border-style: dashed; }
.field-check { margin-bottom: 8px; }
.check { display: flex; align-items: center; gap: 8px; cursor: pointer; font-size: 12px; color: var(--tx); text-transform: none; letter-spacing: normal; font-weight: 500; margin-bottom: 0; }
.check input { width: auto; margin: 0; }
.hint { font-size: 10px; color: var(--tx-4); margin-top: 6px; font-style: italic; }
.hint strong { color: var(--tx-2); font-style: normal; }
.modal-footer { padding: 12px 16px; border-top: 1px solid var(--bd); display: flex; justify-content: flex-end; gap: 10px; }
.btn { padding: 8px 16px; border-radius: 4px; font-size: 12px; cursor: pointer; border: 1px solid var(--bd); background: var(--bg-3); color: var(--tx); font-weight: 600; }
.btn.primary { background: var(--pri); color: var(--on-pri, #062235); border: none; }
.btn:disabled { opacity: 0.5; cursor: not-allowed; }
</style>
