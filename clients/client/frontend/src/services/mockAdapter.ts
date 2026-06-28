/**
 * Unified axios adapter: mock (demo) OR real (Wails desktop) backend.
 *
 * Installed on the shared axios instance in services/api.ts. Every request the
 * XStrike UI makes (`api.get/post/...`) lands here. We branch on mode:
 *
 *   - mock  → answer from services/mockData.ts (browser preview / `?demo=1`)
 *   - real  → call the Wails Go bindings (services/wailsBindings), which proxy
 *             the Go service core, which drives the real Rust implant.
 *
 * This lets the entire existing UI (stores, command registry, Terminal, modals)
 * run unchanged against the real backend — only the transport is swapped. The
 * `{ success, data, error }` envelope is preserved so the response interceptor
 * in api.ts (toasts, 401 redirect) keeps working.
 *
 * Real-mode mapping (XStrike UI endpoint → RustStrike):
 *   GET  /nodes            → ListImplants()            (sparse beacon shape)
 *   GET  /listeners        → []                         (no listener concept)
 *   GET  /bof              → ListBofs()
 *   GET  /bof/commands     → synthesise BofCommandMeta from ListBofs
 *   GET  /logs             → client-side realBackend log buffer (fed by events)
 *   POST /bof/execute      → RunBofByName()  → returns task id
 *   GET  /tasks/{id}       → GetTaskResult() → {status, output} (null while running)
 *   POST /bof/upload       → UploadBof()
 *   POST /nodes/{id}/stop|delete, /nodes/batch/* → DropImplant()
 *   POST /auth/login       → real core password login
 *   POST /logs (audit)     → swallowed (client-side only)
 */

import type { AxiosInstance, AxiosRequestConfig, AxiosResponse } from 'axios'
import { isMockMode, MOCK_TOKEN } from './mockMode'
import {
  mockBeacons,
  mockListeners,
  mockBofs,
  mockEventLog,
  pushMockEvent,
  MOCK_LATENCY_MS,
} from './mockData'
import * as Wails from './wailsBindings'
import { getRealLogs, pushRealLog, eventToLog } from './realBackend'

const LOGIN_ERROR_MESSAGE = '账号或者密码错误'

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

function delay(ms: number) {
  return new Promise<void>((r) => setTimeout(r, ms))
}

/** Strip protocol/host and `/api` prefix; drop query string. Return bare path. */
function toApiPath(url: string | undefined): string {
  if (!url) return ''
  let u = url
  const i = u.indexOf('/api')
  if (i >= 0) u = u.slice(i + 4)
  if (!u.startsWith('/')) u = '/' + u
  return u.split('?')[0]
}

function ok(config: AxiosRequestConfig, data: unknown, status = 200): AxiosResponse {
  return { data, status, statusText: 'OK', headers: {}, config } as AxiosResponse
}

/** Reject with an axios-like error so the interceptor honours `silentError`. */
function fail(config: AxiosRequestConfig, message: string, status = 500): never {
  const e: any = new Error(message)
  e.config = config
  e.response = { data: { success: false, error: message }, status, statusText: 'Error', headers: {}, config }
  throw e
}

function parseBody(config: AxiosRequestConfig): any {
  const d = config.data
  if (d == null) return {}
  if (typeof d === 'string') {
    try { return JSON.parse(d) } catch { return {} }
  }
  return d
}

function bytesToB64(bytes: number[] | undefined): string {
  if (!bytes || !bytes.length) return ''
  let bin = ''
  for (let i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i] & 0xff)
  return btoa(bin)
}

async function syncRealAuthToken() {
  const token = localStorage.getItem('token') || ''
  try {
    await Wails.SetAuthToken(token)
  } catch {
    /* Wails may not be ready during early boot; the next request will retry. */
  }
}

// ---------------------------------------------------------------------------
// real-mode beacon mapping
// ---------------------------------------------------------------------------

function implantToBeacon(im: Wails.WailsImplant): any {
  const id = String(im.id)
  // since/last_seen are unix seconds (the core returns int64). Fall back to
  // "now" if the impl is offline / not yet enriched.
  const lastSeen = Number(im.last_seen || im.since) || Math.floor(Date.now() / 1000)
  // The auto-run sysinfo BOF populates the recon fields on connect; until it
  // reports back only id+addr are known, so fall back to addr for display.
  const computer = im.computer || im.addr
  const internalIP = im.internal_ip || im.addr
  const os = im.os || '-'
  const arch = im.arch || '-'
  return {
    engine: 'windows',
    node_id: id,
    id,
    hostname: computer,
    computer,
    ip: internalIP,
    internal_ip: internalIP,
    external_ip: im.external_ip || '-',
    process: im.process_name || '-',
    process_name: im.process_name || '-',
    pid: im.pid ? Number(im.pid) : 0,
    os,
    os_version: os !== '-' ? `${os}${arch !== '-' ? ` / ${arch}` : ''}` : '-',
    arch,
    os_build: im.os_build || '-',
    user: im.user || '-',
    online_time: im.online_time || '-',
    domain: '-',
    privilege: 'User',
    integrity: 1,
    last_seen: lastSeen,
    listener: 'tcp',
    checkin_interval: 30,
    started_at: lastSeen,
    cwd: '-',
    selected: false,
  }
}

/** Synthesise console-command metadata for the BOF library. */
function bofCommandMetas(bofs: { name: string }[]): any[] {
  // Per-BOF arg encoding. Two conventions are in play:
  //  - raw_string  : RAW UTF-8 bytes, no length prefix. RustStrike's Terminal
  //                  convention — cmd_exec / powershell_exec / ls / download
  //                  read the args buffer verbatim as text.
  //  - beacon_string : [2-byte LE length][UTF-8][null]. The frontend's
  //                  encodeBeaconString() framing — the feature components
  //                  (ProcessList/FileBrowser/Screenshot) send this, and the
  //                  matching BOFs (proc_kill/file_list/file_download) parse
  //                  it with an inline 2-byte-LE reader.
  //  - raw_hex_short : [2-byte LE length][raw bytes] — ShellcodeExecutor's
  //                  framing for shellcode_exec*.
  //  - none         : no args.
  const enc: Record<string, { encode_type: string; aliases?: string; description?: string; destructive?: boolean }> = {
    cmd_exec: { encode_type: 'raw_string', aliases: 'sh,shell,run', description: 'Run a cmd.exe command' },
    powershell_exec: { encode_type: 'raw_string', aliases: 'ps,powershell', description: 'Run a PowerShell command' },
    winapi_exec: { encode_type: 'raw_string', aliases: 'api,winapi,run-direct', description: 'Run an exe directly via CreateProcessA (no shell)' },
    ps: { encode_type: 'none', description: 'List processes (ps.c format)' },
    proc_list: { encode_type: 'none', aliases: 'procs', description: 'List processes (component format)' },
    proc_kill: { encode_type: 'beacon_string', aliases: 'kill', description: 'Kill a process by PID', destructive: true },
    netstat: { encode_type: 'none', aliases: 'net,netlist', description: 'List TCP/UDP network connections (component format)' },
    ls: { encode_type: 'raw_string', aliases: 'dir', description: 'List directory (ls.c format)' },
    file_list: { encode_type: 'beacon_string', aliases: 'ls2,flist', description: 'List directory (component format)' },
    download: { encode_type: 'raw_string', description: 'Download a file (raw-text path, base64 output)' },
    file_download: { encode_type: 'beacon_string', aliases: 'fdownload', description: 'Download a file (component format, base64 output)' },
    screenshot: { encode_type: 'none', aliases: 'screen', description: 'Capture a desktop screenshot (BMP)' },
    shellcode_exec: { encode_type: 'raw_hex_short', aliases: 'sc,shellcode', description: 'Run shellcode (VirtualAlloc+CreateThread)' },
    shellcode_exec_nt: { encode_type: 'raw_hex_short', aliases: 'scnt', description: 'Run shellcode (NtCreateThreadEx)' },
    hello: { encode_type: 'none', description: 'Link check' },
    // ── from the project-root bofs/ tree (CS-packed arg convention) ──
    sysinfo: { encode_type: 'none', description: 'Collect host recon (auto-run on connect)' },
    bof_whoami: { encode_type: 'none', aliases: 'whoami', description: 'Current user (shells out to whoami.exe)' },
    proc_critical_set: { encode_type: 'none', aliases: 'critical-set', description: 'Mark process critical (BSOD on kill); defaults to self', destructive: true },
    proc_critical_unset: { encode_type: 'none', aliases: 'critical-unset', description: 'Unmark a critical process; defaults to self' },
    schtask_persist: { encode_type: 'cs_packed', aliases: 'persist-schtask', description: 'Persist via scheduled task: <name> <ONLOGON|ONSTART|...>', destructive: true },
    schtask_persist_xml: { encode_type: 'cs_packed', aliases: 'persist-schtask-xml', description: 'Persist via task XML file: <name> <schedule>', destructive: true },
    schtask_persist_reg: { encode_type: 'cs_packed', aliases: 'persist-schtask-reg', description: 'Persist via task XML + registry: <name> <schedule>', destructive: true },
    svc_create_api: { encode_type: 'cs_packed', aliases: 'persist-svc', description: 'Persist via Windows service (SCM API): <name> [exe_path]', destructive: true },
    user_create_net: { encode_type: 'cs_packed_multi', aliases: 'adduser-net', description: 'Create local user (NetUserAdd): <user> <pass>', destructive: true },
    user_create_cmd: { encode_type: 'cs_packed_multi', aliases: 'adduser-cmd', description: 'Create local user (net user /add): <user> <pass>', destructive: true },
    user_create_ps: { encode_type: 'cs_packed_multi', aliases: 'adduser-ps', description: 'Create local user (PowerShell): <user> <pass>', destructive: true },
  }
  return bofs.map((b) => {
    const cfg = enc[b.name] || { encode_type: 'none' }
    return {
      bof_name: b.name,
      cmd_name: b.name,
      aliases: cfg.aliases || '',
      description: cfg.description || `Run BOF ${b.name}`,
      category: 'bof',
      args_json: cfg.encode_type === 'none' ? [] : [{ name: 'arg', type: 'string', required: false }],
      encode_type: cfg.encode_type,
      destructive: cfg.destructive || false,
      help_extra: '',
      enabled: true,
      plugin_name: '',
    }
  })
}

// ---------------------------------------------------------------------------
// real-mode request dispatch
// ---------------------------------------------------------------------------

async function realHandle(config: AxiosRequestConfig): Promise<AxiosResponse> {
  const method = (config.method || 'get').toLowerCase()
  const path = toApiPath(config.url)

  // ── auth ────────────────────────────────────────────────────────────────
  if (method === 'post' && path === '/auth/login') {
    const body = parseBody(config)
    let token = ''
    try {
      token = await Wails.Login(String(body?.username || ''), String(body?.password || ''))
    } catch {
      fail(config, LOGIN_ERROR_MESSAGE, 401)
    }
    return ok(config, { success: true, data: { token } })
  }
  await syncRealAuthToken()
  if (method === 'post' && path === '/auth/ticket') return ok(config, { success: true, data: 'real-no-ticket' })
  if (method === 'post' && path === '/auth/logout') return ok(config, { success: true, data: {} })

  // ── sessions ────────────────────────────────────────────────────────────
  if (method === 'get' && path === '/nodes') {
    const implants = await Wails.ListImplants()
    return ok(config, { success: true, data: implants.map(implantToBeacon) })
  }
  if (method === 'get' && path === '/listeners') {
    const listeners = await Wails.ListListeners()
    return ok(config, { success: true, data: listeners.map((l: any) => ({
      ...l,
      id: l.id,
      name: l.name || `tcp/${l.port}`,
      protocol: l.protocol || 'tcp',
      bind_ip: l.bind_ip || '0.0.0.0',
      port: Number(l.port) || 0,
      status: l.active ? 'running' : 'stopped',
      active: !!l.active,
    })) })
  }
  if (method === 'post' && path === '/listeners') {
    const body = parseBody(config)
    const l = await Wails.CreateListener(String(body?.name || ''), String(body?.bind_ip || '0.0.0.0'), String(body?.port || ''))
    return ok(config, { success: true, data: { id: l.id } })
  }
  if (method === 'put' && path.startsWith('/listeners/')) {
    const id = path.split('/')[2]
    const body = parseBody(config)
    await Wails.UpdateListener(id, String(body?.name || ''), String(body?.bind_ip || ''), String(body?.port || ''))
    return ok(config, { success: true, data: {} })
  }
  if (method === 'post' && path.startsWith('/listeners/')) {
    // /listeners/{id}/start | /listeners/{id}/stop
    const parts = path.split('/')
    const id = parts[2]
    const action = parts[3]
    if (id && (action === 'start' || action === 'stop')) {
      await Wails.ToggleListener(id, action === 'start')
      return ok(config, { success: true, data: {} })
    }
  }
  if (method === 'delete' && path.startsWith('/listeners/')) {
    const id = path.split('/')[2]
    await Wails.DeleteListener(id)
    return ok(config, { success: true, data: {} })
  }
  // Stub builder — patches the implant exe via the core and pops the OS Save
  // As dialog (handled in the Go bridge). Returns {path} (empty if cancelled).
  if (method === 'post' && path === '/stub/build') {
    const body = parseBody(config)
    const host = String(body?.host || '')
    const port = String(body?.port || '')
    const name = String(body?.name || '')
    const silent = Boolean(body?.silent)
    const r = await Wails.BuildStubToProject(host, port, name, silent)
    return ok(config, { success: true, data: { path: r.path } })
  }
  // Persistent agent roster + artifacts.
  if (method === 'get' && path === '/agents') {
    const agents = await Wails.ListAgents()
    return ok(config, { success: true, data: agents })
  }
  if (method === 'get' && path.startsWith('/agents/')) {
    // /agents/{id}/artifacts?kind=&limit=  |  /agents/{id}/artifacts/{aid}
    const parts = path.split('/') // ['', 'agents', id, 'artifacts', aid?]
    const id = Number(parts[2])
    if (parts[3] === 'artifacts' && id) {
      if (parts[4]) {
        // fetch one artifact
        const [kind, b64, meta] = await Wails.GetArtifact(id, Number(parts[4]))
        return ok(config, { success: true, data: { id: parts[4], kind, b64, meta } })
      }
      const kind = config.params?.kind || ''
      const limit = Number(config.params?.limit) || 50
      const arts = await Wails.ListArtifacts(id, String(kind), limit)
      return ok(config, { success: true, data: arts })
    }
  }
  if ((method === 'post' || method === 'delete') && path.startsWith('/nodes/')) {
    // /nodes/{id} | /nodes/{id}/stop | /nodes/{id}/delete
    const idStr = path.split('/')[2]
    const id = Number(idStr)
    if (id) await Wails.DropImplant(id)
    return ok(config, { success: true, data: {} })
  }
  if (method === 'post' && path.startsWith('/nodes/batch/')) {
    const body = parseBody(config)
    const ids: any[] = body?.ids || body?.node_ids || []
    for (const id of ids) {
      const n = Number(typeof id === 'object' ? id?.node_id : id)
      if (n) { try { await Wails.DropImplant(n) } catch { /* continue */ } }
    }
    return ok(config, { success: true, data: {} })
  }

  // ── BOFs ────────────────────────────────────────────────────────────────
  if (method === 'get' && path === '/bof') {
    const bofs = await Wails.ListBofs()
    return ok(config, { success: true, data: bofs.map((b) => ({ ...b, plugin_name: '' })) })
  }
  if (method === 'get' && path === '/bof/commands') {
    const bofs = await Wails.ListBofs()
    return ok(config, { success: true, data: bofCommandMetas(bofs) })
  }
  if (method === 'post' && path === '/bof/execute') {
    const body = parseBody(config)
    const id = Number(body?.node_id)
    if (!id) fail(config, 'no target session selected')
    const argsB64 = bytesToB64(body?.args)
    let taskId: string
    if (body?.bof_b64) {
      taskId = await Wails.RunBofByB64(id, body.bof_b64, argsB64)
    } else {
      taskId = await Wails.RunBofByName(id, String(body?.bof_name || ''), argsB64)
    }
    pushRealLog({ created_at: Math.floor(Date.now() / 1000), level: 'task', node_id: String(id), data: `>> ${body?.bof_name || 'bof'} queued` })
    return ok(config, { success: true, data: taskId })
  }
  if (method === 'post' && path === '/bof/upload') {
    const { name, b64 } = await extractUpload(config)
    if (!name) fail(config, 'BOF name required')
    await Wails.UploadBof(name, b64)
    return ok(config, { success: true, data: { name } })
  }
  if (method === 'delete' && path.startsWith('/bof/')) {
    // BOF deletion isn't exposed by the core; report success silently.
    return ok(config, { success: true, data: {} })
  }

  // ── tasks / logs ────────────────────────────────────────────────────────
  if (method === 'get' && path.startsWith('/tasks/')) {
    const taskId = path.split('/')[2]
    const t = await Wails.GetTaskResult(taskId)
    if (!t || t.status === 'running') {
      // Not ready yet — return null data so callers keep polling.
      return ok(config, { success: true, data: null })
    }
    const failed = t.status === 'failed'
    return ok(config, { success: true, data: { output: t.output || '', error: failed ? t.output : '', success: !failed, status: t.status, id: t.id } })
  }
  if (method === 'get' && path === '/logs') {
    // Hydrate from the persisted SQLite logs (survives core restart) and merge
    // in any live tail still in the client-side buffer. The store's fetchLogs
    // sets params.limit; implant filter optional.
    const limit = Number(config.params?.limit) || 500
    const implantId = Number(config.params?.implant) || 0
    let persisted: any[] = []
    try {
      persisted = await Wails.GetLogs(limit, implantId)
    } catch { /* core may be old; fall back to live buffer */ }
    const live = getRealLogs()
    // Dedup by id (persisted rows have ids; live buffer entries may not) —
    // prefer persisted, append live-only entries, newest-first ordering kept
    // by the store's normalizer.
    const seen = new Set(persisted.map((l: any) => l.id).filter(Boolean))
    const merged = [...persisted, ...live.filter((l: any) => !l.id || !seen.has(l.id))]
    return ok(config, { success: true, data: merged })
  }
  if (method === 'get' && path === '/tasks') {
    return ok(config, { success: true, data: [] })
  }
  if (method === 'post' && (path === '/logs' || path === '/tasks')) {
    // audit/task-create logging — client-side only in real mode.
    return ok(config, { success: true, data: {} })
  }

  // ── files (no real caller; swallow) ─────────────────────────────────────
  if (path.startsWith('/files')) return ok(config, { success: true, data: {} })

  // ── generic fallback ────────────────────────────────────────────────────
  return ok(config, { success: true, data: {} })
}

/** Extract {name, b64} from a JSON body or multipart FormData upload. */
async function extractUpload(config: AxiosRequestConfig): Promise<{ name: string; b64: string }> {
  const d = config.data
  if (d && typeof d === 'object' && !(d instanceof FormData)) {
    const body = d as any
    if (body.file_b64) return { name: body.name || '', b64: body.file_b64 }
    if (body.file instanceof Blob) {
      const buf = await body.file.arrayBuffer()
      return { name: body.name || '', b64: blobToB64(new Uint8Array(buf)) }
    }
  }
  if (d instanceof FormData) {
    let name = ''
    let b64 = ''
    for (const [key, val] of d.entries()) {
      if (key === 'name' && typeof val === 'string') name = val
      if ((key === 'file' || key === 'file_b64') && val instanceof Blob) {
        const buf = await val.arrayBuffer()
        b64 = blobToB64(new Uint8Array(buf))
      } else if (key === 'file_b64' && typeof val === 'string') {
        b64 = val
      }
    }
    return { name, b64 }
  }
  return { name: '', b64: '' }
}

function blobToB64(bytes: Uint8Array): string {
  let bin = ''
  for (let i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i])
  return btoa(bin)
}

// ---------------------------------------------------------------------------
// mock-mode request dispatch
// ---------------------------------------------------------------------------

async function mockHandle(config: AxiosRequestConfig): Promise<AxiosResponse> {
  const method = (config.method || 'get').toLowerCase()
  const path = toApiPath(config.url)
  await delay(method === 'get' ? MOCK_LATENCY_MS : 120)

  if (method === 'post' && path === '/auth/login') return ok(config, { success: true, data: { token: MOCK_TOKEN } })
  if (method === 'post' && path === '/auth/ticket') return ok(config, { success: true, data: 'mock-ticket-' + Math.random().toString(36).slice(2) })
  if (method === 'post' && path === '/auth/logout') return ok(config, { success: true, data: {} })

  if (method === 'get' && path === '/nodes') return ok(config, { success: true, data: mockBeacons })
  if (method === 'get' && path === '/listeners') return ok(config, { success: true, data: mockListeners })
  if (method === 'post' && path.startsWith('/listeners/')) {
    // /listeners/{id}/start | /listeners/{id}/stop — flip the mock listener.
    const parts = path.split('/')
    const id = parts[2]
    const action = parts[3]
    const l = mockListeners.find((x: any) => String(x.id) === String(id))
    if (l) {
      l.status = action === 'start' ? 'running' : 'stopped'
    }
    return ok(config, { success: true, data: {} })
  }
  if ((method === 'post' || method === 'delete') && path.startsWith('/nodes/')) {
    const id = path.split('/')[2]
    pushMockEvent('warn', id || '-', `Session ${id} terminated by operator`)
    return ok(config, { success: true, data: {} })
  }

  if (method === 'get' && path === '/bof') return ok(config, { success: true, data: mockBofs })
  if (method === 'get' && path === '/bof/commands') return ok(config, { success: true, data: bofCommandMetas(mockBofs) })
  if (method === 'post' && path === '/bof/execute') {
    const body = parseBody(config)
    const node = body?.node_id || '-'
    pushMockEvent('task', String(node), `${body?.bof_name || 'bof'} queued on ${node}`)
    return ok(config, { success: true, data: 't-' + Math.random().toString(36).slice(2, 8) })
  }
  if (method === 'post' && path === '/bof/upload') {
    pushMockEvent('info', '-', 'BOF uploaded to library')
    return ok(config, { success: true, data: {} })
  }
  if (method === 'post' && path === '/stub/build') {
    const body = parseBody(config)
    const name = String(body?.name || 'ruststrike-implant')
    return ok(config, { success: true, data: { path: `<repo>/agents/${name}.exe (mock)` } })
  }

  if (method === 'get' && path.startsWith('/tasks/')) {
    // mock: immediately "complete" with a canned result
    return ok(config, { success: true, data: { output: '(mock) command executed', error: '', success: true, status: 'completed' } })
  }
  if (method === 'get' && path === '/logs') return ok(config, { success: true, data: mockEventLog })
  if (method === 'post' && (path === '/logs' || path === '/tasks' || path.startsWith('/files'))) return ok(config, { success: true, data: {} })

  return ok(config, { success: true, data: {} })
}

// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------

export function installBackendAdapter(api: AxiosInstance) {
  api.defaults.adapter = async (config: AxiosRequestConfig): Promise<AxiosResponse> => {
    try {
      if (isMockMode()) return await mockHandle(config)
      return await realHandle(config)
    } catch (e: any) {
      if (e?.response || e?.config) throw e
      // Surface the real cause: Wails may reject a non-Error value (plain
      // string, or an object without .message). Don't collapse those to a
      // generic "request failed" — stringify so the operator sees what broke.
      const msg = typeof e === 'string' ? e
        : (e?.message || (e ? (() => { try { return JSON.stringify(e) } catch { return String(e) } })() : 'request failed'))
      fail(config, msg)
    }
  }
}

/** Used by the core:event subscriber to push real-mode events into the log. */
export { pushRealLog, eventToLog }
