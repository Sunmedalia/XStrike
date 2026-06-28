<template>
  <div class="modal">
    <div class="modal-header">
      <h3><Pencil :size="14" /> Edit Listener</h3>
      <button @click="modalStore.close" class="close-btn"><X :size="18" /></button>
    </div>
    <div class="modal-body">
      <form @submit.prevent="submit">
        <div class="field">
          <label>Name</label>
          <input v-model="form.name" required autofocus />
        </div>
        <div class="field">
          <label>Protocol</label>
          <input :value="(listener.protocol || '').toUpperCase()" readonly class="readonly" />
        </div>
        <div class="field">
          <label>Bind IP</label>
          <input v-model="form.bind_ip" required />
        </div>
        <div class="field">
          <label>Port</label>
          <input v-model.number="form.port" type="number" min="1" max="65535" required />
        </div>
      </form>
    </div>
    <div class="modal-footer">
      <button class="btn" @click="modalStore.close">Cancel</button>
      <button class="btn primary" @click="submit" :disabled="loading">
        {{ loading ? 'Saving...' : 'Save' }}
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { reactive, ref } from 'vue'
import { X, Pencil } from 'lucide-vue-next'
import { useModalStore } from '../stores/modal'
import { useAppStore } from '../stores/app'
import { useToastStore } from '../stores/toast'
import api from '../services/api'

const props = defineProps<{ listener: any }>()
const listener = props.listener
const modalStore = useModalStore()
const appStore = useAppStore()
const toast = useToastStore()
const loading = ref(false)

const form = reactive({
  name: listener.name || '',
  bind_ip: listener.bind_ip || '0.0.0.0',
  port: Number(listener.port || 8080)
})

const submit = async () => {
  if (!form.name) return
  loading.value = true
  try {
    const res = await api.put(`/listeners/${listener.id}`, {
      name: form.name,
      bind_ip: form.bind_ip,
      port: form.port
    })
    if (res.data.success) {
      toast.success('Listener updated')
      await appStore.fetchListeners()
      modalStore.close()
    }
  } finally {
    loading.value = false
  }
}
</script>

<style scoped>
.modal { width: 360px; background: var(--bg-2); border: 1px solid var(--bd); border-radius: 8px; box-shadow: 0 20px 50px rgba(0,0,0,0.5); }
.modal-header { padding: 12px 16px; border-bottom: 1px solid var(--bd); display: flex; justify-content: space-between; align-items: center; }
.modal-header h3 { font-size: 14px; color: var(--pri); margin: 0; display: flex; align-items: center; gap: 8px; }
.close-btn { background: transparent; border: none; color: var(--tx-3); cursor: pointer; }
.modal-body { padding: 16px; }
.field { margin-bottom: 16px; }
.field label { display: block; font-size: 11px; color: var(--tx-2); margin-bottom: 6px; }
.field input { width: 100%; background: var(--bg-3); border: 1px solid var(--bd); color: var(--tx); padding: 8px; border-radius: 4px; font-size: 12px; outline: none; }
.field input:focus { border-color: var(--pri); }
.field input.readonly { background: var(--bg-4); color: var(--tx-3); cursor: default; }
.modal-footer { padding: 12px 16px; border-top: 1px solid var(--bd); display: flex; justify-content: flex-end; gap: 8px; }
.btn { padding: 6px 12px; border-radius: 4px; font-size: 12px; cursor: pointer; border: 1px solid var(--bd); background: var(--bg-3); color: var(--tx); }
.btn.primary { background: var(--pri); color: var(--bg); border: none; }
</style>
