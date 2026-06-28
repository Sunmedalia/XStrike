<template>
  <div class="bof-library">
    <div class="toolbar">
      <button class="btn primary" @click="triggerUpload" :disabled="uploading">
        <Upload :size="14" />
        {{ uploading ? `Uploading ${uploadDone}/${uploadTotal}` : 'Upload BOF' }}
      </button>
      <div v-if="uploading" class="upload-progress">
        <div class="upload-bar" :style="{ width: uploadPct + '%' }"></div>
      </div>
      <input type="file" ref="fileInput" @change="handleUpload" multiple hidden accept=".o,.bin" />
      <div class="stats">
        <span class="chip">Files {{ filteredBofs.length }}</span>
        <span class="chip">Size {{ formatSize(totalSize) }}</span>
      </div>
      <div class="spacer"></div>
      <div class="view-toggle">
        <button
          class="view-btn"
          :class="{ active: viewMode === 'category' }"
          @click="viewMode = 'category'"
          title="Group by category"
        >📂</button>
        <button
          class="view-btn"
          :class="{ active: viewMode === 'plugin' }"
          @click="viewMode = 'plugin'"
          title="Group by plugin"
        >🧩</button>
      </div>
      <select v-model="sortBy" class="sort-select">
        <option value="name">Sort: Name</option>
        <option value="size">Sort: Size</option>
        <option value="uploaded_at">Sort: Time</option>
      </select>
      <div class="search-box">
        <Search :size="14" />
        <input v-model="search" placeholder="Filter BOFs..." />
      </div>
    </div>

    <div class="bof-grid">
      <div v-if="filteredBofs.length === 0" class="empty-state">
        <p>No BOFs found</p>
      </div>

      <!-- Category view mode -->
      <template v-if="viewMode === 'category'">
        <div v-for="category in categories" :key="category.key" class="category-section">
          <template v-if="getBofsByCategory(category.key).length > 0">
            <button class="category-header" @click="toggleCategory(category.key)">
              <span class="category-title">
                <span class="category-icon" v-html="category.icon"></span>
                {{ category.label }}
                <span class="count">{{ getBofsByCategory(category.key).length }}</span>
              </span>
              <span class="collapse-indicator">{{ isCollapsed(category.key) ? '+' : '-' }}</span>
            </button>
          </template>
          <div v-if="!isCollapsed(category.key)" class="bof-list">
            <div v-for="bof in getBofsByCategory(category.key)" :key="bof.name" class="bof-item">
              <div class="bof-info">
                <span class="bof-name">{{ bof.name }}</span>
                <span class="bof-size">
                  {{ formatSize(bof.size) }} · {{ formatTime(bof.uploaded_at) }}
                </span>
                <span v-if="bof.plugin_name" class="bof-plugin-tag">🧩 {{ bof.plugin_name }}</span>
              </div>
              <div class="bof-actions">
                <button class="action-btn" @click="executeBof(bof)" title="Execute">
                  <Play :size="14" />
                </button>
                <button class="action-btn delete" @click="deleteBof(bof.name)" title="Delete">
                  <Trash2 :size="14" />
                </button>
              </div>
            </div>
          </div>
        </div>
      </template>

      <!-- Plugin view mode -->
      <template v-if="viewMode === 'plugin'">
        <div v-for="group in pluginGroups" :key="group.key" class="category-section">
          <template v-if="group.bofs.length > 0">
            <button class="category-header" @click="toggleCategory('plugin_' + group.key)">
              <span class="category-title">
                <span class="category-icon">🧩</span>
                {{ group.label }}
                <span class="count">{{ group.bofs.length }}</span>
              </span>
              <span class="collapse-indicator">{{ isCollapsed('plugin_' + group.key) ? '+' : '-' }}</span>
            </button>
          </template>
          <div v-if="!isCollapsed('plugin_' + group.key)" class="bof-list">
            <div v-for="bof in group.bofs" :key="bof.name" class="bof-item">
              <div class="bof-info">
                <span class="bof-name">{{ bof.name }}</span>
                <span class="bof-size">
                  {{ formatSize(bof.size) }} · {{ formatTime(bof.uploaded_at) }}
                </span>
              </div>
              <div class="bof-actions">
                <button class="action-btn" @click="executeBof(bof)" title="Execute">
                  <Play :size="14" />
                </button>
                <button class="action-btn delete" @click="deleteBof(bof.name)" title="Delete">
                  <Trash2 :size="14" />
                </button>
              </div>
            </div>
          </div>
        </div>
      </template>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { useAppStore } from '../stores/app'
import { useModalStore } from '../stores/modal'
import { useToastStore } from '../stores/toast'
import { usePluginStore } from '../stores/plugin'
import ConfirmModal from './ConfirmModal.vue'
import ExecuteBofModal from './ExecuteBofModal.vue'
import { Upload, Search, Play, Trash2 } from 'lucide-vue-next'
import api from '../services/api'
import { reloadBofCommands } from '../services/commandRegistry'

const appStore = useAppStore()
const modalStore = useModalStore()
const toast = useToastStore()
const pluginStore = usePluginStore()
const search = ref('')
const sortBy = ref<'name' | 'size' | 'uploaded_at'>('name')
const viewMode = ref<'category' | 'plugin'>('category')
const fileInput = ref<HTMLInputElement | null>(null)
const uploading = ref(false)
const uploadTotal = ref(0)
const uploadDone = ref(0)
const uploadPct = computed(() => uploadTotal.value ? Math.round((uploadDone.value / uploadTotal.value) * 100) : 0)
const collapsed = ref<Record<string, boolean>>({})

const categories = [
  { key: 'cmd', label: 'Command Execution', icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/></svg>', patterns: [/^cmd_/i, /^powershell_/i, /terminal/i] },
  { key: 'file', label: 'File Operations', icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 00-2 2v16a2 2 0 002 2h12a2 2 0 002-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>', patterns: [/^file_/i] },
  { key: 'proc', label: 'Process', icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 00.33 1.82l.06.06a2 2 0 010 2.83 2 2 0 01-2.83 0l-.06-.06a1.65 1.65 0 00-1.82-.33"/></svg>', patterns: [/^proc_/i] },
  { key: 'schtask', label: 'Persistence: Scheduled Task', icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="4" width="18" height="18" rx="2"/><line x1="3" y1="10" x2="21" y2="10"/></svg>', patterns: [/^schtask_/i] },
  { key: 'service', label: 'Persistence: Service', icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2v4m0 12v4M4.93 4.93l2.83 2.83m8.48 8.48l2.83 2.83"/></svg>', patterns: [/^svc_/i] },
  { key: 'creds', label: 'Credential Access', icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 11l2-2a4 4 0 015.7 0l2.6 2.6"/><path d="M21 13l-2 2a4 4 0 01-5.7 0l-2.6-2.6"/></svg>', patterns: [/^creds_/i, /hashdump/i, /lsadump/i, /netntlm/i] },
  { key: 'shellcode', label: 'Execution / Injection', icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 12h-4l-3 9L9 3l-3 9H2"/></svg>', patterns: [/^shellcode_/i] },
  { key: 'sysinfo', label: 'System Info', icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="3" width="20" height="14" rx="1"/><path d="M8 21h8m-4-4v4"/></svg>', patterns: [/^sysinfo_/i] },
  { key: 'other', label: 'Other', icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 16V8a2 2 0 00-1-1.73l-7-4a2 2 0 00-2 0l-7 4A2 2 0 003 8v8a2 2 0 001 1.73l7 4a2 2 0 002 0l7-4A2 2 0 0021 16z"/></svg>', patterns: [] }
]

const filteredBofs = computed(() => {
  const key = search.value.trim().toLowerCase()
  let result = !key
    ? [...appStore.bofs]
    : appStore.bofs.filter(b => b.name.toLowerCase().includes(key))

  if (sortBy.value === 'size') {
    result.sort((a, b) => (b.size || 0) - (a.size || 0))
  } else if (sortBy.value === 'uploaded_at') {
    result.sort((a, b) => (b.uploaded_at || 0) - (a.uploaded_at || 0))
  } else {
    result.sort((a, b) => a.name.localeCompare(b.name))
  }
  return result
})

const matchCategory = (name: string, key: string) => {
  const category = categories.find(c => c.key === key)
  if (!category) return false
  if (key === 'other') {
    return !categories
      .filter(c => c.key !== 'other')
      .some(c => c.patterns.some((p: RegExp) => p.test(name)))
  }
  return category.patterns.some((p: RegExp) => p.test(name))
}

const getBofsByCategory = (key: string) => {
  return filteredBofs.value.filter(b => matchCategory(b.name, key))
}

const pluginGroups = computed(() => {
  const groups: { key: string; label: string; bofs: any[] }[] = []
  const pluginMap: Record<string, any[]> = {}
  const standalone: any[] = []

  for (const bof of filteredBofs.value) {
    const pname = bof.plugin_name || ''
    if (pname) {
      if (!pluginMap[pname]) pluginMap[pname] = []
      pluginMap[pname].push(bof)
    } else {
      standalone.push(bof)
    }
  }

  // Plugin groups first
  const pluginNames = Object.keys(pluginMap).sort()
  for (const name of pluginNames) {
    groups.push({ key: name, label: `Plugin: ${name}`, bofs: pluginMap[name] })
  }

  // Standalone (built-in) last
  if (standalone.length > 0) {
    groups.push({ key: '_builtin', label: 'Built-in / Standalone', bofs: standalone })
  }

  return groups
})

const totalSize = computed(() => filteredBofs.value.reduce((acc, b) => acc + (b.size || 0), 0))

const formatTime = (ts: number) => {
  if (!ts) return '-'
  return new Date(ts * 1000).toLocaleString()
}

const formatSize = (bytes: number) => {
  if (!bytes) return '0 B'
  const k = 1024
  const sizes = ['B', 'KB', 'MB', 'GB']
  const i = Math.min(Math.floor(Math.log(bytes) / Math.log(k)), sizes.length - 1)
  return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i]
}

const triggerUpload = () => fileInput.value?.click()

const isCollapsed = (key: string) => Boolean(collapsed.value[key])
const toggleCategory = (key: string) => {
  collapsed.value[key] = !collapsed.value[key]
}

const handleUpload = async (e: Event) => {
  const files = (e.target as HTMLInputElement).files
  if (!files) return

  uploading.value = true
  uploadTotal.value = files.length
  uploadDone.value = 0
  for (const file of Array.from(files)) {
    const formData = new FormData()
    formData.append('file', file)
    try {
      await api.post('/bof/upload', formData)
      uploadDone.value += 1
    } catch (err) {
      toast.error(`Upload failed: ${file.name}`)
    }
  }
  ;(e.target as HTMLInputElement).value = ''
  await appStore.fetchBofs()
  await reloadBofCommands()
  uploading.value = false
  toast.success(`Uploaded ${uploadDone.value}/${uploadTotal.value} files`)
}

const deleteBof = (name: string) => {
  modalStore.open(ConfirmModal, {
    title: 'Delete BOF',
    message: `Permanently delete "${name}"?`,
    confirmText: 'Delete',
    type: 'danger',
    onResolve: async () => {
      await api.delete(`/bof/${name}`)
      await appStore.fetchBofs()
      await reloadBofCommands()
      modalStore.close()
    }
  })
}

const executeBof = (bof: any) => {
  modalStore.open(ExecuteBofModal, {
    bofName: bof.name,
    pluginName: bof.plugin_name || '',
  })
}

onMounted(() => {
  pluginStore.fetchPlugins()
})
</script>

<style scoped>
.bof-library {
  display: flex;
  flex-direction: column;
  height: 100%;
  padding: 16px;
  background: var(--bg);
}
.toolbar {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-bottom: 20px;
  flex-wrap: wrap;
}
.btn {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 6px 12px;
  border-radius: 4px;
  border: 1px solid var(--bd);
  background: var(--bg-3);
  color: var(--tx);
  cursor: pointer;
  font-size: 12px;
}
.btn.primary {
  background: var(--pri);
  color: var(--on-pri, #062235);
  border: none;
  position: relative;
}
.upload-progress {
  height: 2px;
  background: var(--bg-4);
  border-radius: 999px;
  overflow: hidden;
  width: 120px;
  align-self: center;
}
.upload-bar {
  height: 100%;
  background: var(--pri);
  border-radius: 999px;
  transition: width 0.2s ease;
}
.btn:disabled { opacity: 0.6; cursor: not-allowed; }
.stats { display: flex; gap: 8px; }
.chip {
  font-size: 10px;
  color: var(--tx-3);
  border: 1px solid var(--bd);
  border-radius: 999px;
  padding: 3px 8px;
  background: var(--bg-2);
}
.sort-select {
  height: 30px;
  border: 1px solid var(--bd);
  background: var(--bg-3);
  color: var(--tx-2);
  border-radius: 4px;
  padding: 0 8px;
  font-size: 11px;
}
.search-box {
  display: flex;
  align-items: center;
  gap: 8px;
  background: var(--bg-3);
  border: 1px solid var(--bd);
  padding: 4px 10px;
  border-radius: 4px;
  width: 200px;
}
.search-box input {
  background: transparent;
  border: none;
  color: var(--tx);
  font-size: 12px;
  outline: none;
  width: 100%;
}
.spacer { flex: 1; }
.view-toggle {
  display: flex;
  border: 1px solid var(--bd);
  border-radius: 4px;
  overflow: hidden;
}
.view-btn {
  background: var(--bg-3);
  border: none;
  padding: 4px 8px;
  cursor: pointer;
  font-size: 12px;
  color: var(--tx-3);
  transition: background 0.12s;
}
.view-btn:hover {
  background: var(--bg-4);
}
.view-btn.active {
  background: var(--pri);
  color: #fff;
}
.view-btn + .view-btn {
  border-left: 1px solid var(--bd);
}

.bof-grid {
  flex: 1;
  overflow-y: auto;
}
.category-header {
  width: 100%;
  font-size: 11px;
  text-transform: uppercase;
  color: var(--tx-3);
  margin: 16px 0 8px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  border: none;
  background: transparent;
  cursor: pointer;
  padding: 0;
}
.category-title { display: flex; align-items: center; gap: 8px; }
.count {
  font-size: 10px;
  color: var(--tx-4);
  border: 1px solid var(--bd);
  border-radius: 999px;
  padding: 1px 6px;
}
.collapse-indicator { color: var(--tx-4); width: 14px; text-align: center; }
.category-icon {
  width: 14px;
  height: 14px;
  display: flex;
}
.bof-list {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(240px, 1fr));
  gap: 8px;
}
.bof-item {
  background: var(--bg-2);
  border: 1px solid var(--bd);
  border-radius: 4px;
  padding: 10px 12px;
  display: flex;
  justify-content: space-between;
  align-items: center;
}
.bof-info {
  display: flex;
  flex-direction: column;
}
.bof-name {
  font-size: 12px;
  font-weight: 500;
  color: var(--tx);
  font-family: var(--font-mono);
}
.bof-size {
  font-size: 10px;
  color: var(--tx-3);
}
.bof-plugin-tag {
  font-size: 9px;
  color: var(--pri);
  background: rgba(var(--pri-rgb, 99, 102, 241), 0.1);
  border-radius: 999px;
  padding: 1px 6px;
  margin-top: 2px;
  display: inline-block;
}
.bof-actions {
  display: flex;
  gap: 4px;
}
.action-btn {
  background: transparent;
  border: none;
  color: var(--tx-3);
  cursor: pointer;
  padding: 4px;
  border-radius: 3px;
}
.action-btn:hover {
  background: var(--bg-4);
  color: var(--tx);
}
.action-btn.delete:hover {
  color: var(--red);
}
.empty-state {
  display: grid;
  place-items: center;
  color: var(--tx-4);
  height: 100%;
  border: 1px dashed var(--bd);
  border-radius: 8px;
}
</style>
