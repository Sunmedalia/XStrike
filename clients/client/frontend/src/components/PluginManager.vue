<template>
  <div class="plugin-manager">
    <!-- Toolbar -->
    <div class="pm-toolbar">
      <button class="pm-upload-btn" @click="triggerUpload">
        <span>📦</span> Install Plugin
      </button>
      <input
        ref="fileInput"
        type="file"
        accept=".zip,.ghost"
        style="display:none"
        @change="handleUpload"
      />
      <button class="pm-refresh-btn" @click="refresh" :disabled="pluginStore.loading">
        ↻ Refresh
      </button>
      <span class="pm-count">{{ pluginStore.plugins.length }} plugin(s)</span>
    </div>

    <!-- Upload progress -->
    <div v-if="uploading" class="pm-progress">
      <div class="pm-progress-bar"><div class="pm-progress-fill" /></div>
      <span>Installing plugin...</span>
    </div>

    <!-- Plugin cards -->
    <div v-if="pluginStore.plugins.length > 0" class="pm-grid">
      <div
        v-for="p in pluginStore.plugins"
        :key="p.id"
        class="pm-card"
        :class="{ disabled: !p.enabled }"
      >
        <div class="pm-card-header">
          <div class="pm-card-title">
            <span class="pm-card-name">{{ p.name }}</span>
            <span class="pm-card-ver">v{{ p.version }}</span>
          </div>
          <div class="pm-card-toggle">
            <label class="pm-switch">
              <input type="checkbox" :checked="p.enabled" @change="togglePlugin(p.id)" />
              <span class="pm-slider"></span>
            </label>
          </div>
        </div>
        <div class="pm-card-desc">{{ p.description || 'No description' }}</div>
        <div class="pm-card-meta">
          <span v-if="p.author" class="pm-chip">👤 {{ p.author }}</span>
          <span v-if="p.command_count" class="pm-chip">⌨️ {{ p.command_count }} cmd</span>
          <span v-if="p.menu_count" class="pm-chip">📋 {{ p.menu_count }} menu</span>
          <span v-if="p.panel_count" class="pm-chip">📊 {{ p.panel_count }} panel</span>
          <span v-if="p.bof_count" class="pm-chip">🔧 {{ p.bof_count }} bof</span>
        </div>
        <!-- BOF files list -->
        <div v-if="p.bof_files && p.bof_files.length > 0" class="pm-bof-files">
          <div class="pm-bof-files-header" @click="toggleBofList(p.id)">
            <span>📁 BOF Files</span>
            <span class="pm-bof-toggle">{{ expandedBofs[p.id] ? '−' : '+' }}</span>
          </div>
          <div v-if="expandedBofs[p.id]" class="pm-bof-list">
            <div v-for="bof in p.bof_files" :key="bof" class="pm-bof-item">
              <span class="pm-bof-icon">📄</span>
              <span class="pm-bof-name">{{ bof }}</span>
            </div>
          </div>
        </div>
        <div class="pm-card-footer">
          <span class="pm-card-time">Installed: {{ formatTime(p.installed_at) }}</span>
          <button class="pm-delete-btn" @click="uninstallPlugin(p.id, p.name)" title="Uninstall">
            🗑
          </button>
        </div>
      </div>
    </div>

    <!-- Empty state -->
    <div v-else class="pm-empty">
      <div style="font-size:36px;opacity:0.4">🧩</div>
      <div class="pm-empty-title">No Plugins Installed</div>
      <div class="pm-empty-desc">
        Upload a <code>.zip</code> or <code>.ghost</code> plugin package to get started.
        <br />
        Plugins can add menus, panels, commands, and toolbar buttons to the UI automatically.
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { usePluginStore } from '../stores/plugin'
import { useToastStore } from '../stores/toast'
import { useModalStore } from '../stores/modal'
import ConfirmModal from './ConfirmModal.vue'
import { reloadBofCommands } from '../services/commandRegistry'

const pluginStore = usePluginStore()
const toast = useToastStore()
const modalStore = useModalStore()

const fileInput = ref<HTMLInputElement>()
const uploading = ref(false)
const expandedBofs = ref<Record<string, boolean>>({})

const toggleBofList = (pluginId: string) => {
  expandedBofs.value[pluginId] = !expandedBofs.value[pluginId]
}

const triggerUpload = () => {
  fileInput.value?.click()
}

const handleUpload = async (e: Event) => {
  const target = e.target as HTMLInputElement
  if (!target.files?.length) return

  uploading.value = true
  let successCount = 0

  for (const file of Array.from(target.files)) {
    const ok = await pluginStore.installPlugin(file)
    if (ok) {
      successCount++
      toast.success(`Plugin installed: ${file.name}`)
    }
  }

  if (successCount > 0) {
    // Reload BOF commands so new plugin commands are available
    await reloadBofCommands()
  }

  uploading.value = false
  target.value = ''
}

const togglePlugin = async (id: string) => {
  const ok = await pluginStore.togglePlugin(id)
  if (ok) {
    await reloadBofCommands()
    toast.success('Plugin toggled')
  }
}

const uninstallPlugin = (id: string, name: string) => {
  modalStore.open(ConfirmModal, {
    title: 'Uninstall Plugin',
    message: `Are you sure you want to uninstall "${name}"? This will remove all associated BOF files and commands.`,
    confirmText: 'Uninstall',
    type: 'danger',
    onResolve: async () => {
      const ok = await pluginStore.uninstallPlugin(id)
      if (ok) {
        toast.success(`Plugin "${name}" uninstalled`)
        await reloadBofCommands()
      }
    },
  })
}

const refresh = () => {
  pluginStore.refreshAll()
}

const formatTime = (ts: number) => {
  if (!ts) return '-'
  return new Date(ts * 1000).toLocaleString([], {
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    hour12: false,
  })
}

onMounted(() => {
  pluginStore.refreshAll()
})
</script>

<style scoped>
.plugin-manager {
  padding: 16px;
  height: 100%;
  overflow-y: auto;
  display: flex;
  flex-direction: column;
  gap: 12px;
}
.pm-toolbar {
  display: flex;
  align-items: center;
  gap: 10px;
  flex-shrink: 0;
}
.pm-upload-btn {
  background: var(--pri);
  color: #fff;
  border: none;
  border-radius: 5px;
  padding: 7px 16px;
  font-size: 12px;
  font-weight: 600;
  cursor: pointer;
  display: flex;
  align-items: center;
  gap: 6px;
  transition: background 0.15s;
}
.pm-upload-btn:hover {
  background: var(--pri-h);
}
.pm-refresh-btn {
  background: transparent;
  color: var(--tx-2);
  border: 1px solid var(--bd);
  border-radius: 5px;
  padding: 7px 12px;
  font-size: 12px;
  cursor: pointer;
  transition: background 0.12s;
}
.pm-refresh-btn:hover {
  background: var(--bg-3);
}
.pm-count {
  margin-left: auto;
  font-size: 11px;
  color: var(--tx-3);
}
.pm-progress {
  display: flex;
  align-items: center;
  gap: 10px;
  font-size: 11px;
  color: var(--tx-2);
}
.pm-progress-bar {
  flex: 1;
  height: 4px;
  background: var(--bg-3);
  border-radius: 2px;
  overflow: hidden;
}
.pm-progress-fill {
  width: 60%;
  height: 100%;
  background: var(--pri);
  border-radius: 2px;
  animation: pm-progress-anim 1.5s ease-in-out infinite;
}
@keyframes pm-progress-anim {
  0% { width: 10%; }
  50% { width: 80%; }
  100% { width: 10%; }
}
.pm-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(320px, 1fr));
  gap: 12px;
}
.pm-card {
  background: var(--bg-2);
  border: 1px solid var(--bd);
  border-radius: 8px;
  padding: 14px 16px;
  display: flex;
  flex-direction: column;
  gap: 8px;
  transition: border-color 0.15s, opacity 0.15s;
}
.pm-card:hover {
  border-color: var(--bd-2);
}
.pm-card.disabled {
  opacity: 0.5;
}
.pm-card-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
}
.pm-card-title {
  display: flex;
  align-items: baseline;
  gap: 8px;
}
.pm-card-name {
  font-weight: 700;
  font-size: 14px;
  color: var(--tx);
}
.pm-card-ver {
  font-size: 11px;
  color: var(--tx-3);
  font-family: var(--font-mono);
}
.pm-card-desc {
  font-size: 12px;
  color: var(--tx-2);
  line-height: 1.5;
}
.pm-card-meta {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
}
.pm-chip {
  font-size: 10px;
  color: var(--tx-3);
  background: var(--bg-3);
  border-radius: 999px;
  padding: 3px 8px;
}
.pm-card-footer {
  display: flex;
  justify-content: space-between;
  align-items: center;
  border-top: 1px solid var(--bd);
  padding-top: 8px;
  margin-top: 4px;
}
.pm-card-time {
  font-size: 10px;
  color: var(--tx-4);
}
.pm-delete-btn {
  background: transparent;
  border: none;
  font-size: 14px;
  cursor: pointer;
  opacity: 0.5;
  transition: opacity 0.15s;
  padding: 2px 6px;
  border-radius: 4px;
}
.pm-delete-btn:hover {
  opacity: 1;
  background: rgba(197, 116, 116, 0.15);
}
/* BOF files list */
.pm-bof-files {
  border-top: 1px solid var(--bd);
  padding-top: 6px;
  margin-top: 2px;
}
.pm-bof-files-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-size: 11px;
  color: var(--tx-3);
  cursor: pointer;
  padding: 2px 0;
}
.pm-bof-files-header:hover {
  color: var(--tx-2);
}
.pm-bof-toggle {
  font-size: 12px;
  width: 16px;
  text-align: center;
}
.pm-bof-list {
  display: flex;
  flex-direction: column;
  gap: 2px;
  margin-top: 4px;
}
.pm-bof-item {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 11px;
  color: var(--tx-2);
  padding: 2px 4px;
  border-radius: 3px;
  background: var(--bg-3);
}
.pm-bof-icon {
  font-size: 10px;
}
.pm-bof-name {
  font-family: var(--font-mono);
  font-size: 10px;
}
/* Toggle switch */
.pm-switch {
  position: relative;
  display: inline-block;
  width: 36px;
  height: 20px;
}
.pm-switch input {
  opacity: 0;
  width: 0;
  height: 0;
}
.pm-slider {
  position: absolute;
  cursor: pointer;
  top: 0; left: 0; right: 0; bottom: 0;
  background: var(--bg-4);
  border-radius: 999px;
  transition: background 0.2s;
}
.pm-slider::before {
  content: '';
  position: absolute;
  height: 14px;
  width: 14px;
  left: 3px;
  bottom: 3px;
  background: var(--tx-3);
  border-radius: 50%;
  transition: transform 0.2s, background 0.2s;
}
.pm-switch input:checked + .pm-slider {
  background: var(--pri);
}
.pm-switch input:checked + .pm-slider::before {
  transform: translateX(16px);
  background: #fff;
}
.pm-empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 10px;
  padding: 60px 20px;
  text-align: center;
}
.pm-empty-title {
  font-size: 16px;
  font-weight: 600;
  color: var(--tx-2);
}
.pm-empty-desc {
  font-size: 12px;
  color: var(--tx-3);
  line-height: 1.8;
}
.pm-empty-desc code {
  background: var(--bg-3);
  padding: 1px 5px;
  border-radius: 3px;
  font-family: var(--font-mono);
  font-size: 11px;
}
</style>
