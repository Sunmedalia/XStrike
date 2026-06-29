import { computed, ref } from 'vue'
import type { Component } from 'vue'
import type { PluginPanelLayout } from '../stores/plugin'

export type AgentView = 'cmd' | 'powershell' | 'files' | 'processes' | 'persistence' | 'shellcode' | 'screenshots'
export type PersistenceMode = 'schtask' | 'service' | 'critical' | 'user'

export interface TabDef {
  id: string
  label: string
  icon?: Component
  closable?: boolean
  type?: 'agent' | 'vnc' | 'plugin-panel'
  beaconId?: string
  nodeId?: string
  initialView?: AgentView
  persistenceMode?: PersistenceMode
  panelLayout?: PluginPanelLayout
}

export interface AgentTabInput {
  beaconId: string
  label: string
  initialView: AgentView
  persistenceMode?: PersistenceMode
}

export interface PluginPanelTabInput {
  panelId: string
  label: string
  beaconId?: string
  layout: PluginPanelLayout
}

export function useDashboardTabs(fixedTabs: TabDef[]) {
  const activeTab = ref('console')
  const dynTabs = ref<TabDef[]>([])
  const allTabs = computed<TabDef[]>(() => [
    ...fixedTabs,
    ...dynTabs.value,
  ])

  const openAgentTab = (input: AgentTabInput) => {
    const tabId = `beacon-${input.beaconId}`
    const existing = dynTabs.value.find(t => t.id === tabId)
    if (!existing) {
      dynTabs.value.push({
        id: tabId,
        label: input.label,
        type: 'agent',
        closable: true,
        beaconId: input.beaconId,
        initialView: input.initialView,
        persistenceMode: input.persistenceMode || 'schtask',
      })
    } else {
      existing.initialView = input.initialView
      if (input.initialView === 'persistence') {
        existing.persistenceMode = input.persistenceMode || 'schtask'
      }
    }
    activeTab.value = tabId
  }

  const openVncTab = (nodeId: string) => {
    const nid = nodeId.trim()
    if (!nid) return false
    const tabId = `vnc-${nid}`
    if (!dynTabs.value.find(t => t.id === tabId)) {
      dynTabs.value.push({
        id: tabId,
        label: `VNC:${nid.slice(0, 8)}`,
        type: 'vnc',
        closable: true,
        nodeId: nid,
      })
    }
    activeTab.value = tabId
    return true
  }

  const openPluginPanelTab = (input: PluginPanelTabInput) => {
    const tabId = `plugin-${input.panelId}`
    if (!dynTabs.value.find(t => t.id === tabId)) {
      dynTabs.value.push({
        id: tabId,
        label: input.label,
        type: 'plugin-panel',
        closable: true,
        beaconId: input.beaconId,
        panelLayout: input.layout,
      })
    }
    activeTab.value = tabId
  }

  const isClosable = (id: string) => !fixedTabs.some(f => f.id === id)

  const closeTab = (id: string) => {
    if (activeTab.value === id) {
      const idx = allTabs.value.findIndex(t => t.id === id)
      const next = allTabs.value[idx + 1] || allTabs.value[idx - 1] || allTabs.value[0]
      activeTab.value = next?.id ?? 'console'
    }
    dynTabs.value = dynTabs.value.filter(t => t.id !== id)
  }

  return {
    activeTab,
    dynTabs,
    allTabs,
    openAgentTab,
    openVncTab,
    openPluginPanelTab,
    isClosable,
    closeTab,
  }
}
