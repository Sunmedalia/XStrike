<template>
  <div class="terminal" @click="focusInput">
    <div v-if="props.targetId && !props.hideModebar" class="terminal-modebar" @click.stop>
      <div class="mode-meta">
        <span class="mode-dot" :class="`dot-${execMode}`"></span>
        <span class="mode-label">Execution Mode</span>
      </div>
      <div class="mode-switch">
        <button class="mode-btn" :class="{ active: execMode === 'cmd' }" @click="setMode('cmd')">CMD</button>
        <button class="mode-btn" :class="{ active: execMode === 'powershell' }" @click="setMode('powershell')">PowerShell</button>
        <button class="mode-btn" :class="{ active: execMode === 'winapi' }" @click="setMode('winapi')">WinAPI</button>
      </div>
      <span class="mode-hint">{{ modeHint }}</span>
    </div>
    <div v-if="taskBanner" class="task-banner" @click.stop>
      <span class="task-banner-label">Task Created</span>
      <span class="task-banner-id mono">{{ taskBanner }}</span>
    </div>

    <div class="terminal-output" ref="outputRef">
      <div v-for="(line, index) in lines" :key="index" class="line" :class="[line.type, { 'cmd-line': !!line.prompt }]">
        <span v-if="line.prompt" class="prompt">{{ line.prompt }}</span>
        <span class="content">{{ line.text }}</span>
      </div>
    </div>
    <div class="terminal-input-wrapper">
      <span class="prompt prompt-input">{{ currentPrompt }}</span>
      <span v-if="loading" class="terminal-spinner" aria-label="Command running"></span>
      <input
        v-model="input"
        @keydown.enter="handleCommand"
        @keydown.up.prevent="historyUp"
        @keydown.down.prevent="historyDown"
        @keydown="onKeydown"
        ref="inputRef"
        class="terminal-input"
        spellcheck="false"
        autocomplete="off"
        :disabled="loading"
        :placeholder="loading ? 'Waiting for response…' : ''"
      />
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref, onMounted, onUnmounted, watch, nextTick } from 'vue'
import api from '../services/api'
import { useAppStore } from '../stores/app'
import { useToastStore } from '../stores/toast'
import { auditTaskInput } from '../services/taskAudit'
import { dispatch, getCommandNames, loadBofCommands, isBofCommandsLoaded, type CommandContext } from '../services/commandRegistry'

const props = defineProps<{
  targetId?: string;
  prompt?: string;
  hideModebar?: boolean;
  forcedExecMode?: 'cmd' | 'powershell' | 'winapi';
}>()

type ExecMode = 'cmd' | 'powershell' | 'winapi'
type TerminalLine = { text: string; type?: string; prompt?: string }
type TerminalSessionCache = {
  lines: TerminalLine[]
  taskBanner: string
  input: string
  history: string[]
  historyIdx: number
  execMode: ExecMode
  selectedTarget: string | null
}
const terminalCache: Map<string, TerminalSessionCache> = (() => {
  const g = globalThis as any
  if (!g.__ghostTerminalCache) {
    g.__ghostTerminalCache = new Map<string, TerminalSessionCache>()
  }
  return g.__ghostTerminalCache as Map<string, TerminalSessionCache>
})()

const appStore = useAppStore()
const toast = useToastStore()
const lines = ref<TerminalLine[]>([])
const input = ref('')
const loading = ref(false)
const execMode = ref<ExecMode>('cmd')
const taskBanner = ref('')
const history = ref<string[]>([])
const historyIdx = ref(-1)
const outputRef = ref<HTMLElement | null>(null)
const inputRef = ref<HTMLInputElement | null>(null)
const selectedTarget = ref<string | null>(null)
const tabCompletionCandidates = ref<string[]>([])
const tabCompletionIdx = ref(-1)
const inputBeforeTab = ref('')

const effectiveTarget = computed(() => props.targetId || selectedTarget.value || null)

const currentPrompt = computed(() => {
  if (props.targetId) {
    if (execMode.value === 'powershell') return 'PS>'
    if (execMode.value === 'winapi') return 'api>'
    return 'cmd>'
  }
  if (selectedTarget.value) {
    const short = selectedTarget.value.length > 16
      ? selectedTarget.value.substring(0, 16) + '..'
      : selectedTarget.value
    return `ghost(${short})>`
  }
  return props.prompt || 'ghost>'
})

const modeHint = computed(() => {
  if (execMode.value === 'powershell') return 'Scripted ops'
  if (execMode.value === 'winapi') return 'Direct CreateProcessA (no shell)'
  return 'Native shell'
})

// Per-mode BOF lookup. cmd/powershell wrap a shell; winapi runs the exe
// directly via CreateProcessA — no shell features (pipes/redirects/builtins).
const modeBofPattern = (mode: ExecMode): RegExp => {
  if (mode === 'powershell') return /^powershell_exec\b/i
  if (mode === 'winapi') return /^winapi_exec\b/i
  return /^cmd_exec\b/i
}
const modeBofFile = (mode: ExecMode): string => {
  if (mode === 'powershell') return 'powershell_exec.o'
  if (mode === 'winapi') return 'winapi_exec.o'
  return 'cmd_exec.o'
}

const cacheKeyFor = (targetId?: string, mode?: ExecMode) => {
  const base = targetId || '__global__'
  return targetId ? `${base}:${mode || 'cmd'}` : base
}
const defaultLinesFor = (targetId?: string): TerminalLine[] => {
  if (targetId) {
    return [{ text: `Ghost Terminal Session [${targetId}]`, type: 'sys' }]
  }
  return [
    { text: 'Ghost Console v0.1.2', type: 'sys' },
    { text: 'Type "help" for available commands. Use Tab for auto-completion.', type: 'info' }
  ]
}
const saveSession = (targetId?: string, mode?: ExecMode) => {
  const resolvedMode = mode || execMode.value
  terminalCache.set(cacheKeyFor(targetId, resolvedMode), {
    lines: [...lines.value],
    taskBanner: taskBanner.value,
    input: input.value,
    history: [...history.value],
    historyIdx: historyIdx.value,
    execMode: execMode.value,
    selectedTarget: selectedTarget.value
  })
}
const loadSession = (targetId?: string, mode?: ExecMode) => {
  const resolvedMode = mode || props.forcedExecMode || execMode.value
  const cached = terminalCache.get(cacheKeyFor(targetId, resolvedMode))
  if (cached) {
    lines.value = [...cached.lines]
    taskBanner.value = cached.taskBanner || ''
    input.value = cached.input
    history.value = [...cached.history]
    historyIdx.value = cached.historyIdx
    execMode.value = cached.execMode
    selectedTarget.value = cached.selectedTarget || null
    return
  }
  lines.value = defaultLinesFor(targetId)
  taskBanner.value = ''
  input.value = ''
  history.value = []
  historyIdx.value = -1
  execMode.value = resolvedMode || 'cmd'
  selectedTarget.value = null
}

const handleCommand = async () => {
  const cmd = input.value.trim()
  if (!cmd) return

  lines.value.push({ text: cmd, prompt: currentPrompt.value })
  history.value.push(cmd)
  historyIdx.value = history.value.length
  input.value = ''
  // Reset tab completion state
  tabCompletionCandidates.value = []
  tabCompletionIdx.value = -1

  if (cmd === 'clear') {
    lines.value = []
    return
  }

  if (props.targetId) {
    // Agent workspace terminal: pass-through to beacon (existing behavior)
    await executeOnBeacon(cmd)
  } else {
    // Global console: use command registry
    await processConsoleCommand(cmd)
  }
  nextTick(() => focusInput())
}

const executeOnBeacon = async (cmd: string) => {
  loading.value = true
  try {
    const mode = execMode.value
    const bof = appStore.bofs.find(b => modeBofPattern(mode).test(b.name))
    const expected = modeBofFile(mode)
    if (!bof) {
      lines.value.push({ text: `No ${expected} BOF found. Upload ${expected} first.`, type: 'error' })
      return
    }

    // RustStrike's cmd_exec / powershell_exec / winapi_exec all take the command
    // as RAW UTF-8 bytes (no length prefix). Encode the typed line verbatim.
    const args = Array.from(new TextEncoder().encode(cmd))
    await auditTaskInput({
      source: mode === 'powershell' ? 'terminal:powershell' : mode === 'winapi' ? 'terminal:winapi' : 'terminal:cmd',
      nodeId: props.targetId || '',
      input: cmd
    })

    const res = await api.post('/bof/execute', {
      node_id: props.targetId,
      bof_name: bof.name,
      plugin_name: bof.plugin_name || '',
      args
    })
    if (res.data.success) {
      const taskId = res.data.data
      taskBanner.value = taskId
      await pollTaskOutput(taskId)
    }
  } catch (err) {
    lines.value.push({ text: 'Command execution failed', type: 'error' })
  } finally {
    loading.value = false
    nextTick(() => focusInput())
  }
}

const pollTaskOutput = async (taskId: string) => {
  for (let i = 0; i < 120; i++) {
    try {
      const res = await api.get(`/tasks/${taskId}`, {
        silentError: true,
        validateStatus: (s: number) => s === 200 || s === 404
      } as any)
      if (res.status === 404) {
        await new Promise(resolve => setTimeout(resolve, 1000))
        continue
      }
      if (res.data.success && res.data.data) {
        const result = res.data.data
        const text = result.output || result.error || '(empty)'
        lines.value.push({ text, type: result.success ? 'info' : 'error' })
        await appStore.fetchLogs()
        return
      }
    } catch {
      break
    }
    await new Promise(resolve => setTimeout(resolve, 1000))
  }
  toast.error(`Task timeout: ${taskId}`)
  lines.value.push({ text: 'Task timed out after 120s', type: 'error' })
}

const processConsoleCommand = async (cmd: string) => {
  const ctx: CommandContext = {
    targetId: effectiveTarget.value,
    appStore,
    toast,
    pushLine: (text: string, type?: string) => {
      lines.value.push({ text, type })
    },
    setLoading: (v: boolean) => { loading.value = v },
    setTaskBanner: (id: string) => { taskBanner.value = id },
    setTarget: (id: string | null) => { selectedTarget.value = id },
    pollTask: (taskId: string) => pollTaskOutput(taskId)
  }
  const result = await dispatch(cmd, ctx)
  if (result === 'clear') {
    lines.value = []
  }
}

const handleTabComplete = () => {
  const current = input.value.trim()
  if (!current) return

  if (tabCompletionCandidates.value.length === 0) {
    // Initialize candidates
    inputBeforeTab.value = current
    const names = getCommandNames()
    tabCompletionCandidates.value = names.filter(n => n.startsWith(current.toLowerCase()))
    if (tabCompletionCandidates.value.length === 0) return
    tabCompletionIdx.value = 0
  } else {
    // Cycle through candidates
    tabCompletionIdx.value = (tabCompletionIdx.value + 1) % tabCompletionCandidates.value.length
  }
  input.value = tabCompletionCandidates.value[tabCompletionIdx.value]
}

const onKeydown = (e: KeyboardEvent) => {
  if (e.key === 'Tab') {
    e.preventDefault()
    handleTabComplete()
    return
  }
  // Reset tab completion on any other key
  if (e.key !== 'Tab') {
    tabCompletionCandidates.value = []
    tabCompletionIdx.value = -1
  }
}

const setMode = (mode: ExecMode) => {
  if (execMode.value === mode) return
  execMode.value = mode
  focusInput()
}

const onTaskInput = (evt: Event) => {
  if (props.targetId) return
  const detail = (evt as CustomEvent<any>).detail
  if (!detail?.line) return
  lines.value.push({ text: detail.line, type: 'sys' })
}

const focusInput = () => inputRef.value?.focus()
const historyUp = () => {
  if (!history.value.length) return
  historyIdx.value = Math.max(0, historyIdx.value - 1)
  input.value = history.value[historyIdx.value] || ''
}
const historyDown = () => {
  if (!history.value.length) return
  historyIdx.value = Math.min(history.value.length, historyIdx.value + 1)
  input.value = historyIdx.value >= history.value.length ? '' : history.value[historyIdx.value]
}

watch(lines, () => {
  nextTick(() => {
    if (outputRef.value) outputRef.value.scrollTop = outputRef.value.scrollHeight
  })
}, { deep: true })
watch([lines, taskBanner, input, history, historyIdx, execMode, selectedTarget], () => {
  saveSession(props.targetId, props.forcedExecMode || execMode.value)
}, { deep: true })
watch(() => props.targetId, (next, prev) => {
  saveSession(prev, props.forcedExecMode || execMode.value)
  loadSession(next, props.forcedExecMode || execMode.value)
})
watch(() => props.forcedExecMode, (mode, prev) => {
  if (!mode) return
  if (prev) saveSession(props.targetId, prev)
  execMode.value = mode
  loadSession(props.targetId, mode)
}, { immediate: true })

onMounted(async () => {
  loadSession(props.targetId, props.forcedExecMode || execMode.value)
  focusInput()
  if (!props.targetId) {
    window.addEventListener('ghost:task-input', onTaskInput as EventListener)
    // Load dynamic BOF commands on first global console mount
    if (!isBofCommandsLoaded()) {
      await loadBofCommands()
    }
  }
})
onUnmounted(() => {
  saveSession(props.targetId, props.forcedExecMode || execMode.value)
  if (!props.targetId) {
    window.removeEventListener('ghost:task-input', onTaskInput as EventListener)
  }
})
</script>

<style scoped>
.terminal { display: flex; flex-direction: column; height: 100%; background: var(--bg); font-family: var(--font-mono); font-size: 13px; padding: 8px; cursor: text; }
.terminal-modebar {
  display: flex;
  align-items: center;
  gap: 10px;
  height: 34px;
  margin-bottom: 8px;
  padding: 0 10px;
  border: 1px solid var(--bd);
  border-radius: 6px;
  background: linear-gradient(180deg, var(--bg-2), var(--bg-3));
  box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.06);
}
.mode-meta { display: flex; align-items: center; gap: 8px; min-width: 120px; }
.mode-dot { width: 7px; height: 7px; border-radius: 999px; }
.mode-dot.dot-cmd { background: var(--blue); box-shadow: 0 0 8px rgba(88, 166, 255, 0.75); }
.mode-dot.dot-powershell { background: var(--pri); box-shadow: 0 0 8px rgba(64, 196, 99, 0.75); }
.mode-dot.dot-winapi { background: var(--amber); box-shadow: 0 0 8px rgba(210, 153, 34, 0.75); }
.mode-label { font-size: 10px; color: var(--tx-3); text-transform: uppercase; letter-spacing: 0.7px; }
.mode-switch {
  display: inline-flex;
  align-items: center;
  background: var(--bg-3);
  border: 1px solid var(--bd);
  border-radius: 999px;
  padding: 2px;
}
.mode-btn {
  border: none;
  background: transparent;
  color: var(--tx-2);
  border-radius: 999px;
  font-size: 10px;
  font-weight: 700;
  letter-spacing: 0.3px;
  padding: 4px 10px;
  cursor: pointer;
}
.mode-btn.active {
  color: var(--bg);
  background: linear-gradient(135deg, var(--pri), #2fbf74);
  box-shadow: 0 2px 10px rgba(64, 196, 99, 0.35);
}
.mode-hint {
  margin-left: auto;
  font-size: 10px;
  color: var(--tx-4);
  letter-spacing: 0.2px;
}
.terminal-output { flex: 1; overflow-y: auto; margin-bottom: 8px; }
.task-banner {
  display: flex;
  align-items: center;
  gap: 8px;
  min-height: 26px;
  margin: 0 0 8px;
  padding: 4px 10px;
  border: 1px solid var(--bd);
  border-radius: 6px;
  background: rgba(88, 166, 255, 0.08);
}
.task-banner-label {
  color: var(--blue);
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: 0.5px;
}
.task-banner-id {
  color: var(--tx);
  font-size: 11px;
}
.line { margin-bottom: 2px; white-space: pre-wrap; word-break: break-all; line-height: 1.4; }
.line.sys { color: var(--tx-3); border-bottom: 1px solid var(--tx-4); margin-bottom: 8px; padding-bottom: 4px; }
.line.info { color: var(--pri); }
.line.error { color: var(--red); }
.line.cmd-line {
  color: var(--tx);
  margin: 6px 0;
  padding: 4px 8px;
  border-left: 2px solid rgba(88, 166, 255, 0.75);
  background: rgba(88, 166, 255, 0.06);
  border-radius: 4px;
}
.prompt { color: var(--purple); margin-right: 8px; font-weight: 600; }
.mono { font-family: var(--font-mono); }
.prompt-input {
  margin-right: 10px;
  color: var(--blue);
  text-transform: lowercase;
}
.terminal-input-wrapper {
  display: flex;
  align-items: center;
  min-height: 34px;
  padding: 6px 10px;
  border-top: 1px solid var(--bd);
  background: linear-gradient(180deg, rgba(255, 255, 255, 0.02), rgba(0, 0, 0, 0.08));
}
.terminal-input {
  flex: 1;
  background: transparent;
  border: none;
  color: var(--tx);
  outline: none;
  font-family: inherit;
  font-size: inherit;
}
.terminal-input:disabled { opacity: 0.45; cursor: not-allowed; }
.terminal-spinner {
  display: inline-block;
  width: 10px;
  height: 10px;
  margin-right: 8px;
  border: 1.5px solid var(--tx-4);
  border-top-color: var(--pri);
  border-radius: 50%;
  animation: tspin 0.7s linear infinite;
  flex-shrink: 0;
}
@keyframes tspin { to { transform: rotate(360deg); } }

:global([data-theme='light']) .terminal-modebar {
  background: linear-gradient(180deg, #ffffff, #edf2f8);
  box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.9);
}
:global([data-theme='light']) .mode-switch {
  background: #f5f8fc;
  border-color: #cbd8e8;
}
:global([data-theme='light']) .mode-btn {
  color: #4a5e76;
}
:global([data-theme='light']) .mode-btn.active {
  color: #ffffff;
  background: linear-gradient(135deg, #0f9d58, #2b6ee7);
  box-shadow: 0 2px 8px rgba(43, 110, 231, 0.25);
}
:global([data-theme='light']) .mode-hint {
  color: #5f738d;
}
</style>
