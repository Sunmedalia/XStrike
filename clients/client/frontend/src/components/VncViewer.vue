<template>
  <div class="vnc-viewer" ref="containerRef">
    <!-- ─ Toolbar ─────────────────────────────────────────────── -->
    <div class="vnc-bar">
      <div class="vnc-bar-left">
        <span class="vnc-status" :class="statusClass">
          <span class="vnc-dot" />
          {{ statusLabel }}
        </span>
        <span v-if="connected" class="vnc-meta">
          {{ remoteWidth }}×{{ remoteHeight }} · {{ fps }} fps · {{ latency }}ms
        </span>
      </div>
      <div class="vnc-bar-right">
        <label class="vnc-label">Quality</label>
        <input type="range" min="10" max="100" step="5" v-model.number="quality"
          :disabled="!connected" class="vnc-slider" @change="sendConfig" />
        <label class="vnc-label">FPS</label>
        <input type="range" min="1" max="30" step="1" v-model.number="targetFps"
          :disabled="!connected" class="vnc-slider vnc-slider-sm" @change="sendConfig" />
        <button class="vnc-btn" @click="toggleFullscreen" :disabled="!connected" title="Fullscreen">⛶</button>
        <button v-if="status === 'error' || status === 'idle'" class="vnc-btn primary" @click="connect">Reconnect</button>
        <button v-if="connected || status === 'connecting'" class="vnc-btn danger" @click="doDisconnect">Disconnect</button>
      </div>
    </div>

    <!-- ─ Canvas ──────────────────────────────────────────────── -->
    <div class="vnc-canvas-wrap">
      <canvas
        ref="canvasRef"
        class="vnc-canvas"
        @mousemove="onMouseMove"
        @mousedown="onMouseDown"
        @mouseup="onMouseUp"
        @contextmenu.prevent="onContextMenu"
        @wheel.prevent="onWheel"
        @keydown="onKeyDown"
        @keyup="onKeyUp"
        tabindex="0"
      />
      <!-- Status overlay (only while not connected) -->
      <div v-if="!connected" class="vnc-overlay">
        <div class="vnc-overlay-inner">
          <div v-if="status === 'connecting'" class="vnc-spinner" />
          <div v-else class="vnc-icon">🖥</div>
          <div class="vnc-msg">{{ overlayMessage }}</div>
          <button v-if="status === 'error' || status === 'idle'" class="vnc-btn primary lg" @click="connect">Reconnect</button>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, nextTick } from 'vue'
import { useToastStore } from '../stores/toast'
import { getWsUrl } from '../runtime/env'
import { apiFetch } from '../runtime/network'

const props = defineProps<{ targetId: string }>()
const toast  = useToastStore()

const canvasRef    = ref<HTMLCanvasElement | null>(null)
const containerRef = ref<HTMLDivElement | null>(null)

type Status = 'idle' | 'connecting' | 'connected' | 'error'
const status       = ref<Status>('idle')
const remoteWidth  = ref(0)
const remoteHeight = ref(0)
const quality      = ref(75)
const targetFps    = ref(15)
const fps          = ref(0)
const latency      = ref(0)

let ws: WebSocket | null = null
let frameCount   = 0
let lastFpsTime  = 0
let manualStop   = false
// Render-loop state: only one frame decoded at a time; newer frames displace older ones.
let pendingFrame: Uint8Array | null = null
let isRendering  = false


const connected    = computed(() => status.value === 'connected')
const statusClass  = computed(() => ({
  'st-idle':       status.value === 'idle',
  'st-connecting': status.value === 'connecting',
  'st-connected':  status.value === 'connected',
  'st-error':      status.value === 'error',
}))
const statusLabel  = computed(() => ({
  idle: 'Disconnected', connecting: 'Connecting…', connected: 'Live', error: 'Error',
}[status.value]))
const overlayMessage = computed(() =>
  status.value === 'error'    ? 'Connection failed — check agent is running'
  : status.value === 'connecting' ? 'Connecting…'
  : 'Disconnected'
)

// (Canvas sizing is handled purely by CSS — no JS resize needed)

// ─── WebSocket ────────────────────────────────────────────────────────────────

async function connect() {
  if (ws) { ws.close(); ws = null }
  manualStop   = false
  status.value = 'connecting'

  // WebSocket cannot set Authorization headers on cross-origin/upgraded
  // connections reliably, so trade the JWT for a short-lived, single-use
  // ticket and pass it as a query param instead of the long-lived token.
  // If the ticket can't be obtained (expired JWT, server unreachable), bail
  // with a clear auth error instead of opening with an empty ticket (which
  // only fails later as a confusing 401 on the WebSocket).
  let ticket = ''
  try {
    const res = await apiFetch('/auth/ticket', { method: 'POST' })
    const body = await res.json()
    if (body?.success && body?.data) {
      ticket = body.data
    } else {
      status.value = 'error'
      toast.error('VNC authentication failed — re-login and try again')
      return
    }
  } catch {
    status.value = 'error'
    toast.error('VNC authentication unavailable — check server and try again')
    return
  }

  const url   = getWsUrl(`/api/vnc/${props.targetId}/ws?ticket=${encodeURIComponent(ticket)}`)

  ws = new WebSocket(url)
  ws.binaryType = 'arraybuffer'

  ws.onopen = () => {
    status.value = 'connected'
    toast.success(`VNC connected to ${props.targetId}`)
    sendConfig()
    canvasRef.value?.focus()
    lastFpsTime = performance.now()
    frameCount  = 0
  }

  ws.onmessage = (ev: MessageEvent) => {
    if (!(ev.data instanceof ArrayBuffer)) return
    // Store latest frame; start render loop if not already running.
    // This drops intermediate frames instead of queuing them, preventing
    // reordering and memory build-up when decode is slower than arrival rate.
    pendingFrame = new Uint8Array(ev.data)
    if (!isRendering) scheduleRender()
  }

  ws.onerror = () => {
    if (!manualStop) {
      status.value = 'error'
      toast.error('VNC WebSocket error')
    }
  }

  ws.onclose = () => {
    if (!manualStop && status.value === 'connected') {
      status.value = 'idle'
      toast.info('VNC session ended')
    }
    ws = null
  }
}

async function doDisconnect() {
  manualStop   = true
  status.value = 'idle'

  // Close WebSocket immediately
  if (ws) { ws.close(1000, 'user disconnect'); ws = null }

  // Tell the server to kill the agent TCP connection
  try {
    await apiFetch(`/vnc/${props.targetId}/disconnect`, {
      method: 'POST',
    })
    toast.info(`Disconnected VNC session ${props.targetId}`)
  } catch {
    // agent may already be gone
  }
}

// ─── Canvas draw ──────────────────────────────────────────────────────────────

// ─── Render loop (single-inflight, frame-dropping) ────────────────────────────

function scheduleRender() {
  isRendering = true
  requestAnimationFrame(() => renderLoop())
}

async function renderLoop() {
  while (pendingFrame) {
    const data = pendingFrame
    pendingFrame = null
    await processFrame(data)
  }
  isRendering = false
}

async function processFrame(data: Uint8Array) {
  if (data.length < 1) return
  const t0    = performance.now()
  const ftype = data[0]

  if (ftype === 0x02 /* FRAME_FULL */) {
    if (data.length < 5) return
    const w = (data[1] << 8) | data[2]
    const h = (data[3] << 8) | data[4]
    if (remoteWidth.value !== w || remoteHeight.value !== h) {
      remoteWidth.value  = w
      remoteHeight.value = h
      await nextTick()
      if (canvasRef.value) { canvasRef.value.width = w; canvasRef.value.height = h }
    }
    await drawJpegFull(data.slice(5))

  } else if (ftype === 0x03 /* FRAME_DELTA */) {
    await drawDelta(data.slice(1))
  }

  latency.value = Math.round(performance.now() - t0)
  frameCount++
  const now = performance.now()
  if (now - lastFpsTime >= 1000) {
    fps.value   = Math.round(frameCount * 1000 / (now - lastFpsTime))
    frameCount  = 0
    lastFpsTime = now
  }
}

async function drawJpegFull(jpeg: Uint8Array) {
  const canvas = canvasRef.value
  if (!canvas) return
  const ctx = canvas.getContext('2d')
  if (!ctx) return
  const bitmap = await createImageBitmap(new Blob([jpeg as unknown as BlobPart], { type: 'image/jpeg' }))
  ctx.drawImage(bitmap, 0, 0)
  bitmap.close()
}

// FRAME_DELTA payload: [count:u16 BE][[x:u16][y:u16][w:u16][h:u16][len:u32 BE][jpeg…]]…
async function drawDelta(payload: Uint8Array) {
  const canvas = canvasRef.value
  if (!canvas) return
  const ctx = canvas.getContext('2d')
  if (!ctx) return
  if (payload.length < 2) return
  const view  = new DataView(payload.buffer, payload.byteOffset, payload.byteLength)
  const count = view.getUint16(0, false)
  let offset  = 2
  for (let i = 0; i < count; i++) {
    if (offset + 12 > payload.length) break
    const x   = view.getUint16(offset, false); offset += 2
    const y   = view.getUint16(offset, false); offset += 2
    offset += 4  // skip w, h (block size known by encoder)
    const len = view.getUint32(offset, false); offset += 4
    if (offset + len > payload.length) break
    const jpeg   = payload.slice(offset, offset + len); offset += len
    const bitmap = await createImageBitmap(new Blob([jpeg as unknown as BlobPart], { type: 'image/jpeg' }))
    ctx.drawImage(bitmap, x, y)
    bitmap.close()
  }
}

// ─── Input helpers ────────────────────────────────────────────────────────────

function sendBinary(ftype: number, payload: Uint8Array) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return
  const buf = new Uint8Array(1 + payload.length)
  buf[0] = ftype
  buf.set(payload, 1)
  ws.send(buf)
}

function sendConfig() {
  sendBinary(0x30, new TextEncoder().encode(JSON.stringify({ fps: targetFps.value, quality: quality.value })))
}

// ── Mouse ─────────────────────────────────────────────────────────────────────

function canvasCoords(e: MouseEvent): [number, number] {
  const canvas = canvasRef.value!
  const rect   = canvas.getBoundingClientRect()
  const scaleX = remoteWidth.value  / rect.width
  const scaleY = remoteHeight.value / rect.height
  return [Math.round((e.clientX - rect.left) * scaleX), Math.round((e.clientY - rect.top) * scaleY)]
}
function buttonMask(e: MouseEvent): number {
  let m = 0
  if (e.buttons & 1) m |= 1
  if (e.buttons & 2) m |= 2
  if (e.buttons & 4) m |= 4
  return m
}
function sendMouse(e: MouseEvent, wheel = 0) {
  if (!connected.value) return
  const [x, y] = canvasCoords(e)
  const buf  = new Uint8Array(6)
  const view = new DataView(buf.buffer)
  view.setUint16(0, x, false)
  view.setUint16(2, y, false)
  buf[4] = buttonMask(e)
  buf[5] = wheel & 0xFF
  sendBinary(0x10, buf)
}
function onMouseMove(e: MouseEvent)   { sendMouse(e) }
function onMouseDown(e: MouseEvent)   { sendMouse(e); canvasRef.value?.focus() }
function onMouseUp(e: MouseEvent)     { sendMouse(e) }
function onContextMenu(e: MouseEvent) { sendMouse(e) }
function onWheel(e: WheelEvent)       { sendMouse(e as unknown as MouseEvent, e.deltaY > 0 ? -1 : 1) }

// ── Keyboard ──────────────────────────────────────────────────────────────────

const VK_MAP: Record<string, number> = {
  Backspace:0x08, Tab:0x09, Enter:0x0D, Escape:0x1B, Space:0x20,
  PageUp:0x21, PageDown:0x22, End:0x23, Home:0x24,
  ArrowLeft:0x25, ArrowUp:0x26, ArrowRight:0x27, ArrowDown:0x28,
  Insert:0x2D, Delete:0x2E,
  F1:0x70, F2:0x71, F3:0x72, F4:0x73, F5:0x74, F6:0x75,
  F7:0x76, F8:0x77, F9:0x78, F10:0x79, F11:0x7A, F12:0x7B,
  ShiftLeft:0xA0, ShiftRight:0xA1,
  ControlLeft:0xA2, ControlRight:0xA3,
  AltLeft:0xA4, AltRight:0xA5,
  MetaLeft:0x5B, MetaRight:0x5C,
  CapsLock:0x14, NumLock:0x90, ScrollLock:0x91,
}
function sendKey(e: KeyboardEvent, down: boolean) {
  if (!connected.value) return
  e.preventDefault()
  let vk = VK_MAP[e.code]
  if (!vk && e.key.length === 1) {
    const c = e.key.toUpperCase().charCodeAt(0)
    if (c >= 0x30 && c <= 0x5A) vk = c
  }
  if (!vk) return
  const buf  = new Uint8Array(5)
  const view = new DataView(buf.buffer)
  view.setUint32(0, vk, true)
  buf[4] = down ? 1 : 0
  sendBinary(0x11, buf)
}
function onKeyDown(e: KeyboardEvent) { sendKey(e, true) }
function onKeyUp(e:   KeyboardEvent) { sendKey(e, false) }

// ── Fullscreen ────────────────────────────────────────────────────────────────

function toggleFullscreen() {
  const el = containerRef.value
  if (!el) return
  if (!document.fullscreenElement) el.requestFullscreen().catch(() => {})
  else document.exitFullscreen()
}

// ── Lifecycle: auto-connect on mount ──────────────────────────────────────────

onMounted(() => {
  // Auto-connect immediately
  connect()
})

onUnmounted(() => {
  manualStop = true
  ws?.close()
  ws = null
})


</script>

<style scoped>
.vnc-viewer {
  display: flex;
  flex-direction: column;
  height: 100%;
  min-height: 0;
  background: #0a0a0a;
}

/* ─ Toolbar ──────────────────────────────────────────────────────────────────── */
.vnc-bar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 5px 12px;
  border-bottom: 1px solid var(--bd);
  background: var(--bg-2);
  flex-shrink: 0;
  gap: 10px;
}
.vnc-bar-left, .vnc-bar-right { display: flex; align-items: center; gap: 8px; }
.vnc-status {
  display: flex; align-items: center; gap: 5px;
  font-size: 11px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.06em;
}
.vnc-dot { width: 7px; height: 7px; border-radius: 50%; background: var(--tx-4); }
.st-connected  .vnc-dot { background: var(--green); box-shadow: 0 0 6px var(--green); }
.st-connecting .vnc-dot { background: var(--amber); animation: blink 1s infinite; }
.st-error      .vnc-dot { background: var(--red);   box-shadow: 0 0 6px var(--red); }
@keyframes blink { 0%,100%{opacity:1} 50%{opacity:0.3} }

.vnc-meta   { font-size: 11px; color: var(--tx-3); font-family: var(--font-mono); }
.vnc-label  { font-size: 10px; color: var(--tx-4); text-transform: uppercase; letter-spacing: 0.05em; }
.vnc-slider    { width: 80px; accent-color: var(--pri); cursor: pointer; }
.vnc-slider-sm { width: 50px; }

.vnc-btn {
  font-size: 11px; padding: 4px 10px;
  border: 1px solid var(--bd); border-radius: 4px;
  background: var(--bg-3); color: var(--tx);
  cursor: pointer; transition: background .1s;
  display: flex; align-items: center; gap: 4px;
}
.vnc-btn:hover   { background: var(--bg-4); }
.vnc-btn.primary { background: var(--pri); border-color: var(--pri); color: #000; font-weight: 600; }
.vnc-btn.primary:hover { filter: brightness(1.1); }
.vnc-btn.danger  { border-color: var(--red); color: var(--red); }
.vnc-btn.danger:hover { background: color-mix(in srgb, var(--red) 15%, var(--bg-3)); }
.vnc-btn.lg      { padding: 8px 18px; font-size: 13px; }
.vnc-btn:disabled { opacity: 0.4; cursor: not-allowed; }

/* ─ Canvas container — fills ALL remaining space ─────────────────────────────── */
.vnc-canvas-wrap {
  position: relative;
  flex: 1;
  min-height: 0;
  overflow: hidden;
  background: #0a0a0a;
}

/*
 * Canvas native resolution (width/height attrs) = remote screen.
 * CSS sizing: absolute-positioned, centered, scaled to fit via max constraints.
 * This avoids all JS timing issues.
 */
.vnc-canvas {
  position: absolute;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  margin: auto;
  max-width: 100%;
  max-height: 100%;
  display: block;
  cursor: crosshair;
}
.vnc-canvas:focus { outline: none; }

/* ─ Overlay ──────────────────────────────────────────────────────────────────── */
.vnc-overlay {
  position: absolute; inset: 0;
  background: rgba(10,10,14,0.88);
  display: flex; align-items: center; justify-content: center;
  backdrop-filter: blur(4px);
}
.vnc-overlay-inner {
  text-align: center;
  display: flex; flex-direction: column; align-items: center; gap: 14px;
}
.vnc-icon { font-size: 48px; line-height: 1; }
.vnc-msg  { font-size: 13px; color: var(--tx-3); max-width: 280px; line-height: 1.5; }

.vnc-spinner {
  width: 32px; height: 32px;
  border: 3px solid var(--bd);
  border-top-color: var(--pri);
  border-radius: 50%;
  animation: spin 0.8s linear infinite;
}
@keyframes spin { to { transform: rotate(360deg); } }
</style>
