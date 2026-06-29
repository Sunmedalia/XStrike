<template>
  <div class="screenshot-viewer">
    <div class="toolbar">
      <button @click="captureScreenshot" :disabled="loading" class="btn primary">
        <Camera :size="14" /> {{ loading ? 'Capturing...' : 'Capture Screenshot' }}
      </button>
      <span v-if="screenshots.length > 0" class="count">{{ screenshots.length }}/8 screenshot(s)</span>
    </div>

    <div v-if="screenshots.length === 0 && !loading" class="empty-state">
      <Camera :size="48" />
      <p>No screenshots yet</p>
      <p class="hint">Click "Capture Screenshot" to take a screenshot of the agent's desktop</p>
    </div>

    <div v-if="screenshots.length > 0" class="screenshots-grid">
      <div v-for="shot in screenshots" :key="shot.id" class="screenshot-item">
        <img :src="shot.thumbnail" @click="viewFullscreen(shot)" alt="Screenshot" />
        <div class="screenshot-info">
          <span class="timestamp">{{ shot.timestamp }}</span>
          <span class="resolution">{{ shot.width }}x{{ shot.height }}</span>
          <button @click.stop="saveScreenshot(shot)" class="icon-btn" title="Save Screenshot">
            <Download :size="14" />
          </button>
        </div>
      </div>
    </div>

    <div v-if="fullscreenImage" class="fullscreen-overlay" @click="closeFullscreen">
      <img :src="fullscreenImage" @click.stop />
      <button class="close-fullscreen" @click="closeFullscreen">
        <X :size="24" />
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, watch } from 'vue'
import { Camera, Download, X } from 'lucide-vue-next'
import { useToastStore } from '../stores/toast'
import { useAppStore } from '../stores/app'
import api from '../services/api'
import { findBofByPattern, runBofTask } from '../services/tasks'

interface Screenshot {
  id: number
  timestamp: string
  width: number
  height: number
  fullImage: string
  thumbnail: string
}

const props = defineProps<{
  agent: { id: string }
}>()

const SCREENSHOT_LIMIT = 8
const screenshotCache: Map<string, Screenshot[]> = (() => {
  const g = globalThis as any
  if (!g.__ghostScreenshotCache) {
    g.__ghostScreenshotCache = new Map<string, Screenshot[]>()
  }
  return g.__ghostScreenshotCache as Map<string, Screenshot[]>
})()

const toast = useToastStore()
const appStore = useAppStore()
const loading = ref(false)
const screenshots = ref<Screenshot[]>([])
const fullscreenImage = ref<string | null>(null)

const loadShots = (agentId: string) => {
  screenshots.value = [...(screenshotCache.get(agentId) || [])]
}

const saveShots = (agentId: string) => {
  screenshotCache.set(agentId, [...screenshots.value])
}

const captureScreenshot = async () => {
  loading.value = true
  try {
    // 先刷新 BOF 列表
    await appStore.fetchBofs()

    const screenshotBof = findBofByPattern(appStore.bofs, /^screenshot/i)

    if (!screenshotBof) {
      toast.error('No screenshot BOF found. Upload screenshot.o first.')
      return
    }

    const taskResult = await runBofTask({
      nodeId: props.agent.id,
      bof: screenshotBof,
      source: 'screenshot:capture',
      auditInput: '(screenshot)'
    }, { maxRetry: 120 })
    if (!taskResult.success || !taskResult.output) {
      toast.error(`Task failed: ${taskResult.error || 'No output received'}`)
      return
    }

    await parseScreenshot(taskResult.output)
    await appStore.fetchLogs()
    toast.success('Screenshot captured successfully')
  } catch (err: any) {
    toast.error(`Screenshot capture failed: ${err.message || err}`)
  } finally {
    loading.value = false
  }
}

const parseScreenshot = async (output: string) => {
  try {
    const match = output.match(/=== SCREENSHOT: (\d+)x(\d+) ===\n([\s\S]+)/)
    if (!match) {
      toast.error('Invalid screenshot format')
      return
    }

    const [, width, height, base64Data] = match
    const cleanBase64 = base64Data.trim()
    const fullImage = `data:image/bmp;base64,${cleanBase64}`
    const thumbnail = await generateThumbnail(fullImage, 300, 200)

    screenshots.value.unshift({
      id: Date.now(),
      timestamp: new Date().toLocaleString(),
      width: parseInt(width, 10),
      height: parseInt(height, 10),
      fullImage,
      thumbnail
    })
    screenshots.value = screenshots.value.slice(0, SCREENSHOT_LIMIT)
    saveShots(props.agent.id)
  } catch {
    toast.error('Failed to parse screenshot')
  }
}

const generateThumbnail = (imageUrl: string, maxW: number, maxH: number): Promise<string> => {
  return new Promise((resolve, reject) => {
    const img = new Image()
    img.onload = () => {
      const canvas = document.createElement('canvas')
      const ctx = canvas.getContext('2d')
      if (!ctx) {
        reject(new Error('Failed to get canvas context'))
        return
      }

      const ratio = Math.min(maxW / img.width, maxH / img.height)
      canvas.width = Math.max(1, Math.floor(img.width * ratio))
      canvas.height = Math.max(1, Math.floor(img.height * ratio))

      ctx.drawImage(img, 0, 0, canvas.width, canvas.height)
      resolve(canvas.toDataURL('image/jpeg', 0.8))
    }
    img.onerror = () => reject(new Error('Failed to load image'))
    img.src = imageUrl
  })
}

const viewFullscreen = (shot: Screenshot) => {
  fullscreenImage.value = shot.fullImage
}

const closeFullscreen = () => {
  fullscreenImage.value = null
}

const saveScreenshot = (shot: Screenshot) => {
  const link = document.createElement('a')
  link.href = shot.fullImage
  link.download = `screenshot_${props.agent.id}_${shot.timestamp.replace(/[:\s]/g, '_')}.bmp`
  link.click()
}

// Hydrate persisted screenshots from the SQLite store (via /agents/{id}/artifacts).
// Called when the in-memory cache for this agent is empty, so screenshots
// survive an app/core restart. Each persisted screenshot is a BMP blob.
const hydrateFromDb = async (agentId: string) => {
  if (!agentId) return
  const id = Number(agentId)
  if (!id) return
  try {
    const res = await api.get(`/agents/${id}/artifacts`, { params: { kind: 'screenshot', limit: 8 }, silentError: true } as any)
    const arts: any[] = res.data?.data || []
    if (!arts.length) return
    const restored: Screenshot[] = []
    for (const a of arts) {
      if (!a.has_blob) continue
      try {
        const one = await api.get(`/agents/${id}/artifacts/${a.id}`, { silentError: true } as any)
        const b64 = one.data?.data?.b64
        if (!b64) continue
        const fullImage = `data:image/bmp;base64,${b64}`
        const thumbnail = await generateThumbnail(fullImage, 300, 200).catch(() => fullImage)
        restored.push({
          id: a.id,
          timestamp: new Date(a.ts * 1000).toLocaleString(),
          width: 0, height: 0,
          fullImage,
          thumbnail
        })
      } catch { /* skip one bad artifact */ }
    }
    if (restored.length) {
      // newest-first in DB (ListArtifacts returns DESC); reverse so unshift-style
      screenshots.value = restored
      saveShots(agentId)
    }
  } catch { /* core may be old/unreachable; silent */ }
}

watch(() => props.agent.id, async (next) => {
  loadShots(next)
  // If the in-memory cache is empty, hydrate from the persisted store.
  if (!screenshots.value.length) {
    await hydrateFromDb(next)
  }
}, { immediate: true })
</script>

<style scoped>
.screenshot-viewer {
  display: flex;
  flex-direction: column;
  height: 100%;
  background: var(--bg-2);
}

.toolbar {
  padding: 12px;
  border-bottom: 1px solid var(--bd);
  display: flex;
  align-items: center;
  gap: 12px;
}

.btn {
  padding: 8px 16px;
  border-radius: 4px;
  font-size: 12px;
  cursor: pointer;
  border: 1px solid var(--bd);
  background: var(--bg-3);
  color: var(--tx);
  font-weight: 600;
  display: flex;
  align-items: center;
  gap: 6px;
}

.btn.primary {
  background: var(--pri);
  color: var(--on-pri, #062235);
  border: none;
}

.btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.count {
  font-size: 11px;
  color: var(--tx-3);
}

.empty-state {
  flex: 1;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  color: var(--tx-3);
  gap: 12px;
}

.empty-state p {
  margin: 0;
  font-size: 13px;
}

.empty-state .hint {
  font-size: 11px;
  color: var(--tx-4);
}

.screenshots-grid {
  flex: 1;
  overflow-y: auto;
  padding: 16px;
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(300px, 1fr));
  gap: 16px;
  align-content: start;
}

.screenshot-item {
  background: var(--bg-3);
  border: 1px solid var(--bd);
  border-radius: 6px;
  overflow: hidden;
  cursor: pointer;
  transition: transform 0.2s, box-shadow 0.2s;
}

.screenshot-item:hover {
  transform: translateY(-2px);
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);
}

.screenshot-item img {
  width: 100%;
  height: 200px;
  object-fit: cover;
  display: block;
}

.screenshot-info {
  padding: 10px;
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 11px;
  color: var(--tx-2);
}

.timestamp {
  flex: 1;
}

.resolution {
  color: var(--tx-3);
}

.icon-btn {
  background: transparent;
  border: none;
  color: var(--tx-3);
  cursor: pointer;
  padding: 4px;
  display: flex;
  align-items: center;
  border-radius: 3px;
}

.icon-btn:hover {
  background: var(--bg-4);
  color: var(--pri);
}

.fullscreen-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background: rgba(0, 0, 0, 0.9);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 9999;
  cursor: pointer;
}

.fullscreen-overlay img {
  max-width: 95%;
  max-height: 95%;
  object-fit: contain;
  cursor: default;
}

.close-fullscreen {
  position: absolute;
  top: 20px;
  right: 20px;
  background: rgba(0, 0, 0, 0.6);
  border: none;
  color: white;
  cursor: pointer;
  padding: 8px;
  border-radius: 50%;
  display: flex;
  align-items: center;
  justify-content: center;
  transition: background 0.2s;
}

.close-fullscreen:hover {
  background: rgba(0, 0, 0, 0.8);
}
</style>
