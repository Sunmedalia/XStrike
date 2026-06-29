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
      <!-- Live autocomplete dropdown: shows registry commands whose name or
           alias starts with the current input. Floats ABOVE the input. Only in
           the global console (agent-workspace terminals run free-text shells). -->
      <ul v-if="suggestions.length" class="autocomplete" ref="autocompleteRef">
        <li
          v-for="(s, i) in suggestions"
          :key="s.name"
          class="ac-item"
          :class="{ active: i === selectedIdx }"
          @mousedown.prevent="acceptSuggestion(s)"
          @mouseenter="selectedIdx = i"
        >
          <span class="ac-name">{{ s.name }}</span>
          <span v-if="s.aliases && s.aliases.length" class="ac-alias">{{ s.aliases.join(', ') }}</span>
          <span class="ac-desc">{{ s.description }}</span>
        </li>
      </ul>
      <span class="prompt prompt-input">{{ currentPrompt }}</span>
      <span v-if="loading" class="terminal-spinner" aria-label="Command running"></span>
      <input
        v-model="input"
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
import { useAppStore } from '../stores/app'
import { useToastStore } from '../stores/toast'
import { encodeRawText } from '../services/bofEncoding'
import { pollTaskResult, queueBofTask } from '../services/tasks'
import { dispatch, getCommandNames, getAllCommands, loadBofCommands, isBofCommandsLoaded, type CommandContext } from '../services/commandRegistry'

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

// ---- Live autocomplete dropdown (global console only) ----
const autocompleteRef = ref<HTMLElement | null>(null)
const selectedIdx = ref(-1)
const AC_LIMIT = 8
// Suggestions: registry commands whose name or an alias starts with the
// current input (case-insensitive). Empty while in agent-workspace mode
// (free-text shell) or when the input doesn't match anything.
const suggestions = computed(() => {
  if (props.targetId) return []   // agent workspace: free-text shell, no registry
  const q = input.value.trim().toLowerCase()
  if (!q) return []
  const all = getAllCommands()
  const out: any[] = []
  for (const c of all) {
    if (c.name.toLowerCase().startsWith(q) || (c.aliases || []).some((a: string) => a.toLowerCase().startsWith(q))) {
      out.push(c)
      if (out.length >= AC_LIMIT) break
    }
  }
  return out
})
const closeAutocomplete = () => { selectedIdx.value = -1 }
const acceptSuggestion = (s: any) => {
  input.value = s.name + ' '
  closeAutocomplete()
  nextTick(() => {
    const el = inputRef.value
    if (el) { el.focus(); el.setSelectionRange(el.value.length, el.value.length) }
  })
}

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
    return `xstrike(${short})>`
  }
  return props.prompt || 'xstrike>'
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
    return [{ text: `XStrike Terminal Session [${targetId}]`, type: 'sys' }]
  }
  return [
    { text: 'XStrike Console v0.1.2', type: 'sys' },
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
    const taskId = await queueBofTask({
      nodeId: props.targetId || '',
      bof,
      args: encodeRawText(cmd),
      source: mode === 'powershell' ? 'terminal:powershell' : mode === 'winapi' ? 'terminal:winapi' : 'terminal:cmd',
      auditInput: cmd
    })
    taskBanner.value = taskId
    await pollTaskOutput(taskId)
  } catch (err) {
    lines.value.push({ text: 'Command execution failed', type: 'error' })
  } finally {
    loading.value = false
    nextTick(() => focusInput())
  }
}

const pollTaskOutput = async (taskId: string) => {
  try {
    const result = await pollTaskResult(taskId, { maxRetry: 120 })
    const text = result.output || result.error || '(empty)'
    lines.value.push({ text, type: result.success ? 'info' : 'error' })
    await appStore.fetchLogs()
  } catch {
    toast.error(`Task timeout: ${taskId}`)
    lines.value.push({ text: 'Task timed out after 120s', type: 'error' })
  }
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
  const acOpen = suggestions.value.length > 0

  // Up/Down: navigate the autocomplete list when it's open, else walk history.
  if (e.key === 'ArrowDown') {
    if (acOpen) {
      e.preventDefault()
      selectedIdx.value = (selectedIdx.value + 1) % suggestions.value.length
      return
    }
    e.preventDefault()
    historyDown()
    return
  }
  if (e.key === 'ArrowUp') {
    if (acOpen) {
      e.preventDefault()
      selectedIdx.value = (selectedIdx.value - 1 + suggestions.value.length) % suggestions.value.length
      return
    }
    e.preventDefault()
    historyUp()
    return
  }

  // Tab: accept the highlighted (or first) suggestion when the dropdown is
  // open; otherwise fall back to the legacy cycle-completion behavior.
  if (e.key === 'Tab') {
    if (acOpen) {
      e.preventDefault()
      const pick = selectedIdx.value >= 0 ? suggestions.value[selectedIdx.value] : suggestions.value[0]
      acceptSuggestion(pick)
      return
    }
    e.preventDefault()
    handleTabComplete()
    return
  }

  // Escape closes the dropdown without running anything.
  if (e.key === 'Escape') {
    closeAutocomplete()
    return
  }

  // Enter always submits the current input (runs the command). Reset
  // completion state. The autocomplete dropdown is purely visual + Tab/arrow.
  if (e.key === 'Enter') {
    tabCompletionCandidates.value = []
    tabCompletionIdx.value = -1
    closeAutocomplete()
    handleCommand()
    return
  }

  // Any other key: reset legacy tab-cycle state; the dropdown recomputes from
  // the new input reactively. Keep the selected index in range.
  if (e.key !== 'Shift' && e.key !== 'Control' && e.key !== 'Alt') {
    tabCompletionCandidates.value = []
    tabCompletionIdx.value = -1
    selectedIdx.value = -1
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
.mode-dot.dot-powershell { background: var(--pri); box-shadow: 0 0 8px color-mix(in srgb, var(--pri) 75%, transparent); }
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
  color: var(--on-pri, #062235);
  background: linear-gradient(135deg, var(--pri), var(--blue));
  box-shadow: 0 2px 10px color-mix(in srgb, var(--pri) 35%, transparent);
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
  position: relative;
}
/* Live autocomplete dropdown — floats above the input. */
.autocomplete {
  position: absolute;
  bottom: 100%;
  left: 0;
  right: 0;
  list-style: none;
  margin: 0;
  padding: 4px;
  max-height: 260px;
  overflow-y: auto;
  background: var(--bg-2);
  border: 1px solid var(--bd);
  border-radius: 6px 6px 0 0;
  box-shadow: 0 -8px 24px rgba(0, 0, 0, 0.45);
  z-index: 20;
  font-size: 12px;
}
.ac-item {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 5px 8px;
  border-radius: 4px;
  cursor: pointer;
  white-space: nowrap;
}
.ac-item.active {
  background: var(--bg-4);
}
.ac-name {
  color: var(--pri);
  font-weight: 600;
  font-family: var(--font-mono);
  min-width: 90px;
}
.ac-alias {
  color: var(--tx-4);
  font-size: 10px;
  font-family: var(--font-mono);
}
.ac-desc {
  color: var(--tx-3);
  margin-left: auto;
  overflow: hidden;
  text-overflow: ellipsis;
  max-width: 50%;
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
  background: linear-gradient(135deg, #8bd8ff, #5ebef2);
  box-shadow: 0 2px 8px rgba(43, 110, 231, 0.25);
}
:global([data-theme='light']) .mode-hint {
  color: #5f738d;
}
</style>
