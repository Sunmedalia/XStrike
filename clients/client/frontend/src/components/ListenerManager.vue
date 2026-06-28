<template>
  <div class="listener-manager">
    <div class="toolbar">
      <button class="btn primary" @click="openCreateModal">
        <Plus :size="14" /> New Listener
      </button>
      <div class="spacer"></div>
      <button class="btn" @click="handleRefresh" :disabled="loading">
        <RefreshCw :size="14" :class="{ 'spinning': loading }" /> Refresh
      </button>
    </div>

    <div class="listener-list">
      <div v-for="l in listeners" :key="l.id" class="listener-card" :class="{ 'active': l.active }">
        <!-- 标志球 -->
        <div class="status-indicator" :class="{ 'online': l.active }">
          <div class="dot"></div>
          <div class="ring" v-if="l.active"></div>
        </div>
        
        <div class="listener-main">
          <div class="listener-header">
            <span class="listener-name">{{ l.name }}</span>
            <!-- 状态小标签 -->
            <span class="status-tag" :class="l.active ? 'active' : 'stopped'">
              {{ l.active ? 'RUNNING' : 'STOPPED' }}
            </span>
            <span class="listener-id">#{{ l.id }}</span>
          </div>

          <div class="tag-group">
            <!-- 协议标签 -->
            <div class="tag protocol" :class="l.protocol">
              <component :is="getProtocolIcon(l.protocol)" :size="10" />
              <span>{{ (l.protocol || '').toUpperCase() }}</span>
            </div>
            
            <!-- 地址标签 -->
            <div class="tag address">
              <Server :size="10" />
              <span>{{ l.bind_ip }}:{{ l.port }}</span>
            </div>
          </div>
        </div>

        <div class="listener-actions">
          <button class="action-btn" 
                  @click="toggleListener(l)" 
                  :disabled="toggling === l.id"
                  :class="l.active ? 'btn-stop' : 'btn-start'">
            <RefreshCw v-if="toggling === l.id" :size="14" class="spinning" />
            <template v-else>
              <Square v-if="l.active" :size="14" />
              <Play v-else :size="14" />
            </template>
          </button>
          
          <button class="action-btn" @click="generateAgent(l)" title="Generate Agent">
            <Download :size="14" />
          </button>

          <button class="action-btn" @click="openEditModal(l)" title="Edit">
            <Pencil :size="14" />
          </button>
          
          <button class="action-btn delete" @click="confirmDelete(l)" title="Delete">
            <Trash2 :size="14" />
          </button>
        </div>
      </div>
      
      <div v-if="listeners.length === 0 && !loading" class="empty-state">
        <div class="empty-icon"><Radio :size="48" /></div>
        <p>No listeners configured.</p>
        <button class="btn primary" @click="openCreateModal">Setup your first listener</button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { useAppStore } from '../stores/app'
import { useModalStore } from '../stores/modal'
import { useToastStore } from '../stores/toast'
import api from '../services/api'
import NewListenerModal from './NewListenerModal.vue'
import EditListenerModal from './EditListenerModal.vue'
import GenerateAgentModal from './GenerateAgentModal.vue'
import ConfirmModal from './ConfirmModal.vue'
import { 
  Plus, RefreshCw, Play, Square, Download, Pencil, Trash2, 
  Radio, Globe, ShieldCheck, Zap, Server 
} from 'lucide-vue-next'

const appStore = useAppStore()
const modalStore = useModalStore()
const toast = useToastStore()
const listeners = computed(() => appStore.listeners)
const loading = ref(false)
const toggling = ref<string | number | null>(null)

const getProtocolIcon = (proto: string) => {
  if (proto === 'https') return ShieldCheck
  if (proto === 'ws') return Zap
  return Globe
}

const handleRefresh = async () => {
  loading.value = true
  await appStore.fetchListeners()
  loading.value = false
}

const openCreateModal = () => {
  modalStore.open(NewListenerModal)
}

const openEditModal = (l: any) => {
  modalStore.open(EditListenerModal, { listener: l })
}

const toggleListener = async (l: any) => {
  const action = l.active ? 'stop' : 'start'
  toggling.value = l.id
  try {
    // start|stop are POST actions on the core (PUT is "update config"). Using
    // PUT here used to fall into the update no-op branch and never actually
    // stop the listener — the card stayed RUNNING.
    const res = await api.post(`/listeners/${l.id}/${action}`)
    if (res.data.success) {
      toast.success(`Listener "${l.name}" ${action}ed`)
      await appStore.fetchListeners()
    }
  } catch (err: any) {
    toast.error(err?.message || `Failed to ${action} listener`)
  } finally {
    toggling.value = null
  }
}

const confirmDelete = (l: any) => {
  modalStore.open(ConfirmModal, {
    title: 'Delete Listener',
    message: `Remove listener "${l.name}"?`,
    confirmText: 'Delete',
    type: 'danger',
    onResolve: async () => {
      await api.delete(`/listeners/${l.id}`)
      await appStore.fetchListeners()
      toast.success('Listener deleted')
    }
  })
}

const generateAgent = (l: any) => {
  modalStore.open(GenerateAgentModal, { listener: l })
}

onMounted(() => {
  handleRefresh()
})
</script>

<style scoped>
.listener-manager { display: flex; flex-direction: column; height: 100%; padding: 16px; background: var(--bg); overflow-y: auto; }
.toolbar { display: flex; align-items: center; gap: 12px; margin-bottom: 24px; }
.btn { display: flex; align-items: center; gap: 6px; padding: 6px 12px; border-radius: 4px; border: 1px solid var(--bd); background: var(--bg-3); color: var(--tx); cursor: pointer; font-size: 12px; }
.btn.primary { background: var(--pri); color: var(--bg); border: none; }
.spacer { flex: 1; }

.listener-list { display: grid; grid-template-columns: repeat(auto-fill, minmax(340px, 1fr)); gap: 16px; }
.listener-card { background: var(--bg-2); border: 1px solid var(--bd); border-radius: 10px; padding: 16px; display: flex; align-items: center; gap: 16px; transition: all 0.2s cubic-bezier(0.4, 0, 0.2, 1); }
.listener-card.active { border-color: var(--pri); background: color-mix(in srgb, var(--pri) 5%, transparent); }

/* 标志球 */
.status-indicator { width: 10px; height: 10px; position: relative; flex-shrink: 0; }
.dot { width: 100%; height: 100%; border-radius: 50%; background: var(--tx-4); transition: all 0.3s; }
.online .dot { background: var(--pri); box-shadow: 0 0 10px var(--pri); }
.ring { position: absolute; inset: -4px; border: 1px solid var(--pri); border-radius: 50%; animation: pulse-ring 2s infinite; }

@keyframes pulse-ring {
  0% { transform: scale(0.7); opacity: 0.8; }
  100% { transform: scale(1.6); opacity: 0; }
}

.listener-main { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 8px; }
.listener-header { display: flex; align-items: center; gap: 8px; }
.listener-name { font-weight: 700; font-size: 14px; color: var(--tx); }

/* 状态标签 */
.status-tag { font-size: 8px; font-weight: 800; padding: 1px 4px; border-radius: 3px; letter-spacing: 0.5px; border: 1px solid transparent; }
.status-tag.active { color: var(--pri); border-color: color-mix(in srgb, var(--pri) 34%, transparent); background: color-mix(in srgb, var(--pri) 12%, transparent); }
.status-tag.stopped { color: var(--tx-3); border-color: var(--bd); background: var(--bg-3); }

.listener-id { font-size: 10px; color: var(--tx-4); font-family: var(--font-mono); }

/* 标签组 */
.tag-group { display: flex; flex-wrap: wrap; gap: 6px; }
.tag { display: flex; align-items: center; gap: 5px; font-size: 10px; font-weight: 600; padding: 2px 8px; border-radius: 12px; background: var(--bg-3); border: 1px solid var(--bd); color: var(--tx-2); }

.tag.protocol.http { color: var(--blue); border-color: rgba(88, 166, 255, 0.3); background: rgba(88, 166, 255, 0.05); }
.tag.protocol.https { color: var(--purple); border-color: rgba(188, 140, 255, 0.3); background: rgba(188, 140, 255, 0.05); }
.tag.protocol.ws { color: var(--amber); border-color: rgba(210, 153, 34, 0.3); background: rgba(210, 153, 34, 0.05); }
.tag.address { font-family: var(--font-mono); color: var(--tx-3); }

.listener-actions { display: flex; gap: 6px; }
.action-btn { background: var(--bg-3); border: 1px solid var(--bd); color: var(--tx-2); padding: 8px; border-radius: 6px; cursor: pointer; display: flex; }
.action-btn:hover:not(:disabled) { border-color: var(--tx-2); color: var(--tx); background: var(--bg-4); }
.btn-start:hover { color: var(--pri); border-color: var(--pri); }
.btn-stop { color: var(--amber); }
.btn-stop:hover { color: var(--red); border-color: var(--red); }
.action-btn.delete:hover { color: var(--red); border-color: var(--red); background: rgba(248, 81, 73, 0.05); }

.empty-state { grid-column: 1 / -1; text-align: center; padding: 60px 20px; color: var(--tx-3); border: 2px dashed var(--bd); border-radius: 12px; }
.spinning { animation: spin 1s linear infinite; }
@keyframes spin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }
</style>
