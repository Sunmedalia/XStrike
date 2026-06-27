<template>
  <div class="sp">
    <div class="sp-header">
      <div class="sp-title">
        <Network :size="14" />
        <span>SOCKS5 Proxy</span>
        <span class="sp-badge" :class="status.running ? 'running' : (status.binary_exists ? 'idle' : 'no-binary')">
          {{ status.running ? 'Running' : (status.binary_exists ? 'Idle' : 'No Binary') }}
        </span>
        <span v-if="status.running && status.pid" class="sp-pid">PID {{ status.pid }}</span>
      </div>
      <button class="icon-btn" @click="refreshStatus" title="Refresh status"><RefreshCw :size="13" /></button>
    </div>

    <!-- Upload area -->
    <div class="sp-section">
      <div class="sp-row">
        <span class="sp-label">ghost-socks binary</span>
        <span class="sp-hint">{{ status.binary_exists ? '✓ uploaded' : 'not uploaded' }}</span>
        <input ref="fileInput" type="file" class="hidden" @change="onFileChange" />
        <button class="sp-btn" @click="fileInput?.click()" :disabled="uploading">
          <Upload :size="13" /> {{ uploading ? 'Uploading…' : 'Upload Binary' }}
        </button>
      </div>
    </div>

    <!-- Config -->
    <div class="sp-section">
      <div class="sp-row mode-row">
        <span class="sp-label">Mode</span>
        <div class="mode-pills">
          <button class="mpill" :class="{ active: mode === 'forward' }" @click="mode = 'forward'">Forward</button>
          <button class="mpill" :class="{ active: mode === 'server' }" @click="mode = 'server'">Reverse Server</button>
        </div>
      </div>

      <div class="sp-row" v-if="mode === 'forward'">
        <span class="sp-label">Listen</span>
        <input v-model="fwd.listen" placeholder="0.0.0.0:1080" class="sp-input" />
        <span class="sp-hint">SOCKS5 client connect address</span>
      </div>

      <template v-if="mode === 'server'">
        <div class="sp-row">
          <span class="sp-label">Control</span>
          <input v-model="srv.control" placeholder="0.0.0.0:9999" class="sp-input" />
          <span class="sp-hint">Agent connects back here</span>
        </div>
        <div class="sp-row">
          <span class="sp-label">SOCKS5</span>
          <input v-model="srv.socks" placeholder="127.0.0.1:1080" class="sp-input" />
          <span class="sp-hint">Local SOCKS5 interface</span>
        </div>
        <div class="sp-note">
          Agent command: <code>ghost-socks agent --server &lt;this-server&gt;:{{ srv.control.split(':')[1] || '9999' }}</code>
        </div>
      </template>
    </div>

    <!-- Actions -->
    <div class="sp-actions">
      <button
        v-if="!status.running"
        class="sp-btn primary"
        :disabled="!status.binary_exists || acting"
        @click="startProxy"
      >
        <Play :size="13" /> Start
      </button>
      <button
        v-else
        class="sp-btn danger"
        :disabled="acting"
        @click="stopProxy"
      >
        <Square :size="13" /> Stop
      </button>
    </div>

    <!-- Running info -->
    <div v-if="status.running" class="sp-running">
      <span class="sp-label">Args</span>
      <code class="sp-code">{{ status.args.join(' ') }}</code>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, reactive, onMounted, onUnmounted } from 'vue'
import { Network, RefreshCw, Upload, Play, Square } from 'lucide-vue-next'
import { useToastStore } from '../stores/toast'
import api from '../services/api'

const toast = useToastStore()

const fileInput = ref<HTMLInputElement>()
const uploading = ref(false)
const acting = ref(false)
const mode = ref<'forward' | 'server'>('forward')

const fwd = reactive({ listen: '0.0.0.0:1080' })
const srv = reactive({ control: '0.0.0.0:9999', socks: '127.0.0.1:1080' })

const status = reactive({
  running: false,
  pid: null as number | null,
  mode: '',
  args: [] as string[],
  binary_exists: false,
})

const refreshStatus = async () => {
  try {
    const res = await api.get('/socks5/status', { silentError: true } as any)
    if (res.data.success) Object.assign(status, res.data.data)
  } catch { /* ignore */ }
}

const onFileChange = async (e: Event) => {
  const file = (e.target as HTMLInputElement).files?.[0]
  if (!file) return
  uploading.value = true
  try {
    const form = new FormData()
    form.append('file', file)
    await api.post('/socks5/upload', form, { headers: { 'Content-Type': 'multipart/form-data' } })
    toast.success('ghost-socks binary uploaded')
    await refreshStatus()
  } catch (err: any) {
    toast.error(err?.response?.data?.error || 'Upload failed')
  } finally {
    uploading.value = false
    if (fileInput.value) fileInput.value.value = ''
  }
}

const startProxy = async () => {
  acting.value = true
  try {
    const payload: any = { mode: mode.value }
    if (mode.value === 'forward') payload.listen = fwd.listen
    else { payload.control = srv.control; payload.socks = srv.socks }
    await api.post('/socks5/start', payload)
    toast.success('SOCKS5 proxy started')
    await refreshStatus()
  } catch (err: any) {
    toast.error(err?.response?.data?.error || 'Start failed')
  } finally {
    acting.value = false
  }
}

const stopProxy = async () => {
  acting.value = true
  try {
    await api.post('/socks5/stop', {})
    toast.success('SOCKS5 proxy stopped')
    await refreshStatus()
  } catch (err: any) {
    toast.error(err?.response?.data?.error || 'Stop failed')
  } finally {
    acting.value = false
  }
}

let timer: any
onMounted(() => {
  refreshStatus()
  timer = setInterval(refreshStatus, 5000)
})
onUnmounted(() => clearInterval(timer))
</script>

<style scoped>
.sp {
  padding: 14px 16px;
  display: flex;
  flex-direction: column;
  gap: 0;
}

.sp-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 14px;
}
.sp-title {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 13px;
  font-weight: 600;
  color: var(--tx);
}
.sp-badge {
  font-size: 10px;
  padding: 2px 7px;
  border-radius: 999px;
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: .4px;
}
.sp-badge.running { background: color-mix(in srgb, var(--green) 18%, transparent); color: var(--green); }
.sp-badge.idle { background: var(--bg-3); color: var(--tx-3); }
.sp-badge.no-binary { background: color-mix(in srgb, var(--amber) 18%, transparent); color: var(--amber); }
.sp-pid { font-size: 11px; color: var(--tx-3); font-family: var(--font-mono, monospace); }

.icon-btn {
  display: inline-flex; align-items: center; justify-content: center;
  width: 28px; height: 28px; border-radius: 6px; border: none;
  background: transparent; color: var(--tx-3); cursor: pointer;
}
.icon-btn:hover { color: var(--tx); background: var(--bg-3); }

/* Sections */
.sp-section {
  border-top: 1px solid var(--bd);
  padding: 10px 0;
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.sp-row {
  display: flex;
  align-items: center;
  gap: 10px;
  flex-wrap: wrap;
}
.mode-row { padding-bottom: 2px; }

.sp-label {
  font-size: 11px;
  color: var(--tx-3);
  width: 56px;
  flex-shrink: 0;
}
.sp-hint { font-size: 11px; color: var(--tx-4); }

.sp-input {
  height: 28px;
  background: var(--bg-3);
  border: 1px solid var(--bd);
  color: var(--tx);
  border-radius: 6px;
  padding: 0 8px;
  font-size: 12px;
  font-family: var(--font-mono, monospace);
  width: 180px;
}
.sp-input:focus { outline: none; border-color: var(--pri); }

.mode-pills { display: flex; gap: 2px; background: var(--bg-3); border-radius: 8px; padding: 3px; }
.mpill {
  border: none; background: transparent; color: var(--tx-3);
  border-radius: 6px; padding: 4px 12px; font-size: 11px; cursor: pointer; transition: all .13s;
}
.mpill:hover { color: var(--tx); }
.mpill.active { background: var(--bg-2); color: var(--tx); box-shadow: 0 1px 3px rgba(0,0,0,.15); }

.sp-note {
  font-size: 11px;
  color: var(--tx-3);
  padding: 6px 8px;
  background: var(--bg-3);
  border-radius: 6px;
  margin-top: 2px;
}
.sp-note code { font-family: var(--font-mono, monospace); font-size: 11px; color: var(--pri); }

.sp-actions {
  border-top: 1px solid var(--bd);
  padding: 10px 0;
  display: flex;
  gap: 8px;
}

.sp-btn {
  display: inline-flex; align-items: center; gap: 5px; height: 30px;
  padding: 0 12px; border-radius: 6px; border: 1px solid var(--bd);
  background: var(--bg-3); color: var(--tx); font-size: 12px; cursor: pointer;
  transition: background .12s, color .12s; white-space: nowrap;
}
.sp-btn:hover:not(:disabled) { background: var(--bg-2); }
.sp-btn.primary:hover:not(:disabled) { background: var(--pri); color: #fff; border-color: var(--pri); }
.sp-btn.danger:hover:not(:disabled) { background: var(--red); border-color: var(--red); color: #fff; }
.sp-btn:disabled { opacity: .45; cursor: not-allowed; }

.sp-running {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 8px 0;
  border-top: 1px solid var(--bd);
}
.sp-code {
  font-size: 11px;
  font-family: var(--font-mono, monospace);
  color: var(--pri);
  background: var(--bg-3);
  padding: 3px 7px;
  border-radius: 4px;
}

.hidden { display: none; }
</style>
