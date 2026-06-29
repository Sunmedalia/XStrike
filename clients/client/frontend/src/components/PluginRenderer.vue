<template>
  <div class="plugin-renderer">
    <!-- Form layout -->
    <template v-if="layout?.type === 'form'">
      <div class="pr-form">
        <div v-for="field in layout.fields" :key="field.name" class="pr-field">
          <label class="pr-label">{{ field.label }}</label>

          <!-- Text input -->
          <input
            v-if="field.type === 'text'"
            v-model="formData[field.name]"
            :placeholder="field.placeholder || ''"
            :required="field.required"
            class="pr-input"
          />

          <!-- Textarea -->
          <textarea
            v-else-if="field.type === 'textarea'"
            v-model="formData[field.name]"
            :placeholder="field.placeholder || ''"
            :rows="field.rows || 4"
            class="pr-input pr-textarea"
          />

          <!-- Number input -->
          <input
            v-else-if="field.type === 'number'"
            v-model.number="formData[field.name]"
            type="number"
            :min="field.min"
            :max="field.max"
            :placeholder="field.placeholder || ''"
            class="pr-input"
          />

          <!-- Select -->
          <select
            v-else-if="field.type === 'select'"
            v-model="formData[field.name]"
            class="pr-input pr-select"
          >
            <option v-for="opt in field.options" :key="opt" :value="opt">{{ opt }}</option>
          </select>

          <!-- Checkbox -->
          <label v-else-if="field.type === 'checkbox'" class="pr-checkbox-wrap">
            <input type="checkbox" v-model="formData[field.name]" />
            <span>{{ formData[field.name] ? 'Yes' : 'No' }}</span>
          </label>

          <!-- Radio group -->
          <div v-else-if="field.type === 'radio'" class="pr-radio-group">
            <label v-for="opt in field.options" :key="opt" class="pr-radio-wrap">
              <input type="radio" :value="opt" v-model="formData[field.name]" />
              <span>{{ opt }}</span>
            </label>
          </div>

          <!-- File input -->
          <input
            v-else-if="field.type === 'file'"
            type="file"
            :accept="field.accept || '*'"
            @change="handleFileField(field.name, $event)"
            class="pr-input"
          />

          <!-- Fallback text -->
          <input
            v-else
            v-model="formData[field.name]"
            :placeholder="field.placeholder || ''"
            class="pr-input"
          />
        </div>

        <!-- Submit button -->
        <div v-if="layout.submit" class="pr-submit-wrap">
          <button
            class="pr-submit-btn"
            :disabled="executing"
            @click="handleSubmit"
          >
            {{ executing ? 'Executing...' : layout.submit.label || 'Submit' }}
          </button>
        </div>
      </div>
    </template>

    <!-- Text/info layout -->
    <template v-else-if="layout?.type === 'text'">
      <div class="pr-text" v-html="sanitizeHtml(layout.content || '')"></div>
    </template>

    <!-- Fallback -->
    <template v-else>
      <div class="pr-empty">
        <div style="font-size:24px;opacity:0.5">🧩</div>
        <div>No layout defined for this panel</div>
      </div>
    </template>

    <!-- Result display -->
    <div v-if="result" class="pr-result" :class="resultClass">
      <pre>{{ result }}</pre>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, reactive, watch } from 'vue'
import DOMPurify from 'dompurify'
import { useToastStore } from '../stores/toast'
import type { PluginPanelLayout, PluginAction } from '../stores/plugin'
import { runPluginBofAction } from '../services/pluginActions'

const props = defineProps<{
  layout: PluginPanelLayout
  targetId?: string
}>()

const toast = useToastStore()
const formData = reactive<Record<string, any>>({})
const executing = ref(false)
const result = ref('')
const resultSuccess = ref(true)

const resultClass = ref('')

const sanitizeHtml = (html: string) =>
  DOMPurify.sanitize(html, {
    USE_PROFILES: { html: true },
    FORBID_TAGS: ['style', 'script', 'iframe', 'object', 'embed'],
  })

// Initialize form data with defaults
const initFormData = () => {
  if (props.layout?.fields) {
    for (const field of props.layout.fields) {
      if (field.default !== undefined) {
        formData[field.name] = field.default
      } else if (field.type === 'checkbox') {
        formData[field.name] = false
      } else if (field.type === 'number') {
        formData[field.name] = field.min || 0
      } else if (field.type === 'select' && field.options?.length) {
        formData[field.name] = field.options[0]
      } else {
        formData[field.name] = ''
      }
    }
  }
}

initFormData()
watch(() => props.layout, initFormData, { deep: true })

const handleFileField = (fieldName: string, event: Event) => {
  const target = event.target as HTMLInputElement
  if (target.files?.length) {
    formData[fieldName] = target.files[0]
  }
}

const executeAction = async (action: PluginAction) => {
  if (action.type === 'execute_bof') {
    if (!props.targetId) {
      toast.error('No target beacon selected')
      return
    }
    if (!action.bof_name) {
      toast.error('No BOF name specified in action')
      return
    }

    executing.value = true
    result.value = ''
    try {
      const taskResult = await runPluginBofAction(action, props.targetId, formData)
      resultSuccess.value = taskResult.success
      resultClass.value = taskResult.success ? 'pr-result-ok' : 'pr-result-err'
      result.value = taskResult.output || taskResult.error || 'No output'
    } catch (e: any) {
      result.value = e.message || 'Execution failed'
      resultClass.value = 'pr-result-err'
    } finally {
      executing.value = false
    }
  }
}

const handleSubmit = () => {
  if (props.layout?.submit?.action) {
    executeAction(props.layout.submit.action)
  }
}
</script>

<style scoped>
.plugin-renderer {
  padding: 16px;
  height: 100%;
  overflow-y: auto;
}
.pr-form {
  display: flex;
  flex-direction: column;
  gap: 14px;
  max-width: 600px;
}
.pr-field {
  display: flex;
  flex-direction: column;
  gap: 4px;
}
.pr-label {
  font-size: 11px;
  font-weight: 600;
  color: var(--tx-2);
  text-transform: uppercase;
  letter-spacing: 0.04em;
}
.pr-input {
  background: var(--bg-3);
  border: 1px solid var(--bd);
  border-radius: 4px;
  padding: 8px 10px;
  color: var(--tx);
  font-size: 12px;
  font-family: var(--font-mono);
  outline: none;
  transition: border-color 0.15s;
}
.pr-input:focus {
  border-color: var(--pri);
}
.pr-textarea {
  resize: vertical;
  min-height: 60px;
}
.pr-select {
  cursor: pointer;
}
.pr-checkbox-wrap {
  display: flex;
  align-items: center;
  gap: 8px;
  cursor: pointer;
  font-size: 12px;
  color: var(--tx);
}
.pr-radio-group {
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.pr-radio-wrap {
  display: flex;
  align-items: center;
  gap: 8px;
  cursor: pointer;
  font-size: 12px;
  color: var(--tx);
}
.pr-submit-wrap {
  padding-top: 8px;
}
.pr-submit-btn {
  background: var(--pri);
  color: #fff;
  border: none;
  border-radius: 4px;
  padding: 8px 20px;
  font-size: 12px;
  font-weight: 600;
  cursor: pointer;
  transition: background 0.15s, opacity 0.15s;
}
.pr-submit-btn:hover {
  background: var(--pri-h);
}
.pr-submit-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}
.pr-result {
  margin-top: 16px;
  padding: 12px;
  border-radius: 4px;
  font-family: var(--font-mono);
  font-size: 11px;
  max-height: 300px;
  overflow-y: auto;
}
.pr-result pre {
  margin: 0;
  white-space: pre-wrap;
  word-break: break-all;
}
.pr-result-ok {
  background: color-mix(in srgb, var(--pri) 10%, transparent);
  border: 1px solid color-mix(in srgb, var(--pri) 28%, transparent);
  color: var(--green, #6aad7e);
}
.pr-result-err {
  background: rgba(197, 116, 116, 0.08);
  border: 1px solid rgba(197, 116, 116, 0.25);
  color: var(--red);
}
.pr-empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 8px;
  padding: 40px;
  color: var(--tx-3);
  font-size: 12px;
}
.pr-text {
  font-size: 13px;
  color: var(--tx);
  line-height: 1.6;
}
</style>
