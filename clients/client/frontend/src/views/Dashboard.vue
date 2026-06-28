<template>
  <div
    class="dusk"
    :class="{
      light: isLight,
      'platform-windows': activePlatform === 'windows',
      'platform-linux': activePlatform === 'linux',
      'platform-switching': platformSwitching,
    }"
    :style="rippleStyle"
  >
    <!-- ═══ Body ═══ -->
    <main class="d-body">
      <!-- Top: Beacon Table -->
      <section
        class="d-top"
        :class="{ 'd-top-full': topFullscreen }"
        v-show="!btmFullscreen"
        :style="topFullscreen ? undefined : { height: topH + 'px' }"
      >
        <div class="d-bar">
          <div class="d-search" :class="{ focus: searchFocus }">
            <Search :size="13" />
            <input ref="searchEl" v-model="filter" :placeholder="searchPlaceholder"
              @focus="searchFocus = true" @blur="searchFocus = false" />
            <span v-if="filter" class="d-x" @click="filter = ''">×</span>
          </div>
          <div class="d-stats d-bar-stats">
            <span class="d-chip"><b>{{ aliveCount }}</b> agents</span>
            <span class="d-chip"><b>{{ bofCount }}</b> bofs</span>
            <span class="d-chip"><b>{{ taskCount }}</b> tasks</span>
            <span v-if="connStore.isRemote" class="d-chip d-chip-remote" :title="`Connected to ${connStore.host}:${connStore.port}`">
              <Zap :size="12" /> {{ connStore.host }}:{{ connStore.port }}
            </span>
          </div>
          <span v-if="activePlatform === 'windows'" class="d-cnt">{{ filteredCount }}<small>/{{ totalCount }}</small></span>
          <span v-else class="d-cnt linux-hint">Linux management preview</span>
          <div class="d-bar-actions">
            <button
              v-for="btn in pluginStore.allToolbarButtons"
              :key="btn.id"
              class="d-ibtn d-plugin-tbtn"
              :title="btn.tooltip || btn.label"
              @click="executePluginAction(btn.action)"
            >
              {{ btn.label }}
            </button>
            <span class="d-clock">{{ clock }}</span>
          <button class="d-ibtn" @click="handleSync" title="Sync (Ctrl+R)">
              <RefreshCw :size="14" :class="{ spinning: syncing }" />
            </button>
            <button class="d-ibtn" @click="toggleTheme($event)" :title="isLight ? 'Dark mode' : 'Light mode'">
              <Sun v-if="!isLight" :size="14" />
              <Moon v-else :size="14" />
            </button>
            <button class="d-ibtn d-ibtn-exit" @click="logout" title="Logout">
              <LogOut :size="14" />
            </button>
          </div>
          <button class="d-ibtn d-fs-btn" @click="topFullscreen = !topFullscreen"
            :title="topFullscreen ? 'Exit Fullscreen (Esc)' : 'Fullscreen'">
            <Minimize2 v-if="topFullscreen" :size="13" />
            <Maximize2 v-else :size="13" />
          </button>
        </div>
        <div class="d-top-body">
          <div v-if="activePlatform === 'windows'" class="d-table-wrap">
            <BeaconTable :filterText="filter" @interact="onInteract" @contextmenu="onCtxMenu" />
          </div>
          <div v-else class="linux-agent-frame">
            <div class="linux-agent-shell">
              <div class="linux-agent-topline">
                <span class="linux-dot"></span>
                <span class="mono">linux-agent@xstrike:~$</span>
                <span class="linux-status">management skeleton</span>
              </div>
              <div class="linux-agent-grid">
                <section>
                  <span>Agents</span>
                  <strong>Linux agent list placeholder</strong>
                </section>
                <section>
                  <span>Tasks</span>
                  <strong>Linux task pipeline placeholder</strong>
                </section>
                <section>
                  <span>Controls</span>
                  <strong>Agent controls placeholder</strong>
                </section>
              </div>
            </div>
          </div>
        </div>
        <!-- Floating selection bar -->
        <Transition v-if="activePlatform === 'windows'" name="pop">
          <div v-if="selectedCount > 0" class="d-sel">
            <span><b>{{ selectedCount }}</b> selected</span>
            <button @click="batchSleep">Sleep</button>
            <button @click="batchExit" class="danger">Exit</button>
            <button @click="batchDelete" class="danger">Delete</button>
            <button @click="clearSel" class="ghost-btn">Clear</button>
          </div>
        </Transition>
      </section>

      <!-- Divider -->
      <div class="d-divider" v-show="!btmFullscreen && !topFullscreen" @mousedown.prevent="startResize"><div class="d-grip" /></div>

      <!-- Bottom: Operations -->
      <section class="d-btm" v-show="!topFullscreen">
        <nav class="d-tabs">
          <button class="d-tabs-brand" @click="togglePlatform($event)" :title="platformTitle">
            <img :src="logoSrc" alt="" class="d-logo d-tabs-logo" />
            <span class="d-name d-tabs-name">XStrike</span>
            <span class="d-platform" :class="`platform-${activePlatform}`">
              {{ platformLabel }}
            </span>
          </button>
          <template v-if="activePlatform === 'windows'">
            <button v-for="t in allTabs" :key="t.id"
            :class="['d-tab', { active: activeTab === t.id }]"
            @click="activeTab = t.id">
              <component v-if="t.icon" :is="t.icon" :size="12" />
              {{ t.label }}
              <span v-if="t.closable" class="d-tab-x" @click.stop="closeTab(t.id)">×</span>
            </button>
          </template>
          <template v-else>
            <button class="d-tab active">
              <TerminalSquare :size="12" />
              Linux Console
            </button>
            <button class="d-tab d-tab-disabled">Agents</button>
            <button class="d-tab d-tab-disabled">Tasks</button>
            <button class="d-tab d-tab-disabled">Files</button>
          </template>
          <button class="d-ibtn d-fs-btn" @click="btmFullscreen = !btmFullscreen"
            :title="btmFullscreen ? 'Exit Fullscreen (Esc)' : 'Fullscreen'">
            <Minimize2 v-if="btmFullscreen" :size="13" />
            <Maximize2 v-else :size="13" />
          </button>
        </nav>
        <div class="d-content">
          <div v-if="activePlatform === 'linux'" class="linux-console-frame">
            <div class="linux-console-shell">
              <div class="linux-console-topline">
                <span class="linux-dot"></span>
                <span class="mono">linux@xstrike:~$</span>
                <span class="linux-status">pending design</span>
              </div>
              <div class="linux-console-grid">
                <section>
                  <span>Shell</span>
                  <strong>Command surface placeholder</strong>
                </section>
                <section>
                  <span>Agents</span>
                  <strong>Linux agent controls placeholder</strong>
                </section>
                <section>
                  <span>Operations</span>
                  <strong>Workflow panels placeholder</strong>
                </section>
              </div>
            </div>
          </div>
          <template v-else>
            <Terminal v-if="activeTab === 'console'" />
            <BofLibrary v-else-if="activeTab === 'library'" />
            <PluginManager v-else-if="activeTab === 'plugins'" />
            <ListenerManager v-else-if="activeTab === 'listeners'" />
            <TaskLog v-else-if="activeTab === 'log'" />

            <!-- VNC sessions panel -->
            <div v-else-if="activeTab === 'vnc'" class="d-vnc">
            <!-- VNC Listener control bar -->
            <div class="d-vnc-bar d-vnc-listener-bar">
              <span class="d-vnc-listener-status" :class="vncListenerRunning ? 'vl-running' : 'vl-stopped'">
                <span class="d-vnc-dot" />
                {{ vncListenerRunning ? 'LISTENING' : 'STOPPED' }}
              </span>
              <span v-if="vncListenerRunning" class="d-vnc-listener-info">
                Port <strong>{{ vncListenerPort }}</strong> · {{ vncSessionCount }} session{{ vncSessionCount !== 1 ? 's' : '' }}
              </span>
              <span style="flex:1"></span>
              <template v-if="!vncListenerRunning">
                <input v-model.number="vncStartPort" type="number" min="1" max="65535" class="d-vnc-in" style="width:64px;text-align:center" placeholder="Port" />
                <button class="d-vnc-btn pri" @click="startVncListener" :disabled="vncListenerLoading">
                  <Play :size="13" /> Start
                </button>
              </template>
              <template v-else>
                <button class="d-vnc-btn danger" @click="stopVncListener" :disabled="vncListenerLoading">
                  <Square :size="12" /> Stop
                </button>
              </template>
            </div>
            <!-- Session list + manual connect -->
            <div class="d-vnc-bar">
              <input v-model="vncManualId" class="d-vnc-in" placeholder="Enter node_id..."
                :disabled="!vncListenerRunning"
                @keydown.enter="openVnc({ node_id: vncManualId })" />
              <button class="d-vnc-btn pri" @click="openVnc({ node_id: vncManualId })"
                :disabled="!vncManualId.trim() || !vncListenerRunning">Connect</button>
              <button class="d-vnc-btn icon-only" @click="refreshVnc" :disabled="!vncListenerRunning" title="Refresh">
                <RefreshCw :size="13" />
              </button>
            </div>
            <table v-if="vncListenerRunning && vncSessions.length" class="d-vnc-tbl">
              <thead><tr><th>NODE</th><th>RESOLUTION</th><th>CONNECTED</th><th></th></tr></thead>
              <tbody>
                <tr v-for="s in vncSessions" :key="s.node_id">
                  <td class="mono">{{ s.node_id }}</td>
                  <td class="mono">{{ s.width }}×{{ s.height }}</td>
                  <td class="mono dim">{{ fmtVncTime(s.connected_at) }}</td>
                  <td class="d-vnc-acts">
                    <button @click="openVnc(s)">View</button>
                    <button class="danger" @click="killVnc(s)">Kill</button>
                  </td>
                </tr>
              </tbody>
            </table>
            <div v-else-if="!vncListenerRunning" class="d-empty">
              <Radio :size="28" />
              <div>VNC listener is not running</div>
              <div style="font-size:11px;color:var(--tx-3)">Start the listener to accept VNC agent connections</div>
            </div>
            <div v-else class="d-empty">No VNC sessions connected</div>
            </div>

            <!-- Dynamic agent / vnc / plugin tabs -->
            <template v-for="t in dynTabs" :key="t.id">
              <div v-if="activeTab === t.id && t.type === 'agent'" class="d-fill">
                <AgentWorkspace :targetId="t.beaconId!" :initialView="t.initialView" :persistenceMode="t.persistenceMode" />
              </div>
              <div v-if="activeTab === t.id && t.type === 'vnc'" class="d-fill">
                <VncViewer :targetId="t.nodeId!" />
              </div>
              <div v-if="activeTab === t.id && t.type === 'plugin-panel'" class="d-fill">
                <PluginRenderer :layout="t.panelLayout!" :targetId="t.beaconId" />
              </div>
            </template>
          </template>
        </div>
      </section>
    </main>

    <!-- Overlays -->
    <div v-if="modalStore.isOpen && modalStore.component" class="d-overlay" @click.self="modalStore.close">
      <component :is="modalStore.component" v-bind="modalStore.props" />
    </div>
    <ContextMenu />
    <ToastContainer />
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { useRouter } from 'vue-router'
import { useAppStore } from '../stores/app'
import { useModalStore } from '../stores/modal'
import { useContextMenuStore } from '../stores/contextMenu'
import { useToastStore } from '../stores/toast'
import { useConnectionStore } from '../stores/connection'
import { useEventStream } from '../composables/useEventStream'
import {
  RefreshCw, LogOut, Search, Sun, Moon,
  TerminalSquare, Package, Radio, Monitor, FileText,
  Terminal as TermIcon, Files, Cpu, Power, Trash2,
  ShieldPlus, Layers, Camera, Clock, Maximize2, Minimize2,
  Puzzle, Play, Square, Zap
} from 'lucide-vue-next'
import api from '../services/api'
import { asset } from '../runtime/env'
import { apiFetch } from '../runtime/network'

import BeaconTable from '../components/BeaconTable.vue'
import Terminal from '../components/Terminal.vue'
import BofLibrary from '../components/BofLibrary.vue'
import ListenerManager from '../components/ListenerManager.vue'
import TaskLog from '../components/TaskLog.vue'
import AgentWorkspace from '../components/AgentWorkspace.vue'
import VncViewer from '../components/VncViewer.vue'
import ConfirmModal from '../components/ConfirmModal.vue'
import SetSleepModal from '../components/SetSleepModal.vue'
import ContextMenu from '../components/ContextMenu.vue'
import ToastContainer from '../components/ToastContainer.vue'
import PluginManager from '../components/PluginManager.vue'
import PluginRenderer from '../components/PluginRenderer.vue'
import { usePluginStore } from '../stores/plugin'
import type { PluginAction } from '../stores/plugin'

const router = useRouter()
const appStore = useAppStore()
const modalStore = useModalStore()
const ctxStore = useContextMenuStore()
const toastStore = useToastStore()
const connStore = useConnectionStore()
const pluginStore = usePluginStore()
const eventStream = useEventStream()

// ─── Theme ───
const THEME_KEY = 'ghost-theme'
const PLATFORM_KEY = 'ghost-platform'
const isLight = ref(localStorage.getItem(THEME_KEY) === 'light')
const logoSrc = computed(() => asset(isLight.value ? '/ico/xstrike-icon-light.png' : '/ico/xstrike-icon-dark.png'))
const rippleX = ref('50%')
const rippleY = ref('50%')
const rippleA = ref('rgba(0, 0, 0, 0.85)')
const rippleB = ref('rgba(255, 255, 255, 0.92)')
const rippleStyle = computed(() => ({
  '--ripple-x': rippleX.value,
  '--ripple-y': rippleY.value,
  '--ripple-a': rippleA.value,
  '--ripple-b': rippleB.value,
}))

type RipplePalette = { inner: string; outer: string }
const startRipple = (event?: MouseEvent, palette?: RipplePalette) => {
  if (event?.currentTarget instanceof HTMLElement) {
    const rect = event.currentTarget.getBoundingClientRect()
    rippleX.value = `${rect.left + rect.width / 2}px`
    rippleY.value = `${rect.top + rect.height / 2}px`
  }
  if (palette) {
    rippleA.value = palette.inner
    rippleB.value = palette.outer
  }
  platformSwitching.value = false
  if (platformSwitchTimer) window.clearTimeout(platformSwitchTimer)
  requestAnimationFrame(() => {
    platformSwitching.value = true
    platformSwitchTimer = window.setTimeout(() => {
      platformSwitching.value = false
      platformSwitchTimer = undefined
    }, 1400)
  })
}

const toggleTheme = (event?: MouseEvent) => {
  const nextLight = !isLight.value
  startRipple(event, nextLight
    ? { inner: 'rgba(0, 0, 0, 0.88)', outer: 'rgba(255, 255, 255, 0.98)' }
    : { inner: 'rgba(255, 255, 255, 0.98)', outer: 'rgba(0, 0, 0, 0.88)' }
  )
  isLight.value = !isLight.value
  localStorage.setItem(THEME_KEY, isLight.value ? 'light' : 'dark')
  document.documentElement.setAttribute('data-theme', isLight.value ? 'light' : 'dark')
}

type PlatformMode = 'windows' | 'linux'
const activePlatform = ref<PlatformMode>(
  localStorage.getItem(PLATFORM_KEY) === 'linux' ? 'linux' : 'windows'
)
const platformSwitching = ref(false)
let platformSwitchTimer: number | undefined
const platformLabel = computed(() => activePlatform.value === 'windows' ? 'Windows' : 'Linux · pending')
const searchPlaceholder = computed(() => activePlatform.value === 'windows' ? 'Search agents...' : 'Search Linux agents...')
const platformTitle = computed(() =>
  activePlatform.value === 'windows'
    ? 'Current platform: Windows. Click to preview Linux mode.'
    : 'Linux platform is pending development. Click to switch back to Windows.'
)
const togglePlatform = (event?: MouseEvent) => {
  const nextPlatform = activePlatform.value === 'windows' ? 'linux' : 'windows'
  startRipple(event, nextPlatform === 'windows'
    ? { inner: 'rgba(110, 168, 217, 0.55)', outer: 'rgba(255, 255, 255, 0)' }
    : { inner: 'rgba(217, 154, 69, 0.55)', outer: 'rgba(255, 255, 255, 0)' }
  )
  activePlatform.value = activePlatform.value === 'windows' ? 'linux' : 'windows'
  localStorage.setItem(PLATFORM_KEY, activePlatform.value)
  if (activePlatform.value === 'linux') {
    toastStore.warning('Linux platform is still under development')
  } else {
    toastStore.success('Switched to Windows platform')
  }
}

// ─── State ───
const filter = ref('')
const searchFocus = ref(false)
const searchEl = ref<HTMLInputElement>()
const activeTab = ref('console')
const syncing = ref(false)
const clock = ref('')
const vncManualId = ref('')
const vncSessions = ref<any[]>([])

// VNC listener state
const vncListenerRunning = ref(false)
const vncListenerPort    = ref(9998)
const vncListenerLoading = ref(false)
const vncStartPort       = ref(9998)
const vncSessionCount    = ref(0)

const btmFullscreen = ref(false)
const topFullscreen = ref(false)
type AgentView = 'cmd' | 'powershell' | 'files' | 'processes' | 'persistence' | 'shellcode' | 'screenshots'
type PersistenceMode = 'schtask' | 'service' | 'critical' | 'user'

interface TabDef {
  id: string
  label: string
  icon?: any
  closable?: boolean
  type?: 'agent' | 'vnc' | 'plugin-panel'
  beaconId?: string
  nodeId?: string
  initialView?: AgentView
  persistenceMode?: PersistenceMode
  panelLayout?: any
}

const fixedTabs: TabDef[] = [
  { id: 'console',   label: 'Console',   icon: TerminalSquare },
  { id: 'library',   label: 'Library',   icon: Package },
  { id: 'plugins',   label: 'Plugins',   icon: Puzzle },
  { id: 'listeners', label: 'Listeners', icon: Radio },
  { id: 'vnc',       label: 'VNC',       icon: Monitor },
  { id: 'log',       label: 'Log',       icon: FileText },
]
const dynTabs = ref<TabDef[]>([])
const allTabs = computed<TabDef[]>(() => [
  ...fixedTabs,
  ...dynTabs.value,
])

// ─── Computed ───
const totalCount = computed(() => appStore.beacons.length)
const aliveCount = computed(() => {
  const now = Math.floor(Date.now() / 1000)
  return appStore.beacons.filter((b: any) => {
    const ls = Number(b.last_seen || 0)
    return ls > 0 && (now - ls) < 300
  }).length
})
const bofCount = computed(() => appStore.stats.bofs)
const taskCount = computed(() => appStore.stats.tasks)
const filteredCount = computed(() => {
  const q = filter.value.trim().toLowerCase()
  if (!q) return totalCount.value
  return appStore.beacons.filter((b: any) => {
    return [b.hostname, b.computer, b.node_id, b.id, b.ip, b.internal_ip, b.user]
      .some(v => String(v || '').toLowerCase().includes(q))
  }).length
})
const selectedBeacons = computed(() => appStore.beacons.filter((b: any) => b.selected))
const selectedCount = computed(() => selectedBeacons.value.length)

// ─── Layout ───
const topH = ref(300)
const resizing = ref(false)
const startResize = () => {
  resizing.value = true
  window.addEventListener('mousemove', onMouseMove)
  window.addEventListener('mouseup', onMouseUp)
}
const onMouseMove = (e: MouseEvent) => {
  if (!resizing.value) return
  topH.value = Math.max(120, Math.min(e.clientY - 44, window.innerHeight - 160))
}
const onMouseUp = () => {
  resizing.value = false
  window.removeEventListener('mousemove', onMouseMove)
  window.removeEventListener('mouseup', onMouseUp)
}

// ─── Clock ───
const updateClock = () => {
  clock.value = new Date().toLocaleTimeString([], { hour12: false })
}

// ─── Sync ───
const handleSync = async () => {
  syncing.value = true
  await appStore.refreshAll()
  window.dispatchEvent(new CustomEvent('ghost:sync', { detail: { activeTab: activeTab.value, ts: Date.now() } }))
  toastStore.success('Synced')
  syncing.value = false
}

// ─── Beacon interaction ───
const isAlive = (b: any) => {
  const ls = Number(b?.last_seen || 0)
  return ls > 0 && (Math.floor(Date.now() / 1000) - ls) < 300
}

const openBeaconView = (beacon: any, view: AgentView, pm: PersistenceMode = 'schtask') => {
  if (!isAlive(beacon)) {
    toastStore.warning(`Beacon is offline: ${beacon.node_id || beacon.id}`)
    return
  }
  const bid = beacon.node_id || beacon.id
  const tabId = `beacon-${bid}`
  const existing = dynTabs.value.find(t => t.id === tabId)
  if (!existing) {
    dynTabs.value.push({
      id: tabId, label: beacon.computer || bid.slice(0, 10),
      type: 'agent', closable: true, beaconId: bid,
      initialView: view, persistenceMode: pm,
    })
  } else {
    existing.initialView = view
    if (view === 'persistence') existing.persistenceMode = pm
  }
  activeTab.value = tabId
}

const onInteract = (beacon: any) => openBeaconView(beacon, 'cmd')

const onCtxMenu = (e: MouseEvent, beacon: any) => {
  ctxStore.show(e.pageX, e.pageY, [
    { label: 'Interact (CMD)', icon: TermIcon, action: () => openBeaconView(beacon, 'cmd') },
    { label: 'Interact (PS)', icon: TermIcon, action: () => openBeaconView(beacon, 'powershell') },
    { label: 'File Browser', icon: Files, action: () => openBeaconView(beacon, 'files') },
    { label: 'Processes', icon: Cpu, action: () => openBeaconView(beacon, 'processes') },
    { label: 'Screenshot', icon: Camera, action: () => openBeaconView(beacon, 'screenshots') },
    { label: 'Shellcode', icon: Layers, action: () => openBeaconView(beacon, 'shellcode') },
    { divider: true, label: '', action: () => {} },
    { label: 'Persistence', icon: ShieldPlus, action: () => openBeaconView(beacon, 'persistence') },
    { divider: true, label: '', action: () => {} },
    { label: 'Set Sleep', icon: Clock, action: () => openSleepModal([beacon.node_id || beacon.id]) },
    {
      label: 'Exit Agent', icon: Power, danger: true,
      action: () => modalStore.open(ConfirmModal, {
        title: 'Exit Agent',
        message: `Send exit to ${beacon.node_id || beacon.id}?`,
        confirmText: 'Exit', type: 'danger',
        onResolve: async () => {
          await api.post(`/nodes/${beacon.node_id || beacon.id}/stop`)
          toastStore.warning('Exit command sent')
        }
      })
    },
    {
      label: 'Delete Beacon', icon: Trash2, danger: true,
      action: () => modalStore.open(ConfirmModal, {
        title: 'Delete Beacon',
        message: `Delete ${beacon.node_id || beacon.id}?`,
        confirmText: 'Delete', type: 'danger',
        onResolve: async () => {
          await api.post(`/nodes/${beacon.node_id || beacon.id}/delete`)
          await appStore.fetchBeacons()
          toastStore.error('Beacon deleted')
        }
      })
    },
    ...buildPluginMenuItems(beacon),
  ])
}

// ─── Batch ops ───
const clearSel = () => { appStore.beacons.forEach((b: any) => { b.selected = false }) }

// ─── Plugin action helpers ───
const executePluginAction = async (action?: PluginAction, beaconId?: string) => {
  if (!action) return
  if (action.type === 'execute_bof') {
    const targetId = beaconId
    if (!targetId) {
      toastStore.error('No target beacon selected for plugin action')
      return
    }
    if (!action.bof_name) return
    try {
      const payload: any = {
        node_id: targetId,
        bof_name: action.bof_name,
        plugin_name: action.plugin_name || '',
      }
      const res = await api.post('/bof/execute', payload)
      if (res.data.success) {
        toastStore.success(`BOF task created: ${action.bof_name}`)
      }
    } catch {
      toastStore.error('Plugin BOF execution failed')
    }
  } else if (action.type === 'open_panel') {
    if (!action.panel_id) return
    openPluginPanel(action.panel_id, beaconId)
  }
}

const openPluginPanel = (panelId: string, beaconId?: string) => {
  // Find the panel in all manifests
  for (const m of pluginStore.enabledManifests) {
    if (m.panels) {
      for (const panel of m.panels) {
        if (panel.id === panelId) {
          const tabId = `plugin-${panelId}`
          if (!dynTabs.value.find(t => t.id === tabId)) {
            dynTabs.value.push({
              id: tabId,
              label: panel.title,
              type: 'plugin-panel',
              closable: true,
              beaconId: beaconId,
              panelLayout: panel.layout,
            })
          }
          activeTab.value = tabId
          return
        }
      }
    }
  }
}

/** Build context menu items from plugin menus */
const buildPluginMenuItems = (beacon: any): any[] => {
  const pluginMenus = pluginStore.menusForLocation('beacon_context_menu')
  if (!pluginMenus.length) return []

  const items: any[] = [{ divider: true, label: '', action: () => {} }]
  const bid = beacon.node_id || beacon.id

  for (const menu of pluginMenus) {
    if (menu.children?.length) {
      // Render each child as a menu item
      for (const child of menu.children) {
        items.push({
          label: `${menu.label} → ${child.label}`,
          icon: Puzzle,
          action: () => executePluginAction(child.action, bid),
        })
      }
    } else {
      items.push({
        label: menu.label,
        icon: Puzzle,
        action: () => {},
      })
    }
  }
  return items
}

const openSleepModal = (ids: string[]) => {
  modalStore.open(SetSleepModal, {
    targetIds: ids,
    onResolve: async (ms: number) => {
      for (const id of ids) {
        const taskId = `sleep-${Date.now()}-${Math.random().toString(36).substr(2, 9)}`
        await api.post('/tasks', { node_id: id, task: { SetSleep: { task_id: taskId, interval_ms: ms } } })
      }
      toastStore.success(`Sleep set to ${ms}ms for ${ids.length} agent(s)`)
      modalStore.close()
    },
    onReject: () => modalStore.close()
  })
}

const batchSleep = () => {
  const ids = selectedBeacons.value.map((b: any) => b.node_id || b.id).filter(Boolean)
  if (ids.length) openSleepModal(ids)
}

const batchExit = () => {
  const ids = selectedBeacons.value.map((b: any) => b.node_id || b.id).filter(Boolean)
  if (!ids.length) return
  modalStore.open(ConfirmModal, {
    title: 'Batch Exit', message: `Exit ${ids.length} agent(s)?`,
    confirmText: 'Exit', type: 'danger',
    onResolve: async () => {
      const res = await api.post('/nodes/batch/stop', { node_ids: ids })
      if (res.data.success) { toastStore.warning(res.data.data || 'Batch exit sent'); clearSel(); await appStore.fetchBeacons() }
    }
  })
}

const batchDelete = () => {
  const ids = selectedBeacons.value.map((b: any) => b.node_id || b.id).filter(Boolean)
  if (!ids.length) return
  modalStore.open(ConfirmModal, {
    title: 'Batch Delete', message: `Delete ${ids.length} record(s)?`,
    confirmText: 'Delete', type: 'danger',
    onResolve: async () => {
      const res = await api.post('/nodes/batch/delete', { node_ids: ids })
      if (res.data.success) { toastStore.error(res.data.data || 'Batch deleted'); clearSel(); await appStore.fetchBeacons() }
    }
  })
}

// ─── VNC ───
const refreshVncListenerStatus = async () => {
  try {
    const res = await api.get('/vnc/listener', { silentError: true } as any)
    vncListenerRunning.value = res.data.running
    vncListenerPort.value    = res.data.port
    vncSessionCount.value    = res.data.session_count
    if (!vncListenerRunning.value) {
      vncStartPort.value = res.data.port
    }
  } catch { /* server may not be up */ }
}

const startVncListener = async () => {
  vncListenerLoading.value = true
  try {
    await api.post('/vnc/listener/start', { port: vncStartPort.value })
    toastStore.success(`VNC listener started on port ${vncStartPort.value}`)
    await refreshVncListenerStatus()
  } catch (e: any) {
    toastStore.error(e?.response?.data?.error || e.message || 'Failed to start VNC listener')
  } finally {
    vncListenerLoading.value = false
  }
}

const stopVncListener = async () => {
  vncListenerLoading.value = true
  try {
    await api.post('/vnc/listener/stop')
    toastStore.success('VNC listener stopped')
    vncSessions.value = []
    await refreshVncListenerStatus()
  } catch (e: any) {
    toastStore.error(e?.response?.data?.error || e.message || 'Failed to stop VNC listener')
  } finally {
    vncListenerLoading.value = false
  }
}

const refreshVnc = async () => {
  try {
    const res = await api.get('/vnc/sessions', { silentError: true } as any)
    vncSessions.value = (res.data.sessions || []).sort((a: any, b: any) => b.connected_at - a.connected_at)
  } catch { /* ignore */ }
}

const fmtVncTime = (ts: number) => ts ? new Date(ts * 1000).toLocaleTimeString([], { hour12: false }) : '—'

const openVnc = (s: { node_id: string }) => {
  if (!s.node_id?.trim()) return
  const nid = s.node_id.trim()
  const tabId = `vnc-${nid}`
  if (!dynTabs.value.find(t => t.id === tabId)) {
    dynTabs.value.push({ id: tabId, label: `VNC:${nid.slice(0, 8)}`, type: 'vnc', closable: true, nodeId: nid })
  }
  activeTab.value = tabId
  vncManualId.value = ''
}

const killVnc = async (s: any) => {
  try {
    await apiFetch(`/vnc/${s.node_id}/disconnect`, { method: 'POST' })
    await refreshVnc()
    dynTabs.value = dynTabs.value.filter(t => t.id !== `vnc-${s.node_id}`)
    if (activeTab.value === `vnc-${s.node_id}`) activeTab.value = 'console'
  } catch { /* ignore */ }
}

// ─── Tab management ───
const isClosable = (id: string) => !fixedTabs.some(f => f.id === id)

const closeTab = (id: string) => {
  if (activeTab.value === id) {
    const all = allTabs.value
    const idx = all.findIndex(t => t.id === id)
    const next = all[idx + 1] || all[idx - 1] || all[0]
    activeTab.value = next?.id ?? 'console'
  }
  dynTabs.value = dynTabs.value.filter(t => t.id !== id)
}

// ─── Auth ───
const logout = () => {
  localStorage.removeItem('token')
  if (connStore.isRemote) {
    connStore.disconnect()
    router.push('/connect')
  } else {
    router.push('/login')
  }
}

// ─── Keyboard shortcuts ───
const onKey = (e: KeyboardEvent) => {
  if (e.ctrlKey && e.key === 'r') { e.preventDefault(); handleSync() }
  if (e.ctrlKey && e.key === 'w') { e.preventDefault(); if (isClosable(activeTab.value)) closeTab(activeTab.value) }
  if (e.ctrlKey && e.key === 'f') { e.preventDefault(); searchEl.value?.focus() }
  if (e.key === 'Escape') {
    if (btmFullscreen.value) btmFullscreen.value = false
    else if (topFullscreen.value) topFullscreen.value = false
    else if (modalStore.isOpen) modalStore.close()
  }
}

// ─── Lifecycle ───
let clockTimer: any, beaconPollTimer: any, vncTimer: any

onMounted(() => {
  document.documentElement.setAttribute('data-theme', isLight.value ? 'light' : 'dark')
  appStore.refreshAll()
  pluginStore.refreshAll()
  updateClock()
  clockTimer = setInterval(updateClock, 1000)
  refreshVncListenerStatus()
  refreshVnc()
  vncTimer = setInterval(() => {
    refreshVncListenerStatus()
    if (vncListenerRunning.value) refreshVnc()
  }, 4000)
  window.addEventListener('keydown', onKey)

  // Set up SSE real-time updates with polling fallback
  eventStream.setCallbacks({
    onBeaconUpdated: (data) => appStore.handleBeaconUpdated(data),
    onBeaconDeleted: (data) => appStore.handleBeaconDeleted(data),
    onTaskCompleted: (data) => appStore.handleTaskCompleted(data),
    onListenerChanged: (data) => appStore.handleListenerChanged(data),
  })

  if (typeof EventSource !== 'undefined') {
    eventStream.connect()
    // Fallback polling at a reduced rate, in case SSE disconnects silently
    beaconPollTimer = setInterval(() => {
      if (!eventStream.connected.value) {
        appStore.fetchBeacons()
      }
    }, 10000)
  } else {
    // No EventSource support — use traditional polling
    beaconPollTimer = setInterval(() => appStore.fetchBeacons(), 5000)
  }
})
onUnmounted(() => {
  eventStream.disconnect()
  clearInterval(clockTimer)
  clearInterval(beaconPollTimer)
  clearInterval(vncTimer)
  if (platformSwitchTimer) window.clearTimeout(platformSwitchTimer)
  window.removeEventListener('keydown', onKey)
})
</script>

<style scoped>
.dusk {
  --bg:    #151922;
  --bg-2:  #1d232d;
  --bg-3:  #242b36;
  --bg-4:  #2d3542;
  --bd:    rgba(191, 203, 218, 0.14);
  --bd-2:  rgba(191, 203, 218, 0.22);
  --tx:    #edf1f6;
  --tx-2:  #aeb9c7;
  --tx-3:  #778394;
  --tx-4:  #4f5b6b;
  --pri:   #4fc3ad;
  --pri-h: #3aaf98;
  --platform-wash: rgba(79, 195, 173, 0.06);
  --blue:  #6ea8d9;
  --red:   #f26f6f;
  --amber: #e0b154;
  --purple:#a78bfa;
  --cyan:  #35c7d0;
  --green: #50c878;
  --font-mono: 'JetBrains Mono', 'Fira Code', monospace;
  --font-sans: 'Inter', system-ui, -apple-system, sans-serif;

  position: relative;
  display: flex;
  flex-direction: column;
  height: 100vh;
  background:
    linear-gradient(180deg, var(--platform-wash), transparent 26%),
    var(--bg);
  color: var(--tx);
  font-family: var(--font-sans);
  overflow: hidden;
  transition: background-color 0.32s ease, color 0.32s ease;
}

.dusk.light {
  --bg:    #eef1f5;
  --bg-2:  #fbfcfe;
  --bg-3:  #f3f5f8;
  --bg-4:  #e7ebf0;
  --bd:    rgba(43, 59, 79, 0.13);
  --bd-2:  rgba(43, 59, 79, 0.20);
  --tx:    #17202b;
  --tx-2:  #526173;
  --tx-3:  #8491a2;
  --tx-4:  #b2bbc7;
  --pri:   #168b78;
  --pri-h: #117665;
  --platform-wash: rgba(22, 139, 120, 0.05);
  --red:   #c94c4c;
  --amber: #9a6c16;
  --purple:#7158c8;
  --cyan:  #0c909a;
  --green: #15834f;
}
.dusk.platform-windows {
  --pri:   #6ea8d9;
  --pri-h: #5a94c5;
  --platform-wash: rgba(110, 168, 217, 0.08);
}
.dusk.platform-linux {
  --pri:   #d99a45;
  --pri-h: #bd7f29;
  --platform-wash: rgba(217, 154, 69, 0.08);
}
.dusk.light.platform-windows {
  --pri:   #6baee8;
  --pri-h: #4f98d6;
  --blue:  #4f98d6;
  --platform-wash: rgba(107, 174, 232, 0.12);
}
.dusk.light.platform-linux {
  --pri:   #e2a258;
  --pri-h: #c9822d;
  --amber: #c9822d;
  --platform-wash: rgba(226, 162, 88, 0.14);
}
.dusk::after {
  content: '';
  position: absolute;
  inset: 0;
  pointer-events: none;
  z-index: 900;
  opacity: 0;
  background: radial-gradient(
    circle at var(--ripple-x) var(--ripple-y),
    color-mix(in srgb, var(--ripple-a) 42%, transparent) 0%,
    color-mix(in srgb, var(--ripple-b) 24%, transparent) 24%,
    transparent 62%
  );
  clip-path: circle(0 at var(--ripple-x) var(--ripple-y));
}
.dusk.platform-switching::after {
  animation: platform-ripple 1.4s ease-out;
}
@keyframes platform-ripple {
  0% {
    opacity: 0.95;
    clip-path: circle(0 at var(--ripple-x) var(--ripple-y));
  }
  68% {
    opacity: 0.42;
    clip-path: circle(118vmax at var(--ripple-x) var(--ripple-y));
  }
  100% {
    opacity: 0;
    clip-path: circle(150vmax at var(--ripple-x) var(--ripple-y));
  }
}

/* ─── Header ─── */
.d-hdr {
  height: 56px;
  display: flex;
  align-items: center;
  padding: 0 18px;
  background: color-mix(in srgb, var(--bg-2) 92%, transparent);
  border-bottom: 1px solid var(--bd);
  flex-shrink: 0;
  gap: 24px;
  box-shadow: 0 10px 30px rgba(0, 0, 0, 0.18);
  backdrop-filter: blur(16px);
}
.d-brand {
  display: flex;
  align-items: center;
  gap: 8px;
}
.d-logo {
  width: 28px;
  height: 28px;
  object-fit: contain;
  filter: drop-shadow(0 8px 14px rgba(79, 209, 177, 0.24));
}
.d-name {
  font-family: var(--font-mono);
  font-weight: 700;
  font-size: 17px;
  color: var(--pri);
}
.d-stats {
  display: flex;
  gap: 6px;
}
.d-chip {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  font-size: 11px;
  color: var(--tx-2);
  padding: 5px 10px;
  border-radius: 999px;
  background: color-mix(in srgb, var(--bg-3) 78%, transparent);
  border: 1px solid var(--bd);
  transition: background-color 0.28s ease, border-color 0.28s ease, color 0.28s ease;
}
.d-chip b {
  color: var(--tx);
  font-weight: 600;
  font-family: var(--font-mono);
}
.d-hdr-end {
  margin-left: auto;
  display: flex;
  align-items: center;
  gap: 8px;
}
.d-clock {
  font-family: var(--font-mono);
  font-size: 11px;
  color: var(--tx-3);
  min-width: 56px;
}
.d-ibtn {
  width: 32px;
  height: 32px;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: 8px;
  border: 1px solid transparent;
  background: transparent;
  color: var(--tx-2);
  cursor: pointer;
  transition: background 0.15s, color 0.15s, border-color 0.15s, transform 0.15s;
}
.d-ibtn:hover {
  background: var(--bg-3);
  border-color: var(--bd);
  color: var(--tx);
  transform: translateY(-1px);
}
.d-ibtn-exit:hover {
  color: var(--red);
}
.d-plugin-tbtn {
  font-size: 10px;
  width: auto;
  padding: 4px 10px;
  border-radius: 8px;
  background: color-mix(in srgb, var(--pri) 12%, var(--bg-3));
  border: 1px solid color-mix(in srgb, var(--pri) 25%, var(--bd));
  color: var(--pri);
  transition: background-color 0.28s ease, border-color 0.28s ease, color 0.28s ease;
}
.d-plugin-tbtn:hover {
  background: color-mix(in srgb, var(--pri) 22%, var(--bg-3));
}

/* ─── Body ─── */
.d-body {
  flex: 1;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  padding: 14px;
  gap: 10px;
}

/* ─── Top Pane (Beacons) ─── */
.d-top {
  display: flex;
  flex-direction: column;
  min-height: 120px;
  position: relative;
  overflow: hidden;
  border: 1px solid var(--bd);
  border-radius: 12px;
  background: color-mix(in srgb, var(--bg-2) 78%, transparent);
  box-shadow: 0 18px 45px rgba(0, 0, 0, 0.16);
  transition: background-color 0.28s ease, border-color 0.28s ease;
}
.d-top-full {
  flex: 1;
  min-height: 0;
}
.d-bar {
  height: 44px;
  display: flex;
  align-items: center;
  padding: 0 10px 0 14px;
  gap: 10px;
  flex-shrink: 0;
  background: linear-gradient(180deg, color-mix(in srgb, var(--bg-3) 76%, transparent), color-mix(in srgb, var(--bg-2) 86%, transparent));
  border-bottom: 1px solid var(--bd);
  transition: background-color 0.28s ease, border-color 0.28s ease;
}
.d-search {
  display: flex;
  align-items: center;
  gap: 6px;
  height: 32px;
  padding: 0 10px;
  border-radius: 9px;
  background: color-mix(in srgb, var(--bg) 58%, transparent);
  border: 1px solid var(--bd);
  color: var(--tx-3);
  min-width: 220px;
  transition: border-color 0.15s, box-shadow 0.15s;
}
.d-bar-stats {
  flex-shrink: 1;
  min-width: 0;
  overflow: hidden;
}
.d-bar-actions {
  display: flex;
  align-items: center;
  gap: 6px;
  flex-shrink: 0;
}
.d-search.focus {
  border-color: var(--pri);
  box-shadow: 0 0 0 3px color-mix(in srgb, var(--pri) 16%, transparent);
}
.d-search input {
  flex: 1;
  border: none;
  outline: none;
  background: transparent;
  color: var(--tx);
  font-size: 11px;
}
.d-x {
  cursor: pointer;
  font-size: 14px;
  line-height: 1;
  color: var(--tx-3);
}
.d-x:hover { color: var(--red); }
.d-cnt {
  font-size: 11px;
  font-family: var(--font-mono);
  color: var(--tx-2);
  margin-left: auto;
}
.d-cnt small {
  color: var(--tx-3);
}

.d-table-wrap {
  flex: 1;
  overflow: hidden;
  background: var(--bg);
}
.d-top-body {
  flex: 1;
  min-height: 0;
  display: flex;
  overflow: hidden;
}
.linux-agent-frame {
  flex: 1;
  min-height: 0;
  padding: 14px;
  overflow: auto;
  background:
    radial-gradient(circle at 20% 18%, color-mix(in srgb, var(--pri) 12%, transparent), transparent 30%),
    color-mix(in srgb, var(--bg) 94%, transparent);
}
.linux-agent-shell {
  min-height: 100%;
  border: 1px solid color-mix(in srgb, var(--pri) 28%, var(--bd));
  border-radius: 10px;
  background: color-mix(in srgb, var(--bg-2) 74%, transparent);
  display: flex;
  flex-direction: column;
  gap: 12px;
  padding: 14px;
}
.linux-agent-topline {
  display: flex;
  align-items: center;
  gap: 8px;
  min-height: 34px;
  padding: 0 10px;
  border-radius: 8px;
  background: color-mix(in srgb, var(--bg-3) 82%, transparent);
  border: 1px solid var(--bd);
  color: var(--tx-2);
  font-size: 12px;
}
.linux-agent-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 12px;
}
.linux-agent-grid section {
  min-height: 110px;
  border: 1px solid var(--bd);
  border-radius: 8px;
  background: color-mix(in srgb, var(--bg-3) 54%, transparent);
  padding: 12px;
}
.linux-agent-grid span {
  display: block;
  color: var(--tx-3);
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: 0.06em;
  margin-bottom: 8px;
}
.linux-agent-grid strong {
  color: var(--tx-2);
  font-size: 13px;
  font-weight: 600;
}
.d-cnt.linux-hint {
  color: var(--amber);
}

/* ─── Floating selection bar ─── */
.d-sel {
  position: absolute;
  bottom: 12px;
  left: 50%;
  transform: translateX(-50%);
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 14px;
  background: color-mix(in srgb, var(--bg-3) 92%, transparent);
  border: 1px solid var(--bd-2);
  border-radius: 12px;
  font-size: 11px;
  color: var(--tx-2);
  box-shadow: 0 14px 40px rgba(0, 0, 0, 0.32);
  backdrop-filter: blur(12px);
  z-index: 20;
}
.d-sel b { color: var(--tx); font-family: var(--font-mono); }
.d-sel button {
  height: 24px;
  padding: 0 10px;
  border-radius: 5px;
  border: 1px solid var(--bd-2);
  background: var(--bg-4);
  color: var(--tx-2);
  font-size: 10px;
  cursor: pointer;
  transition: all 0.12s;
}
.d-sel button:hover {
  border-color: var(--pri);
  color: var(--pri);
}
.d-sel button.danger:hover {
  border-color: var(--red);
  color: var(--red);
}
.d-sel button.ghost-btn {
  border-color: transparent;
  background: transparent;
}
.d-sel button.ghost-btn:hover {
  color: var(--tx);
}

/* Pop transition */
.pop-enter-active { transition: all 0.2s ease-out; }
.pop-leave-active { transition: all 0.15s ease-in; }
.pop-enter-from,
.pop-leave-to { opacity: 0; transform: translateX(-50%) translateY(8px); }

/* ─── Divider ─── */
.d-divider {
  height: 8px;
  background: transparent;
  cursor: row-resize;
  position: relative;
  z-index: 10;
  flex-shrink: 0;
}
.d-divider:hover { background: color-mix(in srgb, var(--bg-3) 36%, transparent); }
.d-grip {
  position: absolute;
  left: 50%;
  top: 50%;
  transform: translate(-50%, -50%);
  width: 42px;
  height: 3px;
  border-radius: 1px;
  background: var(--tx-4);
  transition: background 0.12s;
}
.d-divider:hover .d-grip { background: var(--tx-3); }

/* ─── Bottom Pane ─── */
.d-btm {
  flex: 1;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  min-height: 0;
  overflow: hidden;
  border: 1px solid var(--bd);
  border-radius: 12px;
  background: color-mix(in srgb, var(--bg-2) 78%, transparent);
  box-shadow: 0 18px 45px rgba(0, 0, 0, 0.16);
  transition: background-color 0.28s ease, border-color 0.28s ease;
}

/* Tabs */
.d-tabs {
  display: flex;
  align-items: center;
  gap: 4px;
  padding: 8px 10px;
  background: linear-gradient(180deg, color-mix(in srgb, var(--bg-3) 78%, transparent), color-mix(in srgb, var(--bg-2) 86%, transparent));
  border-bottom: 1px solid var(--bd);
  transition: background-color 0.28s ease, border-color 0.28s ease;
  flex-shrink: 0;
  overflow-x: auto;
  scrollbar-width: none;
}
.d-tabs::-webkit-scrollbar { display: none; }
.d-tabs-brand {
  display: inline-flex;
  align-items: center;
  gap: 7px;
  min-height: 30px;
  padding: 0 12px 0 4px;
  margin-right: 4px;
  border: 0;
  border-right: 1px solid var(--bd);
  background: transparent;
  color: inherit;
  cursor: pointer;
  flex-shrink: 0;
  transition: background 0.12s, border-color 0.12s;
}
.d-tabs-brand:hover {
  background: color-mix(in srgb, var(--pri) 10%, transparent);
  border-right-color: color-mix(in srgb, var(--pri) 35%, var(--bd));
}
.d-tabs-logo {
  width: 22px;
  height: 22px;
  filter: drop-shadow(0 5px 10px rgba(79, 209, 177, 0.18));
}
.d-tabs-name {
  font-size: 14px;
}
.d-platform {
  display: inline-flex;
  align-items: center;
  height: 20px;
  padding: 0 7px;
  border-radius: 999px;
  border: 1px solid var(--bd);
  font-size: 10px;
  font-weight: 600;
  color: var(--tx-3);
  background: color-mix(in srgb, var(--bg-3) 78%, transparent);
}
.d-platform.platform-windows {
  color: var(--blue);
  border-color: color-mix(in srgb, var(--blue) 32%, var(--bd));
  background: color-mix(in srgb, var(--blue) 10%, var(--bg-3));
}
.d-platform.platform-linux {
  color: var(--amber);
  border-color: color-mix(in srgb, var(--amber) 38%, var(--bd));
  background: color-mix(in srgb, var(--amber) 10%, var(--bg-3));
}

.d-tab {
  display: flex;
  align-items: center;
  gap: 6px;
  min-height: 30px;
  padding: 5px 12px;
  border-radius: 8px;
  border: 1px solid transparent;
  background: transparent;
  color: var(--tx-3);
  font-size: 11px;
  font-family: var(--font-sans);
  cursor: pointer;
  white-space: nowrap;
  transition: background 0.12s, border-color 0.12s, color 0.12s;
}
.d-tab:hover {
  color: var(--tx-2);
  background: var(--bg-2);
}
.d-tab.active {
  color: var(--tx);
  background: color-mix(in srgb, var(--pri) 14%, var(--bg-3));
  border-color: color-mix(in srgb, var(--pri) 34%, var(--bd));
}
.d-tab-disabled {
  opacity: 0.42;
  cursor: not-allowed;
}
.d-tab-disabled:hover {
  color: var(--tx-3);
  background: transparent;
}
.d-tab-x {
  font-size: 14px;
  line-height: 1;
  margin-left: 2px;
  opacity: 0.4;
  transition: opacity 0.12s;
}
.d-tab-x:hover {
  opacity: 1;
  color: var(--red);
}

/* Fullscreen toggle button */
.d-fs-btn {
  margin-left: auto;
  opacity: 0.5;
  transition: opacity 0.15s;
}
.d-fs-btn:hover { opacity: 1; }
.d-bar .d-fs-btn {
  margin-left: 0;
  flex-shrink: 0;
}

/* Content area */
.d-content {
  flex: 1;
  background: color-mix(in srgb, var(--bg) 92%, transparent);
  overflow: hidden;
  display: flex;
  flex-direction: column;
  min-height: 0;
}
.d-fill {
  flex: 1;
  display: flex;
  flex-direction: column;
  min-height: 0;
}
.linux-console-frame {
  flex: 1;
  min-height: 0;
  padding: 14px;
  background:
    radial-gradient(circle at 18% 12%, color-mix(in srgb, var(--pri) 14%, transparent), transparent 32%),
    color-mix(in srgb, var(--bg) 92%, transparent);
  overflow: auto;
}
.linux-console-shell {
  min-height: 100%;
  border: 1px dashed color-mix(in srgb, var(--pri) 36%, var(--bd));
  border-radius: 10px;
  background: color-mix(in srgb, var(--bg-2) 72%, transparent);
  display: flex;
  flex-direction: column;
  gap: 14px;
  padding: 14px;
}
.linux-console-topline {
  display: flex;
  align-items: center;
  gap: 8px;
  min-height: 34px;
  padding: 0 10px;
  border-radius: 8px;
  background: color-mix(in srgb, var(--bg-3) 82%, transparent);
  border: 1px solid var(--bd);
  color: var(--tx-2);
  font-size: 12px;
}
.linux-dot {
  width: 8px;
  height: 8px;
  border-radius: 999px;
  background: var(--pri);
  box-shadow: 0 0 12px color-mix(in srgb, var(--pri) 70%, transparent);
}
.linux-status {
  margin-left: auto;
  font-size: 10px;
  color: var(--amber);
  text-transform: uppercase;
  letter-spacing: 0.06em;
}
.linux-console-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 12px;
}
.linux-console-grid section {
  min-height: 120px;
  border: 1px solid var(--bd);
  border-radius: 8px;
  background: color-mix(in srgb, var(--bg-3) 54%, transparent);
  padding: 12px;
}
.linux-console-grid span {
  display: block;
  color: var(--tx-3);
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: 0.06em;
  margin-bottom: 8px;
}
.linux-console-grid strong {
  color: var(--tx-2);
  font-size: 13px;
  font-weight: 600;
}

/* ─── VNC Panel ─── */
.d-vnc {
  flex: 1;
  display: flex;
  flex-direction: column;
  padding: 12px;
  gap: 12px;
  overflow: auto;
}
.d-vnc-bar {
  display: flex;
  gap: 8px;
  align-items: center;
  padding: 10px;
  border: 1px solid var(--bd);
  border-radius: 10px;
  background: var(--bg-2);
}
.d-vnc-in {
  flex: 1;
  max-width: 300px;
  height: 32px;
  padding: 0 10px;
  border-radius: 6px;
  border: 1px solid var(--bd);
  background: var(--bg-2);
  color: var(--tx);
  font-size: 11px;
  font-family: var(--font-mono);
  outline: none;
}
.d-vnc-in:focus { border-color: var(--pri); }
.d-vnc-btn {
  height: 32px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 7px;
  padding: 0 12px;
  border-radius: 6px;
  border: 1px solid var(--bd);
  background: var(--bg-3);
  color: var(--tx-2);
  font-size: 11px;
  cursor: pointer;
  transition: all 0.12s;
}
.d-vnc-btn:hover { border-color: var(--tx-3); color: var(--tx); }
.d-vnc-btn.pri { background: var(--pri); color: var(--bg); border-color: var(--pri); }
.d-vnc-btn.pri:hover { background: var(--pri-h); }
.d-vnc-btn.icon-only {
  width: 34px;
  padding: 0;
}
.d-vnc-btn:disabled { opacity: 0.4; cursor: not-allowed; }
.d-vnc-tbl {
  width: 100%;
  border-collapse: collapse;
  font-size: 11px;
  overflow: hidden;
  border: 1px solid var(--bd);
  border-radius: 10px;
  background: var(--bg-2);
}
.d-vnc-tbl thead th {
  text-align: left;
  padding: 8px 12px;
  font-size: 10px;
  font-weight: 600;
  letter-spacing: 0.04em;
  color: var(--tx-3);
  border-bottom: 1px solid var(--bd);
}
.d-vnc-tbl tbody tr {
  border-bottom: 1px solid var(--bd);
  transition: background 0.1s;
}
.d-vnc-tbl tbody tr:hover { background: var(--bg-2); }
.d-vnc-tbl td { padding: 8px 12px; }
.d-vnc-acts { display: flex; gap: 6px; }
.d-vnc-acts button {
  height: 24px;
  padding: 0 10px;
  border-radius: 5px;
  border: 1px solid var(--bd);
  background: var(--bg-3);
  color: var(--tx-2);
  font-size: 10px;
  cursor: pointer;
  transition: all 0.12s;
}
.d-vnc-acts button:hover { border-color: var(--pri); color: var(--pri); }
.d-vnc-acts button.danger:hover { border-color: var(--red); color: var(--red); }
/* VNC listener controls */
.d-vnc-listener-bar { gap: 10px; }
.d-vnc-listener-status {
  display: flex; align-items: center; gap: 5px;
  font-size: 10px; font-weight: 700; letter-spacing: 0.06em; text-transform: uppercase;
}
.d-vnc-dot {
  width: 7px; height: 7px; border-radius: 50%; display: inline-block;
}
.vl-running .d-vnc-dot { background: var(--green); box-shadow: 0 0 6px var(--green); }
.vl-running { color: var(--green); }
.vl-stopped .d-vnc-dot { background: var(--tx-4); }
.vl-stopped { color: var(--tx-4); }
.d-vnc-listener-info {
  font-size: 11px; color: var(--tx-3); font-family: var(--font-mono);
}
.d-vnc-listener-info strong { color: var(--tx-2); font-weight: 600; }
.d-vnc-btn.danger { color: var(--red); border-color: color-mix(in srgb, var(--red) 40%, var(--bd)); }
.d-vnc-btn.danger:hover { border-color: var(--red); }
.d-chip-remote { color: var(--cyan); border-color: color-mix(in srgb, var(--cyan) 30%, var(--bd)); }
.d-empty {
  flex: 1;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 4px;
  color: var(--tx-4);
  font-size: 12px;
  border: 1px dashed var(--bd-2);
  border-radius: 12px;
  background: color-mix(in srgb, var(--bg-2) 55%, transparent);
}

/* ─── Utilities ─── */
.mono { font-family: var(--font-mono); }
.dim { color: var(--tx-3); }

/* ─── Overlay ─── */
.d-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.6);
  backdrop-filter: blur(3px);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1000;
}

/* ─── Animations ─── */
.spinning { animation: spin 1s linear infinite; }
@keyframes spin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }
</style>
