<template>
  <div class="sc-wrap">
    <div class="sc-toolbar">
      <div class="field">
        <label>Method</label>
        <select v-model="method">
          <option value="shellcode_exec">VirtualAlloc + CreateThread</option>
          <option value="shellcode_exec_nt">NtCreateThreadEx (recommended)</option>
          <option value="shellcode_exec_ntalloc">NtAllocateVirtualMemory</option>
          <option value="shellcode_exec_heap">HeapAlloc + VirtualProtect</option>
          <option value="shellcode_exec_callback">EnumSystemLocalesA Callback</option>
          <option value="shellcode_exec_fiber">Fiber API</option>
        </select>
      </div>
      <div class="field grow">
        <label>Shellcode (hex)</label>
        <textarea
          v-model="hexInput"
          placeholder="e.g. 90 90 90 c3 or 909090c3"
          spellcheck="false"
        ></textarea>
      </div>
      <div class="actions">
        <button class="btn" :disabled="loading" @click="executeHex">Execute Hex</button>
        <label class="btn file-btn" :class="{ disabled: loading }">
          Load File
          <input type="file" :disabled="loading" @change="loadFileAndExecute" />
        </label>
      </div>
    </div>

    <div class="sc-output">
      <div v-if="taskBanner" class="task-banner">Task created: {{ taskBanner }}</div>
      <div v-for="(line, idx) in outputLines" :key="idx" class="line" :class="line.type">{{ line.text }}</div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted, watch } from 'vue'
import api from '../services/api'
import { useAppStore } from '../stores/app'
import { useToastStore } from '../stores/toast'
import { auditTaskInput } from '../services/taskAudit'

const props = defineProps<{ targetId: string }>()
const appStore = useAppStore()
const toast = useToastStore()

type ShellcodeLine = { text: string; type: 'sys' | 'info' | 'error' }
type ShellcodeSessionCache = {
  method: string
  hexInput: string
  taskBanner: string
  outputLines: ShellcodeLine[]
}
const shellcodeCache: Map<string, ShellcodeSessionCache> = (() => {
  const g = globalThis as any
  if (!g.__ghostShellcodeCache) {
    g.__ghostShellcodeCache = new Map<string, ShellcodeSessionCache>()
  }
  return g.__ghostShellcodeCache as Map<string, ShellcodeSessionCache>
})()

const method = ref('shellcode_exec')
const hexInput = ref('')
const loading = ref(false)
const taskBanner = ref('')
const outputLines = ref<ShellcodeLine[]>([])

const defaultLinesFor = (targetId: string): ShellcodeLine[] => ([
  { text: `Shellcode execution view ready for ${targetId}.`, type: 'sys' },
  { text: 'Only execute trusted payloads in authorized environments.', type: 'sys' }
])
const saveSession = (targetId: string) => {
  shellcodeCache.set(targetId, {
    method: method.value,
    hexInput: hexInput.value,
    taskBanner: taskBanner.value,
    outputLines: [...outputLines.value]
  })
}
const loadSession = (targetId: string) => {
  const cached = shellcodeCache.get(targetId)
  if (cached) {
    method.value = cached.method || 'shellcode_exec_nt'
    hexInput.value = cached.hexInput || ''
    taskBanner.value = cached.taskBanner || ''
    outputLines.value = [...cached.outputLines]
    return
  }
  method.value = 'shellcode_exec_nt'
  hexInput.value = ''
  taskBanner.value = ''
  outputLines.value = defaultLinesFor(targetId)
}

const parseHex = (raw: string): number[] => {
  const cleaned = raw.trim().replace(/\s+/g, '')
  if (!cleaned) throw new Error('No shellcode provided')
  if (!/^[0-9a-fA-F]+$/.test(cleaned)) throw new Error('Invalid hex format')
  if (cleaned.length % 2 !== 0) throw new Error('Hex length must be even')
  const bytes: number[] = []
  for (let i = 0; i < cleaned.length; i += 2) {
    bytes.push(parseInt(cleaned.slice(i, i + 2), 16))
  }
  return bytes
}

const findMethodBof = (methodName: string) => {
  return appStore.bofs.find((b: any) => b.name.toLowerCase().replace(/\.o$/i, '') === methodName.toLowerCase())
}

const pollTaskResult = async (taskId: string, maxRetry = 120): Promise<any> => {
  for (let i = 0; i < maxRetry; i++) {
    try {
      const res = await api.get(`/tasks/${taskId}`, {
        silentError: true,
        validateStatus: (s: number) => s === 200 || s === 404
      } as any)
      if (res.status === 404) {
        await new Promise(resolve => setTimeout(resolve, 1000))
        continue
      }
      if (res.data.success && res.data.data) return res.data.data
    } catch (err: any) {
      throw err
    }
    await new Promise(resolve => setTimeout(resolve, 1000))
  }
  throw new Error('Task timeout')
}

const executeBytes = async (bytes: number[], source: string) => {
  const bof = findMethodBof(method.value)
  if (!bof) {
    outputLines.value.push({ text: `${method.value}.o not found`, type: 'error' })
    return
  }
  loading.value = true
  try {
    const len = bytes.length
    const args = [len & 0xff, (len >> 8) & 0xff, ...bytes]
    outputLines.value.push({ text: `Executing ${bytes.length} bytes via ${method.value}`, type: 'info' })
    await auditTaskInput({
      source: `shellcode:${source}`,
      nodeId: props.targetId,
      input: `${method.value} bytes=${bytes.length}`
    })
    const res = await api.post('/bof/execute', {
      node_id: props.targetId,
      bof_name: bof.name,
      plugin_name: bof.plugin_name || '',
      args
    })
    taskBanner.value = res.data.data
    const result = await pollTaskResult(res.data.data, 120)
    outputLines.value.push({
      text: result.output || result.error || '(empty)',
      type: result.success ? 'info' : 'error'
    })
    await appStore.fetchLogs()
  } catch (err: any) {
    outputLines.value.push({ text: err.message || 'Shellcode execution failed', type: 'error' })
  } finally {
    loading.value = false
  }
}

const executeHex = async () => {
  try {
    const bytes = parseHex(hexInput.value)
    await executeBytes(bytes, 'hex')
  } catch (err: any) {
    toast.error(err.message || 'Invalid hex')
    outputLines.value.push({ text: err.message || 'Invalid hex', type: 'error' })
  }
}

const loadFileAndExecute = async (evt: Event) => {
  const target = evt.target as HTMLInputElement
  const file = target.files?.[0]
  if (!file) return
  try {
    const buf = await file.arrayBuffer()
    const bytes = Array.from(new Uint8Array(buf))
    outputLines.value.push({ text: `Loaded ${bytes.length} bytes from ${file.name}`, type: 'info' })
    await executeBytes(bytes, `file:${file.name}`)
  } catch (err: any) {
    outputLines.value.push({ text: err.message || 'Failed to read file', type: 'error' })
  } finally {
    target.value = ''
  }
}

watch([method, hexInput, taskBanner, outputLines], () => {
  saveSession(props.targetId)
}, { deep: true })

watch(() => props.targetId, (next, prev) => {
  if (prev) saveSession(prev)
  loadSession(next)
})

onMounted(() => {
  loadSession(props.targetId)
})

onUnmounted(() => {
  saveSession(props.targetId)
})
</script>

<style scoped>
.sc-wrap { display: flex; flex-direction: column; height: 100%; background: var(--bg); }
.sc-toolbar { display: grid; grid-template-columns: 260px 1fr auto; gap: 10px; padding: 12px; border-bottom: 1px solid var(--bd); background: var(--bg-2); }
.field { display: flex; flex-direction: column; gap: 6px; }
.field.grow { min-width: 0; }
.field label { font-size: 10px; text-transform: uppercase; color: var(--tx-3); letter-spacing: 0.5px; }
.field select, .field textarea {
  background: var(--bg-3);
  border: 1px solid var(--bd);
  color: var(--tx);
  border-radius: 4px;
  font-size: 12px;
  padding: 8px;
  font-family: var(--font-mono);
  outline: none;
}
.field textarea { min-height: 80px; resize: vertical; }
.actions { display: flex; flex-direction: column; gap: 8px; justify-content: flex-end; }
.btn {
  border: 1px solid var(--bd);
  background: var(--bg-3);
  color: var(--tx-2);
  border-radius: 4px;
  padding: 7px 12px;
  font-size: 11px;
  cursor: pointer;
}
.btn:hover { border-color: var(--tx-3); color: var(--tx); }
.btn:disabled, .file-btn.disabled { opacity: .5; cursor: not-allowed; }
.file-btn input { display: none; }

.sc-output { flex: 1; overflow: auto; padding: 10px 12px; font-family: var(--font-mono); font-size: 12px; }
.task-banner { margin-bottom: 8px; padding: 6px 8px; border: 1px solid var(--bd); border-radius: 4px; background: rgba(88,166,255,.08); color: var(--blue); }
.line { white-space: pre-wrap; word-break: break-word; margin-bottom: 6px; padding: 6px 8px; border-radius: 4px; border: 1px solid transparent; }
.line.sys { color: var(--tx-3); border-color: var(--bd); background: var(--bg-2); }
.line.info { color: var(--tx); background: rgba(64,196,99,.08); border-color: rgba(64,196,99,.25); }
.line.error { color: var(--red); background: rgba(244,67,54,.08); border-color: rgba(244,67,54,.25); }
</style>
