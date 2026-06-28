<template>
  <div class="table-container" @mousemove="onMouseMove" @mouseup="onMouseUp" @mouseleave="onMouseUp">
    <table class="beacon-table" :style="{ tableLayout: 'fixed', width: totalWidth }">
      <thead>
        <tr>
          <th class="c-chk"><input type="checkbox" @change="toggleAll"></th>
          <th v-for="col in columns" :key="col.key" 
              :class="['c-' + col.key, { resizing: resizer.activeKey === col.key, sorted: sortKey === col.key }]"
              :style="{ width: col.width + 'px' }">
            <div class="th-content">
              <button
                class="th-sort"
                type="button"
                :disabled="col.sortable === false"
                @click="toggleSort(col.key)"
                :title="col.sortable === false ? '' : `Sort by ${col.label}`"
              >
                <span>{{ col.label }}</span>
                <ArrowUp v-if="sortKey === col.key && sortDir === 'asc'" :size="11" />
                <ArrowDown v-else-if="sortKey === col.key && sortDir === 'desc'" :size="11" />
                <ArrowUpDown v-else-if="col.sortable !== false" :size="11" class="sort-idle" />
              </button>
              <div class="resize-handle" @mousedown.stop="onMouseDown($event, col.key)"></div>
            </div>
          </th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="beacon in sortedBeacons" :key="beacon.id" 
            :class="[heartbeatClass(beacon.last_seen), { selected: beacon.selected || activeBeaconId === beacon.id }]"
            @click="selectBeacon(beacon)"
            @contextmenu.prevent="openContextMenu($event, beacon)"
            @dblclick="$emit('interact', beacon)">
          <td class="c-chk"><input type="checkbox" v-model="beacon.selected"></td>
          <td class="c-led">
            <div class="led" :class="ledClass(beacon.last_seen)"></div>
          </td>
          <td class="c-id">
            <div class="id-wrapper">
              <Shield v-if="isAdmin(beacon.user)" :size="10" class="priv-icon" title="Admin/System" />
              {{ beacon.id }}
            </div>
          </td>
          <td class="c-iip">{{ beacon.internal_ip }}</td>
          <td class="c-eip">{{ beacon.external_ip || '-' }}</td>
          <td class="c-user">{{ beacon.user }}</td>
          <td class="c-comp">{{ beacon.computer }}</td>
          <td class="c-proc">
            <div class="proc-wrapper">
              <component :is="getProcIcon(beacon.process_name)" :size="12" class="proc-icon" />
              {{ beacon.process_name }}
            </div>
          </td>
          <td class="c-pid">{{ beacon.pid }}</td>
          <td class="c-seen">{{ formatLastSeen(beacon.last_seen) }}</td>
          <td class="c-os">
            <div class="os-wrapper">
              <img v-if="isWindowsOs(beacon.os_version)" :src="winIco" alt="Windows" class="os-icon" />
              <span>{{ beacon.os_version }}</span>
            </div>
          </td>
          <td class="c-online">{{ beacon.online_time || '-' }}</td>
        </tr>
        <tr v-if="sortedBeacons.length === 0">
          <td :colspan="columns.length + 1" class="empty-msg">No beacons matching filter</td>
        </tr>
      </tbody>
    </table>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, reactive } from 'vue'
import { useAppStore } from '../stores/app'
import { ArrowDown, ArrowUp, ArrowUpDown, Shield, Terminal, Globe, Cpu } from 'lucide-vue-next'
import { asset } from '../services/base'

const winIco = asset('/ico/Windows_ico.png')

const props = defineProps<{ filterText?: string }>()
const appStore = useAppStore()
const activeBeaconId = ref('')

// 权限检测逻辑
const isAdmin = (user: string) => {
  if (!user) return false
  const u = user.toLowerCase()
  return u.includes('admin') || u.includes('system') || u.includes('root') || u.endsWith('*')
}

// 进程图标映射
const getProcIcon = (name: string) => {
  if (!name) return Cpu
  const n = name.toLowerCase()
  if (n.includes('cmd') || n.includes('powershell')) return Terminal
  if (n.includes('chrome') || n.includes('explorer')) return Globe
  return Cpu
}

const filteredBeacons = computed(() => {
  if (!props.filterText) return appStore.beacons
  const s = props.filterText.toLowerCase()
  return appStore.beacons.filter(b => 
    String(b.computer || '').toLowerCase().includes(s) ||
    String(b.internal_ip || '').toLowerCase().includes(s) ||
    String(b.user || '').toLowerCase().includes(s) ||
    String(b.id || '').toLowerCase().includes(s)
  )
})

type SortDir = 'asc' | 'desc'
const sortKey = ref('')
const sortDir = ref<SortDir>('asc')
const collator = new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' })

const toggleSort = (key: string) => {
  const col = columns.value.find(c => c.key === key)
  if (col?.sortable === false) return
  if (sortKey.value === key) {
    sortDir.value = sortDir.value === 'asc' ? 'desc' : 'asc'
    return
  }
  sortKey.value = key
  sortDir.value = 'asc'
}

const ipv4ToNumber = (ip: unknown) => {
  const parts = String(ip || '').split('.')
  if (parts.length !== 4) return null
  let n = 0
  for (const part of parts) {
    const octet = Number(part)
    if (!Number.isInteger(octet) || octet < 0 || octet > 255) return null
    n = (n * 256) + octet
  }
  return n
}

const sortValue = (beacon: any, key: string): string | number => {
  switch (key) {
    case 'id': return String(beacon.id || '')
    case 'iip': return ipv4ToNumber(beacon.internal_ip) ?? String(beacon.internal_ip || '')
    case 'eip': return ipv4ToNumber(beacon.external_ip) ?? String(beacon.external_ip || '')
    case 'user': return String(beacon.user || '')
    case 'comp': return String(beacon.computer || '')
    case 'proc': return String(beacon.process_name || '')
    case 'pid': return Number(beacon.pid) || 0
    case 'seen': return Number(beacon.last_seen) || 0
    case 'os': return String(beacon.os_version || '')
    case 'online': return String(beacon.online_time || '')
    default: return String(beacon[key] || '')
  }
}

const compareValues = (a: string | number, b: string | number) => {
  if (typeof a === 'number' && typeof b === 'number') return a - b
  return collator.compare(String(a), String(b))
}

const sortedBeacons = computed(() => {
  if (!sortKey.value) return filteredBeacons.value
  const dir = sortDir.value === 'asc' ? 1 : -1
  return [...filteredBeacons.value].sort((a, b) => {
    const primary = compareValues(sortValue(a, sortKey.value), sortValue(b, sortKey.value))
    if (primary !== 0) return primary * dir
    return collator.compare(String(a.id || ''), String(b.id || ''))
  })
})

const STORAGE_KEY = 'ghost-beacon-table-widths'

interface ColumnDef {
  key: string
  label: string
  width: number
  sortable?: boolean
}

const defaultColumns: ColumnDef[] = [
  { key: 'led',   label: '',         width: 30, sortable: false },
  { key: 'id',    label: 'ID',       width: 100 },
  { key: 'iip',   label: 'Internal IP', width: 120 },
  { key: 'eip',   label: 'External IP', width: 120 },
  { key: 'user',  label: 'User',     width: 120 },
  { key: 'comp',  label: 'Computer', width: 140 },
  { key: 'proc',  label: 'Process',  width: 140 },
  { key: 'pid',   label: 'PID',      width: 80 },
  { key: 'seen',  label: 'Last Seen', width: 100 },
  { key: 'os',    label: 'OS / Arch', width: 180 },
  { key: 'online', label: 'Online Time', width: 150 },
]

const loadWidths = () => {
  try {
    const saved = localStorage.getItem(STORAGE_KEY)
    if (saved) {
      const widths = JSON.parse(saved)
      return defaultColumns.map(col => ({ ...col, width: widths[col.key] || col.width }))
    }
  } catch (e) {}
  return [...defaultColumns]
}

const columns = ref(loadWidths())
const totalWidth = computed(() => columns.value.reduce((acc, col) => acc + col.width, 40) + 'px')

const saveWidths = () => {
  const widths = columns.value.reduce((acc, col) => { acc[col.key] = col.width; return acc; }, {} as any)
  localStorage.setItem(STORAGE_KEY, JSON.stringify(widths))
}

const resizer = reactive({ activeKey: null as string | null, startX: 0, startWidth: 0 })
const onMouseDown = (e: MouseEvent, key: string) => {
  const col = columns.value.find(c => c.key === key)
  if (!col) return
  resizer.activeKey = key; resizer.startX = e.pageX; resizer.startWidth = col.width
  document.body.style.cursor = 'col-resize'; document.body.style.userSelect = 'none'
}
const onMouseMove = (e: MouseEvent) => {
  if (!resizer.activeKey) return
  const col = columns.value.find(c => c.key === resizer.activeKey)
  if (col) col.width = Math.max(20, resizer.startWidth + (e.pageX - resizer.startX))
}
const onMouseUp = () => { if (resizer.activeKey) saveWidths(); resizer.activeKey = null; document.body.style.cursor = ''; document.body.style.userSelect = '' }

const secondsAgo = (ts: number) => Math.floor(Date.now() / 1000 - (Number(ts) || 0))
const heartbeatClass = (ts: number) => {
  if (!ts) return 'hb-dead'
  const d = secondsAgo(ts)
  if (d < 30) return 'hb-live'
  if (d < 120) return 'hb-warm'
  if (d < 300) return 'hb-idle'
  return 'hb-dead'
}
const ledClass = (ts: number) => {
  if (!ts) return 'led-dead'
  const d = secondsAgo(ts)
  if (d < 30) return 'led-live'
  if (d < 300) return 'led-idle'
  return 'led-dead'
}
const formatLastSeen = (timestamp: number) => {
  if (!timestamp) return 'never'
  const s = secondsAgo(timestamp)
  if (s < 5) return 'now'
  if (s < 60) return `${s}s`
  if (s < 3600) {
    const m = Math.floor(s / 60)
    return `${m}m`
  }
  if (s < 86400) {
    const h = Math.floor(s / 3600)
    return `${h}h`
  }
  const d = Math.floor(s / 86400)
  return `${d}d`
}
const isWindowsOs = (os: string) => String(os || '').toLowerCase().includes('windows')
const toggleAll = (e: any) => {
  const checked = e.target.checked
  filteredBeacons.value.forEach((b: any) => b.selected = checked)
}

const selectBeacon = (beacon: any) => {
  activeBeaconId.value = beacon.id
}

const openContextMenu = (event: MouseEvent, beacon: any) => {
  activeBeaconId.value = beacon.id
  emit('contextmenu', event, beacon)
}

const emit = defineEmits(['interact', 'contextmenu'])
</script>

<style scoped>
.table-container { width: 100%; height: 100%; overflow: auto; background: var(--bg); }
.beacon-table { border-collapse: separate; border-spacing: 0; font-family: var(--font-mono); font-size: 11px; color: var(--tx); min-width: 100%; }
thead th {
  position: sticky;
  top: 0;
  background: color-mix(in srgb, var(--bg-3) 92%, var(--bg));
  border-bottom: 1px solid var(--bd);
  border-right: 1px solid var(--bd);
  color: var(--tx-2);
  text-align: left;
  padding: 0;
  height: 34px;
  z-index: 10;
  box-shadow: 0 8px 22px rgba(0, 0, 0, 0.14);
}
.th-content { display: flex; align-items: center; padding: 0 8px 0 10px; height: 100%; position: relative; }
.th-sort {
  min-width: 0;
  width: 100%;
  height: 100%;
  display: flex;
  align-items: center;
  gap: 5px;
  padding: 0 8px 0 0;
  background: transparent;
  border: 0;
  color: inherit;
  font: inherit;
  cursor: pointer;
  text-align: left;
}
.th-sort:disabled { cursor: default; }
.th-sort span { flex: 1; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; text-transform: uppercase; font-size: 10px; font-weight: 700; }
.th-sort svg { flex-shrink: 0; color: var(--pri); }
.th-sort .sort-idle { color: var(--tx-4); opacity: 0; transition: opacity .12s ease, color .12s ease; }
thead th:hover .sort-idle { opacity: .8; }
thead th.sorted { color: var(--tx); background: color-mix(in srgb, var(--bg-4) 84%, var(--bg)); }
.resize-handle { position: absolute; right: 0; top: 0; bottom: 0; width: 4px; cursor: col-resize; }
.resize-handle:hover, .resizing .resize-handle { background: var(--pri); }
tbody tr { transition: background-color .12s ease, box-shadow .12s ease; }
tbody tr.selected {
  background: color-mix(in srgb, var(--pri) 16%, var(--bg));
  box-shadow: inset 0 0 0 1px color-mix(in srgb, var(--pri) 48%, transparent), inset 3px 0 0 0 var(--pri);
}
tbody tr.hb-live { background: color-mix(in srgb, var(--green) 4%, transparent); }
tbody tr.hb-warm { background: color-mix(in srgb, var(--amber) 4%, transparent); }
tbody tr.hb-idle { background: color-mix(in srgb, var(--blue) 3%, transparent); }
tbody tr.selected.hb-live,
tbody tr.selected.hb-warm,
tbody tr.selected.hb-idle,
tbody tr.selected.hb-dead {
  background: color-mix(in srgb, var(--pri) 16%, var(--bg));
}
tbody tr:not(.selected):hover,
tbody tr.hb-live:not(.selected):hover,
tbody tr.hb-warm:not(.selected):hover,
tbody tr.hb-idle:not(.selected):hover,
tbody tr.hb-dead:not(.selected):hover {
  background: color-mix(in srgb, var(--blue) 13%, var(--bg));
  box-shadow: inset 2px 0 0 0 var(--blue);
}
td {
  height: 34px;
  padding: 7px 10px;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  border-right: 1px solid var(--bd);
  border-bottom: 1px solid var(--bd);
}
.c-chk { width: 32px; text-align: center; }
.led { width: 9px; height: 9px; border-radius: 50%; display: inline-block; border: 1px solid rgba(255,255,255,0.16); }
.led.led-live { background: var(--pri); box-shadow: 0 0 6px var(--pri); }
.led.led-idle { background: var(--amber); box-shadow: 0 0 4px var(--amber); }
.led.led-dead { background: var(--tx-4); }
.id-wrapper, .proc-wrapper { display: flex; align-items: center; gap: 6px; }
.os-wrapper { display: flex; align-items: center; gap: 6px; }
.os-icon { width: 12px; height: 12px; object-fit: contain; }
.priv-icon { color: var(--red); filter: drop-shadow(0 0 2px var(--red)); }
.proc-icon { color: var(--tx-3); }
.empty-msg { padding: 46px; text-align: center; color: var(--tx-3); font-family: var(--font-sans); }
thead th::after { content: ''; position: absolute; left: 0; bottom: 0; width: 100%; border-bottom: 1px solid var(--bd); }
</style>
