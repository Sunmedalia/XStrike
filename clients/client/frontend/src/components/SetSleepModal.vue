<template>
  <div class="modal-card">
    <div class="modal-header">
      <Clock :size="16" />
      <h3>Set Sleep Interval</h3>
    </div>
    <div class="modal-body">
      <p class="modal-desc">
        Configure sleep interval for {{ targetCount }} agent{{ targetCount > 1 ? 's' : '' }}
      </p>
      <div class="form-group">
        <label>Sleep Interval (seconds)</label>
        <input
          v-model.number="sleepSeconds"
          type="number"
          min="1"
          max="86400"
          placeholder="5"
          @keyup.enter="submit"
          ref="inputRef"
        />
        <span class="form-hint">Range: 1 second to 24 hours (86400 seconds)</span>
      </div>
      <div class="quick-presets">
        <button @click="sleepSeconds = 5" class="preset-btn">5s</button>
        <button @click="sleepSeconds = 10" class="preset-btn">10s</button>
        <button @click="sleepSeconds = 30" class="preset-btn">30s</button>
        <button @click="sleepSeconds = 60" class="preset-btn">1m</button>
        <button @click="sleepSeconds = 300" class="preset-btn">5m</button>
        <button @click="sleepSeconds = 600" class="preset-btn">10m</button>
      </div>
    </div>
    <div class="modal-footer">
      <button class="btn-secondary" @click="cancel">Cancel</button>
      <button class="btn-primary" @click="submit" :disabled="!isValid">Set Sleep</button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { Clock } from 'lucide-vue-next'

const props = defineProps<{
  targetIds: string[]
  onResolve?: (sleepMs: number) => void
  onReject?: () => void
}>()

const sleepSeconds = ref(5)
const inputRef = ref<HTMLInputElement>()

const targetCount = computed(() => props.targetIds.length)
const isValid = computed(() => sleepSeconds.value >= 1 && sleepSeconds.value <= 86400)

const submit = () => {
  if (!isValid.value) return
  const sleepMs = sleepSeconds.value * 1000
  props.onResolve?.(sleepMs)
}

const cancel = () => {
  props.onReject?.()
}

onMounted(() => {
  inputRef.value?.focus()
})
</script>

<style scoped>
.modal-card {
  background: var(--bg);
  border: 1px solid var(--bd);
  border-radius: 8px;
  width: 480px;
  max-width: 90vw;
  box-shadow: 0 8px 32px rgba(0, 0, 0, 0.4);
}

.modal-header {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 16px 20px;
  border-bottom: 1px solid var(--bd);
}

.modal-header h3 {
  margin: 0;
  font-size: 14px;
  font-weight: 600;
  color: var(--tx);
}

.modal-body {
  padding: 20px;
}

.modal-desc {
  margin: 0 0 16px 0;
  font-size: 13px;
  color: var(--tx-2);
}

.form-group {
  margin-bottom: 16px;
}

.form-group label {
  display: block;
  margin-bottom: 8px;
  font-size: 12px;
  font-weight: 600;
  color: var(--tx);
  text-transform: uppercase;
  letter-spacing: 0.05em;
}

.form-group input {
  width: 100%;
  padding: 10px 12px;
  background: var(--bg-2);
  border: 1px solid var(--bd);
  border-radius: 4px;
  color: var(--tx);
  font-size: 13px;
  font-family: var(--font-mono);
  transition: border-color 0.2s;
}

.form-group input:focus {
  outline: none;
  border-color: var(--pri);
}

.form-hint {
  display: block;
  margin-top: 6px;
  font-size: 11px;
  color: var(--tx-3);
}

.quick-presets {
  display: flex;
  gap: 8px;
  flex-wrap: wrap;
}

.preset-btn {
  padding: 6px 12px;
  background: var(--bg-2);
  border: 1px solid var(--bd);
  border-radius: 4px;
  color: var(--tx-2);
  font-size: 11px;
  font-family: var(--font-mono);
  cursor: pointer;
  transition: all 0.2s;
}

.preset-btn:hover {
  background: var(--bg-3);
  border-color: var(--pri);
  color: var(--pri);
}

.modal-footer {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
  padding: 16px 20px;
  border-top: 1px solid var(--bd);
}

.btn-secondary,
.btn-primary {
  padding: 8px 16px;
  border: none;
  border-radius: 4px;
  font-size: 12px;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.2s;
}

.btn-secondary {
  background: var(--bg-2);
  color: var(--tx-2);
}

.btn-secondary:hover {
  background: var(--bg-3);
  color: var(--tx);
}

.btn-primary {
  background: var(--pri);
  color: var(--on-pri, #062235);
}

.btn-primary:hover {
  background: color-mix(in srgb, var(--pri) 85%, white);
}

.btn-primary:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}
</style>
