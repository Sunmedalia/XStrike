import { defineStore } from 'pinia'
import api from '../services/api'

// ── Manifest types ──

export interface PluginAction {
  type: string
  bof_name?: string
  plugin_name?: string
  panel_id?: string
  encode_type?: string
  args_map?: Record<string, number>
}

export interface PluginMenuItem {
  id: string
  label: string
  icon?: string
  action?: PluginAction
  children?: PluginMenuItem[]
}

export interface PluginMenu {
  id: string
  label: string
  location: string
  icon?: string
  children: PluginMenuItem[]
}

export interface PluginPanelField {
  name: string
  label: string
  type: string
  placeholder?: string
  required?: boolean
  options?: string[]
  default?: any
  rows?: number
  min?: number
  max?: number
  accept?: string
}

export interface PluginPanelLayout {
  type: string
  content?: string
  fields?: PluginPanelField[]
  submit?: {
    label: string
    action: PluginAction
  }
}

export interface PluginPanel {
  id: string
  title: string
  tab_location: string
  icon?: string
  layout: PluginPanelLayout
}

export interface PluginCommand {
  name: string
  aliases?: string[]
  description?: string
  args?: any[]
  action?: PluginAction
}

export interface PluginToolbarButton {
  id: string
  label: string
  icon?: string
  tooltip?: string
  action?: PluginAction
}

export interface PluginEventHook {
  event: string
  action?: PluginAction
}

export interface PluginManifest {
  name: string
  version: string
  author?: string
  description?: string
  min_ghost_version?: string
  menus?: PluginMenu[]
  panels?: PluginPanel[]
  commands?: PluginCommand[]
  toolbar_buttons?: PluginToolbarButton[]
  event_hooks?: PluginEventHook[]
  _plugin_id?: string
  _enabled?: boolean
}

export interface PluginInfo {
  id: string
  name: string
  category: string
  version: string
  author: string
  description: string
  enabled: boolean
  installed_at: number
  bof_count: number
  menu_count: number
  panel_count: number
  command_count: number
  bof_files: string[]
}

function annotateManifestActions(manifest: PluginManifest): PluginManifest {
  const annotate = (action?: PluginAction) => {
    if (action) action.plugin_name = manifest.name
  }
  const walkItem = (item: PluginMenuItem) => {
    annotate(item.action)
    item.children?.forEach(walkItem)
  }
  manifest.menus?.forEach(menu => menu.children?.forEach(walkItem))
  manifest.commands?.forEach(command => annotate(command.action))
  manifest.toolbar_buttons?.forEach(button => annotate(button.action))
  manifest.event_hooks?.forEach(hook => annotate(hook.action))
  manifest.panels?.forEach(panel => annotate(panel.layout.submit?.action))
  return manifest
}

export const usePluginStore = defineStore('plugins', {
  state: () => ({
    plugins: [] as PluginInfo[],
    manifests: [] as PluginManifest[],
    loading: false,
  }),
  getters: {
    enabledManifests(state): PluginManifest[] {
      return state.manifests
    },

    /** Get all menus for a specific location (e.g., 'beacon_context_menu') */
    menusForLocation() {
      return (location: string): PluginMenu[] => {
        const result: PluginMenu[] = []
        for (const m of this.enabledManifests) {
          if (m.menus) {
            for (const menu of m.menus) {
              if (menu.location === location) {
                result.push(menu)
              }
            }
          }
        }
        return result
      }
    },

    /** Get all panels for a specific tab location (e.g., 'agent_workspace') */
    panelsForLocation() {
      return (location: string): PluginPanel[] => {
        const result: PluginPanel[] = []
        for (const m of this.enabledManifests) {
          if (m.panels) {
            for (const panel of m.panels) {
              if (panel.tab_location === location) {
                result.push(panel)
              }
            }
          }
        }
        return result
      }
    },

    /** Get all toolbar buttons from all enabled plugins */
    allToolbarButtons(): PluginToolbarButton[] {
      const result: PluginToolbarButton[] = []
      for (const m of this.enabledManifests) {
        if (m.toolbar_buttons) {
          result.push(...m.toolbar_buttons)
        }
      }
      return result
    },

    /** Get all commands from all enabled plugins */
    allPluginCommands(): PluginCommand[] {
      const result: PluginCommand[] = []
      for (const m of this.enabledManifests) {
        if (m.commands) {
          result.push(...m.commands)
        }
      }
      return result
    },

    /** Get BOF files grouped by plugin name */
    bofFilesByPlugin(): Record<string, string[]> {
      const map: Record<string, string[]> = {}
      for (const p of this.plugins) {
        if (p.bof_files && p.bof_files.length > 0) {
          map[p.name] = p.bof_files
        }
      }
      return map
    },

    /** Get the plugin name that owns a given BOF file */
    pluginForBof() {
      return (bofName: string): string => {
        for (const p of this.plugins) {
          if (p.bof_files && p.bof_files.includes(bofName)) {
            return p.name
          }
        }
        return ''
      }
    },
  },
  actions: {
    async fetchPlugins() {
      try {
        const res = await api.get('/plugins', { silentError: true } as any)
        this.plugins = res.data.data || []
      } catch (err) {}
    },

    async fetchManifests() {
      try {
        const res = await api.get('/plugins/manifests', { silentError: true } as any)
        this.manifests = (res.data.data || []).map(annotateManifestActions)
      } catch (err) {}
    },

    async installPlugin(file: File): Promise<boolean> {
      const formData = new FormData()
      formData.append('file', file)
      try {
        const res = await api.post('/plugins/upload', formData)
        if (res.data.success) {
          await this.fetchPlugins()
          await this.fetchManifests()
          return true
        }
        return false
      } catch {
        return false
      }
    },

    async uninstallPlugin(id: string): Promise<boolean> {
      try {
        const res = await api.delete(`/plugins/${encodeURIComponent(id)}`)
        if (res.data.success) {
          await this.fetchPlugins()
          await this.fetchManifests()
          return true
        }
        return false
      } catch {
        return false
      }
    },

    async togglePlugin(id: string): Promise<boolean> {
      try {
        const res = await api.put(`/plugins/${encodeURIComponent(id)}/toggle`)
        if (res.data.success) {
          await this.fetchPlugins()
          await this.fetchManifests()
          return true
        }
        return false
      } catch {
        return false
      }
    },

    async refreshAll() {
      this.loading = true
      await Promise.all([this.fetchPlugins(), this.fetchManifests()])
      this.loading = false
    },
  },
})
