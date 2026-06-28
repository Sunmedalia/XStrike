<template>
  <div class="modal">
    <div class="modal-header">
      <h3>Execute BOF: {{ bofName }}</h3>
      <button @click="modalStore.close" class="close-btn"><X :size="18" /></button>
    </div>
    <div class="modal-body">
      <form @submit.prevent="submit">
        <div class="field">
          <label>Target Beacon</label>
          <select v-model="form.node_id" required>
            <option v-for="b in appStore.beacons" :key="b.node_id || b.id" :value="b.node_id || b.id">
              {{ b.hostname || b.computer }} ({{ b.node_id || b.id }}) - {{ b.ip || b.internal_ip }}
            </option>
          </select>
        </div>
        <div class="field">
          <label>Command Arguments</label>
          <input v-model="form.command" placeholder="e.g. whoami /all" />
          <p class="hint">Beacon-format: 2-byte LE length + data + null</p>
        </div>
        <div class="field">
          <label>Raw Hex Arguments (Optional)</label>
          <input v-model="form.raw_hex" placeholder="00 01 02 03" />
          <p class="hint">Ignored if command is set</p>
        </div>
      </form>
    </div>
    <div class="modal-footer">
      <button class="btn" @click="modalStore.close">Cancel</button>
      <button class="btn primary" @click="submit" :disabled="loading">
        {{ loading ? 'Executing...' : 'Execute' }}
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { reactive, ref } from 'vue'
import { X } from 'lucide-vue-next'
import { useModalStore } from '../stores/modal'
import { useAppStore } from '../stores/app'
import { useToastStore } from '../stores/toast'
import api from '../services/api'
import { auditTaskInput } from '../services/taskAudit'

const props = defineProps<{
  bofName: string
  pluginName?: string
}>()

const modalStore = useModalStore()
const appStore = useAppStore()
const toast = useToastStore()
const loading = ref(false)

const form = reactive({
  node_id: appStore.beacons[0]?.node_id || appStore.beacons[0]?.id || '',
  bof_name: props.bofName,
  command: '',
  raw_hex: ''
})

const encodeBeaconString = (text: string): number[] => {
  const bytes = Array.from(new TextEncoder().encode(text))
  const len = bytes.length + 1
  return [len & 0xff, (len >> 8) & 0xff, ...bytes, 0]
}

const parseHex = (raw: string): number[] => {
  const cleaned = raw.replace(/\s+/g, '')
  if (!cleaned) return []
  if (!/^[0-9a-fA-F]+$/.test(cleaned) || cleaned.length % 2 !== 0) {
    throw new Error('Raw hex must be even-length hex string')
  }
  const bytes: number[] = []
  for (let i = 0; i < cleaned.length; i += 2) {
    bytes.push(parseInt(cleaned.slice(i, i + 2), 16))
  }
  return bytes
}

const submit = async () => {
  if (!form.node_id) return alert('Select a target beacon')
  loading.value = true
  try {
    const payload: any = {
      node_id: form.node_id,
      bof_name: form.bof_name,
      plugin_name: props.pluginName || ''
    }
    if (form.command.trim()) {
      payload.args = encodeBeaconString(form.command.trim())
    } else if (form.raw_hex.trim()) {
      payload.args = parseHex(form.raw_hex)
    }

    await auditTaskInput({
      source: 'bof:execute',
      nodeId: form.node_id,
      input: form.command.trim() || form.raw_hex.trim() || '(none)'
    })
    const res = await api.post('/bof/execute', payload)
    if (res.data.success) {
      toast.success('BOF execution task created')
      modalStore.close()
      appStore.fetchLogs()
    }
  } catch (err: any) {
    toast.error(err?.message || 'BOF execution failed')
  } finally {
    loading.value = false
  }
}
</script>

<style scoped>
.modal {
  width: 400px;
  background: var(--bg-2);
  border: 1px solid var(--bd);
  border-radius: 8px;
  box-shadow: 0 20px 40px rgba(0,0,0,0.6);
}
.modal-header {
  padding: 12px 16px;
  border-bottom: 1px solid var(--bd);
  display: flex;
  justify-content: space-between;
  align-items: center;
}
.modal-header h3 { font-size: 14px; color: var(--pri); margin: 0; }
.close-btn { background: transparent; border: none; color: var(--tx-3); cursor: pointer; }
.modal-body { padding: 16px; }
.field { margin-bottom: 16px; }
.field label { display: block; font-size: 11px; color: var(--tx-2); margin-bottom: 6px; }
.field select, .field input {
  width: 100%;
  background: var(--bg-3);
  border: 1px solid var(--bd);
  color: var(--tx);
  padding: 8px;
  border-radius: 4px;
  font-size: 12px;
  outline: none;
}
.field select:focus, .field input:focus { border-color: var(--pri); }
.hint { font-size: 10px; color: var(--tx-4); margin-top: 4px; }
.modal-footer {
  padding: 12px 16px;
  border-top: 1px solid var(--bd);
  display: flex;
  justify-content: flex-end;
  gap: 8px;
}
.btn {
  padding: 6px 12px;
  border-radius: 4px;
  font-size: 12px;
  cursor: pointer;
  border: 1px solid var(--bd);
  background: var(--bg-3);
  color: var(--tx);
}
.btn.primary { background: var(--pri); color: var(--on-pri, #062235); border: none; }
.btn:disabled { opacity: 0.5; cursor: not-allowed; }
</style>
