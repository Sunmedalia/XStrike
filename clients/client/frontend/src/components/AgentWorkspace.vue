<template>
  <div class="agent-workspace">
    <div class="workspace-nav" @click.stop>
      <div class="workspace-meta">
        <span class="workspace-dot" :class="`dot-${activeView}`"></span>
        <span class="workspace-title">Agent Workspace</span>
        <span class="workspace-id mono">{{ targetId }}</span>
      </div>
      <div class="workspace-switch">
        <button class="workspace-btn" :class="{ active: activeView === 'cmd' }" @click="setView('cmd')">CMD</button>
        <button class="workspace-btn" :class="{ active: activeView === 'powershell' }" @click="setView('powershell')">PowerShell</button>
        <button class="workspace-btn" :class="{ active: activeView === 'winapi' }" @click="setView('winapi')">WinAPI</button>
        <button class="workspace-btn" :class="{ active: activeView === 'files' }" @click="setView('files')">Files</button>
        <button class="workspace-btn" :class="{ active: activeView === 'processes' }" @click="setView('processes')">Processes</button>
        <button class="workspace-btn" :class="{ active: activeView === 'net' }" @click="setView('net')">Net</button>
        <button class="workspace-btn" :class="{ active: activeView === 'pivot' }" @click="setView('pivot')">Pivot</button>
        <button class="workspace-btn" :class="{ active: activeView === 'persistence' }" @click="setView('persistence')">Persistence</button>
        <button class="workspace-btn" :class="{ active: activeView === 'shellcode' }" @click="setView('shellcode')">Shellcode</button>
        <button class="workspace-btn" :class="{ active: activeView === 'screenshots' }" @click="setView('screenshots')">Screenshots</button>
        <button class="workspace-btn" :class="{ active: activeView === 'vnc' }" @click="setView('vnc')">VNC</button>
        <button class="workspace-btn" :class="{ active: activeView === 'socks5' }" @click="setView('socks5')">SOCKS5</button>
        <!-- Plugin panels -->
        <button
          v-for="panel in pluginPanels"
          :key="panel.id"
          class="workspace-btn workspace-btn-plugin"
          :class="{ active: activeView === `plugin:${panel.id}` }"
          @click="setView(`plugin:${panel.id}`)"
        >🧩 {{ panel.title }}</button>
      </div>
      <span class="workspace-hint">{{ viewHint }}</span>
    </div>

    <div class="workspace-body">
      <Terminal
        v-if="activeView === 'cmd' || activeView === 'powershell' || activeView === 'winapi'"
        :targetId="targetId"
        :hideModebar="true"
        :forcedExecMode="activeView"
      />
      <FileBrowser
        v-else-if="activeView === 'files'"
        :targetId="targetId"
      />
      <ProcessList
        v-else-if="activeView === 'processes'"
        :targetId="targetId"
      />
      <NetConnections
        v-else-if="activeView === 'net'"
        :targetId="targetId"
      />
      <PivotPanel
        v-else-if="activeView === 'pivot'"
        :targetId="targetId"
      />
      <PersistenceManager
        v-else-if="activeView === 'persistence'"
        :targetId="targetId"
        :initialMode="persistenceMode"
      />
      <ShellcodeExecutor
        v-else-if="activeView === 'shellcode'"
        :targetId="targetId"
      />
      <ScreenshotViewer
        v-else-if="activeView === 'screenshots'"
        :agent="{ id: targetId }"
      />
      <VncViewer
        v-else-if="activeView === 'vnc'"
        :targetId="targetId"
      />
      <Socks5Panel
        v-else-if="activeView === 'socks5'"
      />
      <!-- Plugin panel rendering -->
      <PluginRenderer
        v-else-if="activePluginPanel"
        :layout="activePluginPanel.layout"
        :targetId="targetId"
      />
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import Terminal from './Terminal.vue'
import FileBrowser from './FileBrowser.vue'
import ProcessList from './ProcessList.vue'
import NetConnections from './NetConnections.vue'
import PivotPanel from './PivotPanel.vue'
import PersistenceManager from './PersistenceManager.vue'
import ShellcodeExecutor from './ShellcodeExecutor.vue'
import ScreenshotViewer from './ScreenshotViewer.vue'
import VncViewer from './VncViewer.vue'
import Socks5Panel from './Socks5Panel.vue'
import PluginRenderer from './PluginRenderer.vue'
import { usePluginStore } from '../stores/plugin'

const pluginStore = usePluginStore()

const props = defineProps<{
  targetId: string
  initialView?: string
  persistenceMode?: 'schtask' | 'service' | 'critical' | 'user'
}>()

type AgentView = string
const workspaceCache: Map<string, AgentView> = (() => {
  const g = globalThis as any
  if (!g.__ghostAgentWorkspaceCache) {
    g.__ghostAgentWorkspaceCache = new Map<string, AgentView>()
  }
  return g.__ghostAgentWorkspaceCache as Map<string, AgentView>
})()

const activeView = ref<AgentView>(workspaceCache.get(props.targetId) || props.initialView || 'cmd')

const setView = (view: AgentView) => {
  activeView.value = view
}

/** Plugin panels for the agent workspace */
const pluginPanels = computed(() => pluginStore.panelsForLocation('agent_workspace'))

/** Currently active plugin panel (if viewing one) */
const activePluginPanel = computed(() => {
  if (!activeView.value.startsWith('plugin:')) return null
  const panelId = activeView.value.replace('plugin:', '')
  return pluginPanels.value.find(p => p.id === panelId) || null
})

const viewHint = computed(() => {
  switch (activeView.value) {
    case 'cmd': return 'Native shell'
    case 'powershell': return 'Scripted ops'
    case 'winapi': return 'Direct CreateProcessA (no shell)'
    case 'files': return 'Remote file browser'
    case 'processes': return 'Process inspection'
    case 'net': return 'Network connections'
    case 'pivot': return 'Pivot / relay listeners'
    case 'persistence': return 'Persistence operations'
    case 'shellcode':    return 'Shellcode execution'
    case 'screenshots':  return 'Desktop screenshots'
    case 'vnc':          return 'Live remote desktop'
    case 'socks5':       return 'SOCKS5 proxy tunnel'
    default:
      if (activePluginPanel.value) return `Plugin: ${activePluginPanel.value.title}`
      return ''
  }
})

watch(activeView, (view) => {
  workspaceCache.set(props.targetId, view)
})

watch(() => props.targetId, (next, prev) => {
  if (prev) workspaceCache.set(prev, activeView.value)
  activeView.value = workspaceCache.get(next) || props.initialView || 'cmd'
})

watch(() => props.initialView, (next) => {
  if (next) activeView.value = next
})
</script>

<style scoped>
.agent-workspace {
  display: flex;
  flex-direction: column;
  height: 100%;
  min-height: 0;
  background: var(--bg);
}
.workspace-nav {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 8px 12px;
  border-bottom: 1px solid var(--bd);
  background: linear-gradient(180deg, var(--bg-2), var(--bg));
  flex-shrink: 0;
}
.workspace-meta {
  display: flex;
  align-items: center;
  gap: 8px;
  min-width: 0;
}
.workspace-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: var(--pri);
  box-shadow: 0 0 8px color-mix(in srgb, var(--pri) 60%, transparent);
}
.workspace-dot.dot-cmd { background: var(--blue); box-shadow: 0 0 8px rgba(88, 166, 255, 0.75); }
.workspace-dot.dot-powershell { background: var(--pri); box-shadow: 0 0 8px color-mix(in srgb, var(--pri) 75%, transparent); }
.workspace-dot.dot-winapi { background: var(--amber); box-shadow: 0 0 8px rgba(210, 153, 34, 0.75); }
.workspace-dot.dot-files,
.workspace-dot.dot-processes,
.workspace-dot.dot-net,
.workspace-dot.dot-pivot,
.workspace-dot.dot-persistence,
.workspace-dot.dot-shellcode { background: var(--amber); box-shadow: 0 0 8px rgba(210, 153, 34, 0.6); }
.workspace-dot.dot-vnc { background: var(--blue); box-shadow: 0 0 8px rgba(88, 166, 255, 0.8); animation: pulse-dot 2s ease-in-out infinite; }
.workspace-dot.dot-socks5 { background: var(--green, #6aad7e); box-shadow: 0 0 8px rgba(106, 173, 126, 0.8); }
@keyframes pulse-dot {
  0%, 100% { box-shadow: 0 0 8px rgba(88, 166, 255, 0.8); }
  50%       { box-shadow: 0 0 14px rgba(88, 166, 255, 1.0); }
}
.workspace-title {
  font-size: 11px;
  font-weight: 700;
  color: var(--tx);
  text-transform: uppercase;
  letter-spacing: 0.08em;
}
.workspace-id {
  font-size: 11px;
  color: var(--tx-3);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}
.workspace-switch {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 4px;
  border: 1px solid var(--bd);
  border-radius: 999px;
  background: color-mix(in srgb, var(--bg-3) 84%, transparent);
  flex-wrap: wrap;
}
.workspace-btn {
  border: 1px solid transparent;
  background: transparent;
  color: var(--tx-3);
  border-radius: 999px;
  padding: 5px 12px;
  font-size: 11px;
  cursor: pointer;
  transition: background-color .14s ease, color .14s ease, border-color .14s ease;
}
.workspace-btn:hover {
  color: var(--tx);
  background: color-mix(in srgb, var(--bg-3) 92%, transparent);
}
.workspace-btn.active {
  color: var(--tx);
  border-color: color-mix(in srgb, var(--pri) 45%, var(--bd));
  background: color-mix(in srgb, var(--pri) 16%, var(--bg-3));
  box-shadow: inset 0 0 0 1px color-mix(in srgb, var(--pri) 10%, transparent);
}
.workspace-hint {
  margin-left: auto;
  font-size: 11px;
  color: var(--tx-4);
}
.workspace-body {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
}
.workspace-btn-plugin {
  border: 1px dashed color-mix(in srgb, var(--pri) 30%, var(--bd));
}
.workspace-btn-plugin.active {
  border-style: solid;
}
.mono { font-family: var(--font-mono); }
@media (max-width: 1100px) {
  .workspace-nav { flex-wrap: wrap; align-items: flex-start; }
  .workspace-hint { margin-left: 0; width: 100%; }
}
</style>
