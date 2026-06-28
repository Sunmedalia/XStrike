<template>
  <div class="confirm-modal">
    <div class="modal-header">
      <div class="title-group">
        <AlertTriangle v-if="type === 'danger'" class="icon danger" :size="18" />
        <HelpCircle v-else class="icon info" :size="18" />
        <h3>{{ title }}</h3>
      </div>
    </div>
    <div class="modal-body">
      <p>{{ message }}</p>
    </div>
    <div class="modal-footer">
      <button class="btn" @click="modalStore.close">Cancel</button>
      <button class="btn" :class="type === 'danger' ? 'danger' : 'primary'" @click="onConfirm" :disabled="loading">
        {{ loading ? 'Processing...' : confirmText }}
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref } from 'vue'
import { AlertTriangle, HelpCircle } from 'lucide-vue-next'
import { useModalStore } from '../stores/modal'

const props = defineProps<{
  title: string
  message: string
  confirmText?: string
  type?: 'info' | 'danger'
  onResolve: () => Promise<void>
}>()

const modalStore = useModalStore()
const loading = ref(false)
const confirmText = props.confirmText || 'Confirm'

const onConfirm = async () => {
  loading.value = true
  try {
    await props.onResolve()
    modalStore.close()
  } catch (err) {
    // 错误处理由 API 拦截器负责
  } finally {
    loading.value = false
  }
}
</script>

<style scoped>
.confirm-modal { width: 320px; background: var(--bg-2); border: 1px solid var(--bd); border-radius: 8px; box-shadow: 0 20px 60px rgba(0,0,0,0.6); }
.modal-header { padding: 16px; border-bottom: 1px solid var(--bd); }
.title-group { display: flex; align-items: center; gap: 10px; }
.title-group h3 { margin: 0; font-size: 14px; color: var(--tx); }
.icon.danger { color: var(--red); }
.icon.info { color: var(--blue); }
.modal-body { padding: 16px; font-size: 13px; color: var(--tx-2); line-height: 1.5; }
.modal-footer { padding: 12px 16px; border-top: 1px solid var(--bd); display: flex; justify-content: flex-end; gap: 8px; }
.btn { padding: 6px 16px; border-radius: 4px; font-size: 12px; cursor: pointer; border: 1px solid var(--bd); background: var(--bg-3); color: var(--tx); font-weight: 500; }
.btn.primary { background: var(--pri); color: var(--bg); border: none; }
.btn.danger { background: var(--red); color: white; border: none; }
.btn:disabled { opacity: 0.5; cursor: not-allowed; }
</style>
