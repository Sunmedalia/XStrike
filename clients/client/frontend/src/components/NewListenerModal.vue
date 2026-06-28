<template>
  <div class="modal">
    <div class="modal-header">
      <h3><Plus :size="14" /> New Listener</h3>
      <button @click="modalStore.close" class="close-btn"><X :size="18" /></button>
    </div>
    <div class="modal-body">
      <form @submit.prevent="submit">
        <div class="field">
          <label>Name</label>
          <input v-model="form.name" placeholder="e.g. HTTP-Main" required autofocus />
        </div>
        <div class="field">
          <label>Protocol</label>
          <select v-model="form.protocol" required>
            <option value="http">HTTP</option>
            <option value="https">HTTPS</option>
            <option value="ws">WebSocket</option>
          </select>
        </div>
        <div class="field">
          <label>Bind IP</label>
          <input v-model="form.bind_ip" placeholder="0.0.0.0" />
        </div>
        <div class="field">
          <label>Port</label>
          <input v-model.number="form.port" type="number" min="1" max="65535" placeholder="e.g. 4443" required />
        </div>
      </form>
    </div>
    <div class="modal-footer">
      <button class="btn" @click="modalStore.close">Cancel</button>
      <button class="btn primary" @click="submit" :disabled="loading">
        {{ loading ? 'Creating...' : 'Create' }}
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { reactive, ref } from 'vue'
import { X, Plus } from 'lucide-vue-next'
import { useModalStore } from '../stores/modal'
import { useAppStore } from '../stores/app'
import { useToastStore } from '../stores/toast' // 确保导入了 toast
import api from '../services/api'

const modalStore = useModalStore()
const appStore = useAppStore()
const toastStore = useToastStore()
const loading = ref(false)

const form = reactive({
  name: '',
  protocol: 'http',
  bind_ip: '0.0.0.0',
  port: 8080
})

const submit = async () => {
  if (!form.name) return
  loading.value = true
  try {
    const res = await api.post('/listeners', form)
    if (res.data.success) {
      toastStore.success('Listener created successfully')
      await appStore.fetchListeners()
      modalStore.close()
    }
  } catch (err: any) {
    // 错误已由 api 拦截器处理
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
.field input, .field select { width: 100%; background: var(--bg-3); border: 1px solid var(--bd); color: var(--tx); padding: 8px; border-radius: 4px; font-size: 12px; outline: none; }
.field input:focus, .field select:focus { border-color: var(--pri); }
.modal-footer { padding: 12px 16px; border-top: 1px solid var(--bd); display: flex; justify-content: flex-end; gap: 8px; }
.btn { padding: 6px 12px; border-radius: 4px; font-size: 12px; cursor: pointer; border: 1px solid var(--bd); background: var(--bg-3); color: var(--tx); }
.btn.primary { background: var(--pri); color: var(--on-pri, #062235); border: none; }
</style>
