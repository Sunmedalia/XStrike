<template>
  <div class="file-browser">
    <div class="browser-toolbar">
      <button class="icon-btn" @click="goUp" :disabled="!hasParent"><ChevronUp :size="14" /></button>
      <div class="path-input">
        <Folder :size="12" class="path-icon" />
        <input v-model="path" @keydown.enter="onPathEnter" />
      </div>
      <button class="btn" @click="refreshCurrentView(false)" :disabled="loading">
        <RefreshCw :size="12" :class="{ spinning: loading }" /> Refresh
      </button>
    </div>

    <div class="browser-content">
      <!-- Left: Parent directory contents -->
      <div class="parent-dir-panel">
        <div class="section-header">
          <FolderOpen :size="12" />
          <span>{{ parentDirName }}</span>
        </div>
        <div class="file-list">
          <div
            v-for="file in parentFiles"
            :key="file.name"
            class="file-item"
            :class="{ active: isCurrentDir(file), selected: isSelectedParentFile(file), 'is-dir': isDirEntry(file) }"
            @click="onParentFileClick(file)"
          >
            <Folder v-if="isDirEntry(file)" :size="14" class="item-icon dir-icon" />
            <FileIcon v-else :size="14" class="item-icon file-icon-svg" />
            <span class="item-name">{{ file.name }}</span>
          </div>
          <div v-if="parentFiles.length === 0 && !loadingParent" class="empty-panel">
            <template v-if="parentError">parent: {{ parentError }}</template>
            <template v-else>Root directory</template>
          </div>
        </div>
      </div>

      <!-- Right: Current directory contents -->
      <div class="current-dir-panel">
        <div class="section-header">
          <Folder :size="12" />
          <span>{{ currentDirName }}</span>
          <div class="name-filter">
            <input v-model="nameFilter" placeholder="filter…" :disabled="loading" />
          </div>
        </div>
        <div class="file-list-container">
          <table class="file-table">
            <thead>
              <tr>
                <th style="width: 30px"></th>
                <th>Name</th>
                <th style="width: 100px">Size</th>
                <th style="width: 160px">Modified</th>
                <th style="width: 80px">Actions</th>
              </tr>
            </thead>
            <tbody>
              <tr
                v-for="file in filteredFiles"
                :key="file.name"
                :class="{ selected: isSelectedFile(file) }"
                @click="onFileClick(file)"
              >
                <td class="file-icon">
                  <Folder v-if="isDirEntry(file)" :size="14" class="dir-icon" />
                  <FileIcon v-else :size="14" class="file-icon-svg" />
                </td>
                <td class="file-name" :class="{ 'is-dir': isDirEntry(file) }">{{ file.name }}</td>
                <td class="file-size">{{ isDirEntry(file) ? '-' : formatSize(file.size || 0) }}</td>
                <td class="file-date">{{ file.modified || '-' }}</td>
                <td class="file-actions">
                  <button v-if="!isDirEntry(file)" class="action-btn" @click.stop="downloadFile(file)" title="Download"><Download :size="12" /></button>
                  <button class="action-btn delete" @click.stop="confirmDeleteFile(file)" title="Delete"><Trash2 :size="12" /></button>
                </td>
              </tr>
              <tr v-if="hiddenCount > 0">
                <td colspan="5" class="more-row">…and {{ hiddenCount }} more (use the filter to narrow)</td>
              </tr>
              <tr v-if="files.length === 0 && !loading">
                <td colspan="5" class="empty">No files or path not yet explored</td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, watch } from 'vue'
import { Folder, FolderOpen, File as FileIcon, ChevronUp, RefreshCw, Download, Trash2 } from 'lucide-vue-next'
import api from '../services/api'
import { useToastStore } from '../stores/toast'
import { useAppStore } from '../stores/app'
import { useModalStore } from '../stores/modal'
import ConfirmModal from './ConfirmModal.vue'
import { encodeBeaconString } from '../services/bofEncoding'
import { findBofByPattern, runBofTask } from '../services/tasks'
import {
  formatFileSize,
  getParentPath,
  getPathDisplayName,
  hasParentPath,
  isDirectoryEntry,
  normalizeWindowsPath,
  parseFileListOutput as parseFileListModel,
  type FileEntry,
} from '../services/fileBrowserModel'

type FileBrowserCache = { path: string; files: FileEntry[]; parentFiles?: FileEntry[] }
const fileBrowserCache: Map<string, FileBrowserCache> = (() => {
  const g = globalThis as any
  if (!g.__ghostFileBrowserCache) {
    g.__ghostFileBrowserCache = new Map<string, FileBrowserCache>()
  }
  return g.__ghostFileBrowserCache as Map<string, FileBrowserCache>
})()

const props = defineProps<{ targetId: string }>()
const toast = useToastStore()
const appStore = useAppStore()
const modalStore = useModalStore()

const path = ref('C:\\')
const files = ref<FileEntry[]>([])
const loading = ref(false)
const parentFiles = ref<FileEntry[]>([])
const loadingParent = ref(false)
const selectedFileName = ref('')
const selectedParentFileName = ref('')
// The directory whose contents are CURRENTLY rendered in `files`. Click/download
// navigation resolves against THIS (not `path.value`, which can race ahead during
// an in-flight load). Fixes the "click a sibling → it gets concatenated as a
// child of the just-clicked dir" bug: while a refresh is in flight the right
// panel still shows the old dir's rows, so a click must base off the old dir.
const displayedPath = ref('C:\\')
// Supersede tokens: each refresh/parent-load bumps its token; a stale poll that
// resolves after a newer one started is dropped (last-click-wins). Fixes the
// "click sometimes no response" case where a slow old poll overwrote a newer
// fast one.
const loadToken = ref(0)
const parentLoadToken = ref(0)
const suppressParentWatch = ref(false)
// Last parent path we actually fetched. Clicking a sibling dir (same parent)
// must NOT re-fire loadParentDir — that re-poll, if superseded, blanks the left
// panel (the "无法联动" bug). We keep the existing left panel and just let the
// isCurrentDir highlight recompute.
const loadedParentPath = ref('')
// Client-side name filter for the right panel — large dirs (System32 ~3500
// entries) lag the DOM if rendered raw. Filter narrows the rendered set.
const nameFilter = ref('')
// Cap rendered rows so a giant directory doesn't freeze the panel.
const RENDER_CAP = 500
const parentError = ref('')

// Get parent directory path
const parentPath = computed(() => getParentPath(path.value))

// Check if has parent directory
const hasParent = computed(() => hasParentPath(path.value))

// Get parent directory name
const parentDirName = computed(() => {
  if (!hasParent.value) return 'Root'
  return getPathDisplayName(parentPath.value)
})

// Get current directory name
const currentDirName = computed(() => getPathDisplayName(path.value))

const isDirEntry = isDirectoryEntry

// Check if a file in parent dir is the current directory
const isCurrentDir = (file: FileEntry) => {
  if (!isDirEntry(file)) return false
  const currentName = currentDirName.value
  return file.name === currentName
}

const isSelectedFile = (file: FileEntry) => file.name === selectedFileName.value
const isSelectedParentFile = (file: FileEntry) => file.name === selectedParentFileName.value && !isCurrentDir(file)

// Right-panel rows after the client-side name filter, capped to RENDER_CAP so a
// huge directory (System32) doesn't freeze the DOM. `files` keeps the full set
// (cache intact); `hiddenCount` powers a "...and N more" footer.
const filteredFiles = computed(() => {
  const q = nameFilter.value.trim().toLowerCase()
  const base = q
    ? files.value.filter(f => f.name.toLowerCase().includes(q))
    : files.value
  return base.slice(0, RENDER_CAP)
})
const hiddenCount = computed(() => {
  const q = nameFilter.value.trim().toLowerCase()
  const total = q
    ? files.value.filter(f => f.name.toLowerCase().includes(q)).length
    : files.value.length
  return Math.max(0, total - RENDER_CAP)
})

// Watch path changes to load the parent directory. Only fires on real value
// changes (Vue string ===), so the CWD writeback in refreshFiles(canonicalize)
// doesn't re-fire when the BOF agrees with the path we already set.
// Skip the BOF re-fetch when the parent didn't change (sibling click) — that
// re-poll, if superseded, blanks the left panel (the "无法联动" bug). The
// isCurrentDir highlight recomputes off currentDirName either way.
watch(path, () => {
  if (suppressParentWatch.value) return
  if (hasParent.value) {
    if (parentPath.value !== loadedParentPath.value) {
      loadParentDir()
    }
  } else {
    parentFiles.value = []
    loadedParentPath.value = ''
  }
}, { immediate: false })

const getInitialPathFromBeacon = () => {
  const beacon = appStore.beacons.find((b: any) => (b.node_id || b.id) === props.targetId)
  const proc = String(beacon?.process || beacon?.process_name || '')
  if (!proc) return 'C:\\'
  const normalized = proc.replace(/\//g, '\\')
  const idx = normalized.lastIndexOf('\\')
  if (idx > 1) return normalized.slice(0, idx + 1)
  return 'C:\\'
}

const findBof = (pattern: RegExp) => findBofByPattern(appStore.bofs, pattern)

const updateCache = () => {
  fileBrowserCache.set(props.targetId, {
    path: path.value,
    files: [...files.value],
    parentFiles: [...parentFiles.value]
  })
}

const formatSize = formatFileSize

const loadParentDir = async () => {
  if (!hasParent.value) {
    parentFiles.value = []
    loadedParentPath.value = ''
    return
  }

  // Same parent as last time? Keep the existing left panel — just let the
  // isCurrentDir highlight recompute. Avoids a needless re-poll that, if
  // superseded, would blank the panel.
  if (parentPath.value === loadedParentPath.value && parentFiles.value.length) {
    return
  }

  // Check cache first
  const parentCacheKey = `${props.targetId}:${parentPath.value}`
  const cached = fileBrowserCache.get(parentCacheKey)
  if (cached && cached.files) {
    parentFiles.value = [...cached.files]
    loadedParentPath.value = parentPath.value
    parentError.value = ''
    return
  }

  // Supersede stale parent loads: a rapid nav can start several of these; only
  // the most recent one's RESULT should land in parentFiles. loadingParent must
  // always clear (in finally, unguarded) so the panel never gets stuck blank.
  const myToken = ++parentLoadToken.value
  loadingParent.value = true
  parentError.value = ''
  try {
    const fileListBof = findBof(/^file_list\b/i)
    if (!fileListBof) return

    const result = await runBofTask({
      nodeId: props.targetId,
      bof: fileListBof,
      args: encodeBeaconString(parentPath.value)
    }, { maxRetry: 100, intervalMs: 250 })
    if (myToken !== parentLoadToken.value) return   // a newer nav superseded us
    if (result.success) {
      parentFiles.value = applyFileListOutput(result.output || '', true)
      loadedParentPath.value = parentPath.value
      // Cache parent directory
      fileBrowserCache.set(parentCacheKey, {
        path: parentPath.value,
        files: [...parentFiles.value]
      })
    } else {
      parentError.value = result.error || 'parent list failed'
    }
  } catch (err: any) {
    // Surface the failure instead of silently blanking the left panel — the
    // "无法联动" symptom was a swallowed error + stuck loading flag.
    if (myToken === parentLoadToken.value) {
      parentError.value = err?.message || 'parent load failed'
    }
  } finally {
    // ALWAYS clear the flag — a guarded reset left loadingParent stuck on
    // after a superseded poll, which kept the left panel blank forever.
    if (myToken === parentLoadToken.value) loadingParent.value = false
  }
}

// `canonicalize = true` lets the BOF's `CWD:` header write back to the path
// bar — used for the initial mount and the manual Enter-in-path case where we
// want the typed string canonicalized (`c:\windows` → `C:\Windows\`). For
// click-driven navigations we pass `false` so the BOF's CWD doesn't clobber the
// path the user just chose (which dropped the trailing slash and re-fired the
// path watch → double loadParentDir → the "loading broken" symptom).
const refreshFiles = async (canonicalize = false) => {
  loading.value = true
  // Supersede stale loads: rapid clicks start several refreshes; only the
  // most recent one's result should land in `files`.
  const myToken = ++loadToken.value
  try {
    const fileListBof = findBof(/^file_list\b/i)
    if (!fileListBof) {
      toast.error('No file_list BOF found. Upload file_list.o first.')
      return
    }

    // Snapshot the path we're loading so a later nav doesn't make us apply
    // this result to the wrong directory.
    const targetPath = path.value
    const result = await runBofTask({
      nodeId: props.targetId,
      bof: fileListBof,
      args: encodeBeaconString(targetPath),
      source: 'file:list',
      auditInput: targetPath
    }, { maxRetry: 100, intervalMs: 250 })
    if (myToken !== loadToken.value) return   // a newer click supersed us
    if (!result.success) {
      toast.error(result.error || 'File list failed')
      return
    }
    // When canonicalizing, let the CWD header write the (normalized) path
    // back; when click-driven, skip the writeback so the BOF's CWD doesn't
    // clobber the path the user just chose. Vue's watch only fires on real
    // value changes (string ===), and loadParentDir is cache-served +
    // token-guarded, so the parent load is naturally single and non-racing.
    files.value = applyFileListOutput(result.output || '', !canonicalize)
    // Record the directory these rows belong to, so click/download navigation
    // resolves against what's actually displayed (path.value may have raced
    // ahead if the user clicked again mid-load).
    displayedPath.value = path.value
    updateCache()
    // Parent dir is loaded by watch(path) for free path edits, and by
    // navigateToPath/refreshCurrentView for controlled navigation.
  } catch (err: any) {
    if (myToken === loadToken.value) toast.error(err.message || 'Failed to list files')
  } finally {
    if (myToken === loadToken.value) loading.value = false
  }
}

const refreshCurrentView = async (canonicalize = false) => {
  parentLoadToken.value++
  await refreshFiles(canonicalize)
  await loadParentDir()
}

const navigateToPath = async (nextPath: string, canonicalize = false) => {
  suppressParentWatch.value = true
  parentLoadToken.value++
  loadingParent.value = false
  parentError.value = ''
  path.value = normalizeWindowsPath(nextPath)
  files.value = []
  try {
    await refreshFiles(canonicalize)
    await loadParentDir()
  } finally {
    suppressParentWatch.value = false
  }
}

/** Enter-in-path: canonicalize (let the BOF normalize the typed string). */
const onPathEnter = () => {
  navigateToPath(path.value, true)
}

const applyFileListOutput = (output: string, skipPathUpdate = false) => {
  const parsed = parseFileListModel(output)
  if (parsed.cwd && !skipPathUpdate) path.value = normalizeWindowsPath(parsed.cwd)
  return parsed.rows
}

const goUp = () => {
  if (!hasParent.value) return
  selectedFileName.value = ''
  selectedParentFileName.value = ''
  navigateToPath(parentPath.value, false)
}

const navigateFromBase = (basePath: string, file: FileEntry) => {
  if (!isDirEntry(file)) return
  const resolvedBase = basePath || displayedPath.value || path.value || 'C:\\'
  const base = resolvedBase.endsWith('\\') ? resolvedBase : resolvedBase + '\\'
  const nextPath = normalizeWindowsPath(base + file.name)
  if (nextPath === path.value && !loading.value) return
  selectedFileName.value = ''
  selectedParentFileName.value = ''
  nameFilter.value = ''
  navigateToPath(nextPath, false)
}

const onFileClick = (file: FileEntry) => {
  selectedFileName.value = file.name
  selectedParentFileName.value = ''
  if (isDirEntry(file)) {
    // Base off the directory actually displayed (whose rows are visible), NOT
    // path.value — path.value may have already advanced from a prior click whose
    // load is still in flight, which would concatenate this row as a child of
    // the wrong (just-clicked) dir. Clearing files gives immediate feedback
    // that the click registered (no stale rows to misclick).
    navigateFromBase(displayedPath.value, file)
  }
}

const onParentFileClick = (file: FileEntry) => {
  selectedParentFileName.value = file.name
  selectedFileName.value = ''
  if (isDirEntry(file)) {
    // The parent panel shows loadedParentPath's contents; base off that.
    navigateFromBase(loadedParentPath.value || parentPath.value, file)
  }
}

const downloadFile = async (file: FileEntry) => {
  const base = displayedPath.value.endsWith('\\') ? displayedPath.value : displayedPath.value + '\\'
  const fullPath = base + file.name
  try {
    const fileDownloadBof = findBof(/^file_download\b/i)
    if (!fileDownloadBof) {
      toast.error('No file_download BOF found. Upload file_download.o first.')
      return
    }

    const result = await runBofTask({
      nodeId: props.targetId,
      bof: fileDownloadBof,
      args: encodeBeaconString(fullPath),
      source: 'file:download',
      auditInput: fullPath
    }, { maxRetry: 120 })
    if (!result.success) {
      toast.error(result.error || 'Download failed')
      return
    }

    let content = result.output || ''
    const firstLineIdx = content.indexOf('\n')
    if (firstLineIdx >= 0 && content.slice(0, firstLineIdx).startsWith('=== FILE:')) {
      content = content.slice(firstLineIdx + 1)
    }
    const bin = atob(content)
    const bytes = new Uint8Array(bin.length)
    for (let i = 0; i < bin.length; i++) {
      bytes[i] = bin.charCodeAt(i)
    }
    const blob = new Blob([bytes], { type: 'application/octet-stream' })
    const link = document.createElement('a')
    link.href = URL.createObjectURL(blob)
    link.download = file.name
    link.click()
    URL.revokeObjectURL(link.href)
    toast.success(`Downloaded ${file.name}`)
  } catch (err: any) {
    toast.error(err.message || 'Download failed')
  }
}

const deleteFile = async (file: FileEntry) => {
  const base = displayedPath.value.endsWith('\\') ? displayedPath.value : displayedPath.value + '\\'
  const fullPath = base + file.name
  try {
    const cmdBof = findBof(/^cmd_exec\b/i)
    if (!cmdBof) {
      toast.error('No cmd_exec BOF found. Upload cmd_exec.o first.')
      return
    }
    const command = isDirEntry(file) ? `rmdir /s /q "${fullPath}"` : `del /f /q "${fullPath}"`
    const result = await runBofTask({
      nodeId: props.targetId,
      bof: cmdBof,
      args: encodeBeaconString(command),
      source: 'file:delete',
      auditInput: command
    }, { maxRetry: 60 })
    if (!result.success) {
      toast.error(result.error || 'Delete failed')
      return
    }
    toast.success(`Deleted ${file.name}`)
    await refreshFiles(false)
  } catch (err: any) {
    toast.error(err.message || 'Delete failed')
  }
}

const confirmDeleteFile = (file: FileEntry) => {
  modalStore.open(ConfirmModal, {
    title: 'Delete File',
    message: `Delete "${file.name}" on target ${props.targetId}?`,
    confirmText: 'Delete',
    type: 'danger',
    onResolve: async () => {
      await deleteFile(file)
    }
  })
}

// Hydrate the last file listing from the persisted store (survives restart).
// Only used when there's no in-memory cache and a live refresh didn't yield
// rows (e.g. the implant is offline). Restores the last-browsed directory.
const hydrateFileListFromDb = async () => {
  const id = Number(props.targetId)
  if (!id) return
  try {
    const res = await api.get(`/agents/${id}/artifacts`, { params: { kind: 'file_list', limit: 1 }, silentError: true } as any)
    const arts: any[] = res.data?.data || []
    if (!arts.length) return
    const meta = arts[0].meta
    if (!meta) return
    // applyFileListOutput(canonicalize=true) sets path from the CWD: line.
    const rows = applyFileListOutput(meta, false)
    if (rows.length || path.value) {
      files.value = rows
      displayedPath.value = path.value
      updateCache()
    }
  } catch { /* silent */ }
}

onMounted(async () => {
  const cached = fileBrowserCache.get(props.targetId)
  if (cached) {
    path.value = cached.path
    files.value = [...cached.files]
    displayedPath.value = cached.path
    if (cached.parentFiles) {
      parentFiles.value = [...cached.parentFiles]
      loadedParentPath.value = parentPath.value
    }
  } else {
    path.value = normalizeWindowsPath(getInitialPathFromBeacon())
    displayedPath.value = path.value
    // Canonicalize on first load so the BOF's CWD can correct the initial path.
    await navigateToPath(path.value, true)
    // If the live refresh came up empty (implant offline), hydrate the last
    // listing from the persisted store so the operator sees prior context.
    if (!files.value.length) {
      await hydrateFileListFromDb()
    }
  }
  window.addEventListener('ghost:sync', onSync as EventListener)
})
onUnmounted(() => {
  window.removeEventListener('ghost:sync', onSync as EventListener)
})

const onSync = async () => {
  await refreshCurrentView(false)
}
</script>

<style scoped>
.file-browser { display: flex; flex-direction: column; height: 100%; background: var(--bg); }

/* Toolbar */
.browser-toolbar { height: 36px; display: flex; align-items: center; gap: 8px; padding: 0 12px; background: var(--bg-2); border-bottom: 1px solid var(--bd); }
.icon-btn { display: flex; align-items: center; justify-content: center; width: 28px; height: 24px; border-radius: 3px; border: 1px solid var(--bd); background: var(--bg-3); color: var(--tx-2); cursor: pointer; transition: all 0.15s; }
.icon-btn:hover:not(:disabled) { background: var(--bg-4); color: var(--tx); }
.icon-btn:disabled { opacity: 0.4; cursor: not-allowed; }
.path-input { flex: 1; display: flex; align-items: center; background: var(--bg-3); border: 1px solid var(--bd); border-radius: 4px; padding: 0 8px; height: 24px; }
.path-icon { color: var(--amber); margin-right: 8px; }
.path-input input { background: transparent; border: none; color: var(--tx); font-size: 11px; font-family: var(--font-mono); width: 100%; outline: none; }
.btn { display: flex; align-items: center; gap: 6px; padding: 4px 10px; border-radius: 3px; border: 1px solid var(--bd); background: var(--bg-3); color: var(--tx-2); cursor: pointer; font-size: 11px; transition: all 0.15s; }
.btn:hover:not(:disabled) { background: var(--bg-4); color: var(--tx); }
.btn:disabled { opacity: 0.4; cursor: not-allowed; }

/* Main content: left-right layout */
.browser-content { display: flex; flex: 1; overflow: hidden; }

/* Left panel: parent directory */
.parent-dir-panel { width: 280px; border-right: 1px solid var(--bd); display: flex; flex-direction: column; background: var(--bg-2); }
.section-header { display: flex; align-items: center; gap: 8px; padding: 10px 12px; background: var(--bg-3); border-bottom: 1px solid var(--bd); font-size: 11px; font-weight: 600; color: var(--tx); }
.name-filter { margin-left: auto; }
.name-filter input { background: var(--bg-4); border: 1px solid var(--bd); color: var(--tx); border-radius: 3px; padding: 2px 6px; font-size: 11px; font-family: var(--font-mono); width: 140px; outline: none; }
.name-filter input:focus { border-color: var(--pri); }
.more-row td { text-align: center; padding: 8px; color: var(--tx-4); font-size: 10px; font-style: italic; }
.file-list { flex: 1; overflow-y: auto; padding: 4px; }
.file-item { display: flex; align-items: center; gap: 8px; padding: 6px 10px; cursor: pointer; font-size: 11px; color: var(--tx-2); transition: all 0.15s; border-radius: 4px; margin-bottom: 2px; }
.file-item:hover { background: var(--bg-3); color: var(--tx); }
.file-item.active { background: var(--bg-4); color: var(--tx); font-weight: 600; border: 1px solid var(--pri); }
.file-item.selected:not(.active) { background: color-mix(in srgb, var(--pri) 12%, var(--bg-3)); color: var(--tx); box-shadow: inset 0 0 0 1px color-mix(in srgb, var(--pri) 34%, transparent); }
.file-item.is-dir { color: var(--blue); }
.file-item.is-dir.active { color: var(--pri); }
.item-icon { flex-shrink: 0; }
.file-item .dir-icon { color: var(--amber); }
.file-item .file-icon-svg { color: var(--tx-3); }
.item-name { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-family: var(--font-mono); }
.empty-panel { padding: 20px; text-align: center; color: var(--tx-4); font-size: 11px; }

/* Right panel: current directory */
.current-dir-panel { flex: 1; display: flex; flex-direction: column; background: var(--bg); }
.file-list-container { flex: 1; overflow-y: auto; }
.file-table { width: 100%; border-collapse: collapse; font-size: 11px; font-family: var(--font-mono); }
.file-table th { text-align: left; padding: 8px 12px; background: var(--bg-3); position: sticky; top: 0; color: var(--tx-3); text-transform: uppercase; font-size: 10px; z-index: 1; border-bottom: 1px solid var(--bd); }
.file-table td { padding: 6px 12px; border-bottom: 1px solid var(--bd); vertical-align: middle; }
.file-table tbody tr:hover { background: var(--bg-4); cursor: pointer; }
.file-table tbody tr.selected { background: color-mix(in srgb, var(--pri) 12%, transparent); }
.file-table tbody tr.selected:hover { background: color-mix(in srgb, var(--pri) 18%, transparent); }
.file-icon { color: var(--tx-3); }
.file-table .dir-icon { color: var(--amber); }
.file-icon-svg { color: var(--tx-3); }
.is-dir { color: var(--blue); font-weight: 600; }
.file-size, .file-date { color: var(--tx-3); }
.file-actions { display: flex; gap: 4px; }
.action-btn { background: transparent; border: 1px solid var(--bd); color: var(--tx-3); padding: 4px; border-radius: 3px; cursor: pointer; transition: all 0.15s; }
.action-btn:hover { border-color: var(--tx-2); color: var(--tx); }
.action-btn.delete:hover { border-color: var(--red); color: var(--red); }
.empty { text-align: center; padding: 40px; color: var(--tx-4); }

/* Animations */
.spinning { animation: spin 1s linear infinite; }
@keyframes spin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }
</style>
