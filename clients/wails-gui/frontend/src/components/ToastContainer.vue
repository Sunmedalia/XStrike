<template>
  <div class="toast-container">
    <TransitionGroup name="toast">
      <div v-for="t in store.toasts" :key="t.id" class="toast" :class="t.type" @click="store.remove(t.id)">
        <div class="icon">
          <CheckCircle v-if="t.type === 'success'" :size="14" />
          <AlertCircle v-else-if="t.type === 'error'" :size="14" />
          <Info v-else :size="14" />
        </div>
        <div class="message">{{ t.message }}</div>
      </div>
    </TransitionGroup>
  </div>
</template>

<script setup lang="ts">
import { useToastStore } from '../stores/toast'
import { CheckCircle, AlertCircle, Info } from 'lucide-vue-next'
const store = useToastStore()
</script>

<style scoped>
.toast-container {
  position: fixed;
  top: 40px;
  right: 20px;
  z-index: 9999;
  display: flex;
  flex-direction: column;
  gap: 10px;
  pointer-events: none;
}
.toast {
  pointer-events: auto;
  min-width: 200px;
  max-width: 320px;
  background: var(--bg-2);
  border: 1px solid var(--bd);
  border-radius: 4px;
  padding: 10px 16px;
  display: flex;
  align-items: center;
  gap: 12px;
  box-shadow: 0 10px 20px rgba(0,0,0,0.4);
  cursor: pointer;
}
.toast.success { border-left: 4px solid var(--pri); color: var(--pri); }
.toast.error { border-left: 4px solid var(--red); color: var(--red); }
.toast.info { border-left: 4px solid var(--blue); color: var(--blue); }
.message { font-size: 12px; color: var(--tx); font-weight: 500; }

.toast-enter-active, .toast-leave-active { transition: all 0.3s ease; }
.toast-enter-from { opacity: 0; transform: translateX(30px); }
.toast-leave-to { opacity: 0; transform: scale(0.9); }
</style>
