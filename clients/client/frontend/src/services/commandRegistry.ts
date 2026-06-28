/**
 * XStrike Console Command Registry
 *
 * Provides a unified command system for the global terminal.
 * Three handler types: 'local' (no network), 'api' (REST call), 'bof' (BOF execution on beacon).
 */

import api from './api'
import { auditTaskInput } from './taskAudit'

// ─── Tokenizer ──────────────────────────────────────────────────────────

/** Tokenize input respecting double-quoted strings. */
export function tokenize(input: string): string[] {
  const tokens: string[] = []
  let i = 0
  while (i < input.length) {
    // skip whitespace
    while (i < input.length && input[i] === ' ') i++
    if (i >= input.length) break

    if (input[i] === '"') {
      // quoted token
      i++
      let tok = ''
      while (i < input.length && input[i] !== '"') {
        if (input[i] === '\\' && i + 1 < input.length) {
          tok += input[++i]
        } else {
          tok += input[i]
        }
        i++
      }
      if (i < input.length) i++ // skip closing quote
      tokens.push(tok)
    } else {
      let tok = ''
      while (i < input.length && input[i] !== ' ') {
        tok += input[i]
        i++
      }
      tokens.push(tok)
    }
  }
  return tokens
}

// ─── Beacon arg encoder helpers ─────────────────────────────────────────

/** Encode a string to beacon format: 2-byte LE length + UTF-8 bytes + null */
export function encodeBeaconString(str: string): number[] {
  const bytes = Array.from(new TextEncoder().encode(str))
  const len = bytes.length + 1 // +1 for null terminator
  return [len & 0xff, (len >> 8) & 0xff, ...bytes, 0]
}

/** Encode raw bytes (no beacon string wrapper, just the bytes) */
export function encodeRawBytes(hex: string): number[] {
  const cleaned = hex.replace(/\s+/g, '')
  const bytes: number[] = []
  for (let i = 0; i < cleaned.length; i += 2) {
    const b = parseInt(cleaned.substring(i, i + 2), 16)
    if (isNaN(b)) throw new Error(`Invalid hex at position ${i}: "${cleaned.substring(i, i + 2)}"`)
    bytes.push(b)
  }
  return bytes
}

// ─── Types ──────────────────────────────────────────────────────────────

export interface CommandArg {
  name: string
  required: boolean
  description: string
  rest?: boolean // captures all remaining tokens
}

export interface CommandContext {
  /** Currently selected target beacon ID, or null */
  targetId: string | null
  /** Pinia app store */
  appStore: any
  /** Toast store */
  toast: any
  /** Function to push a line to terminal output */
  pushLine: (text: string, type?: string) => void
  /** Function to set loading state */
  setLoading: (v: boolean) => void
  /** Function to set the task banner */
  setTaskBanner: (id: string) => void
  /** Function to set selected target */
  setTarget: (id: string | null) => void
  /** Function to poll task output and display result */
  pollTask: (taskId: string) => Promise<void>
}

export interface CommandDef {
  name: string
  aliases: string[]
  description: string
  category: 'system' | 'bof' | 'manage'
  /** 'local' = no network, 'api' = REST call, 'bof' = BOF on beacon */
  type: 'local' | 'api' | 'bof'
  /** For BOF commands: the .o file name */
  bofName?: string
  args: CommandArg[]
  /** Whether this command requires a target beacon */
  requiresTarget: boolean
  /** Whether this is a destructive command requiring --force */
  destructive?: boolean
  /** Execute the command. tokens[0] is the command name itself. */
  execute: (tokens: string[], ctx: CommandContext) => Promise<void>
}

// ─── Command Definitions ────────────────────────────────────────────────

const commands: CommandDef[] = []

function def(cmd: CommandDef) {
  commands.push(cmd)
}

// ── System / Local commands ─────────────────────────────────────────────

def({
  name: 'help',
  aliases: ['?'],
  description: 'Show all available commands or help for a specific command',
  category: 'system',
  type: 'local',
  args: [{ name: 'command', required: false, description: 'Command name to get help for' }],
  requiresTarget: false,
  execute: async (tokens, ctx) => {
    const sub = tokens[1]
    if (sub) {
      const cmd = findCommand(sub)
      if (!cmd) {
        ctx.pushLine(`Unknown command: ${sub}`, 'error')
        return
      }
      showCommandHelp(cmd, ctx)
      return
    }
    ctx.pushLine('═══ XStrike Console Commands ═══', 'sys')
    ctx.pushLine('')
    const groups = new Map<string, CommandDef[]>()
    for (const c of commands) {
      const cat = c.category
      if (!groups.has(cat)) groups.set(cat, [])
      groups.get(cat)!.push(c)
    }
    const catLabels: Record<string, string> = {
      system: '📋 System',
      manage: '⚙️  Management',
      bof: '🔧 BOF Plugins (require target)'
    }
    for (const [cat, cmds] of groups) {
      ctx.pushLine(`── ${catLabels[cat] || cat} ──`, 'sys')
      for (const c of cmds) {
        const aliases = c.aliases.length ? ` (${c.aliases.join(', ')})` : ''
        const argStr = c.args.map(a => a.required ? `<${a.name}>` : `[${a.name}]`).join(' ')
        ctx.pushLine(`  ${c.name}${aliases} ${argStr}  — ${c.description}`)
      }
      ctx.pushLine('')
    }
    ctx.pushLine('Use "help <command>" for detailed help. Use -h on any command.', 'info')
    ctx.pushLine('Use "use <node_id>" to select a target for BOF commands.', 'info')
  }
})

def({
  name: 'clear',
  aliases: [],
  description: 'Clear the terminal screen',
  category: 'system',
  type: 'local',
  args: [],
  requiresTarget: false,
  execute: async (_tokens, _ctx) => {
    // handled specially in Terminal.vue (clears lines array)
  }
})

def({
  name: 'use',
  aliases: ['target'],
  description: 'Select a target beacon for BOF commands',
  category: 'system',
  type: 'local',
  args: [{ name: 'node_id', required: false, description: 'Node ID to target (omit to clear)' }],
  requiresTarget: false,
  execute: async (tokens, ctx) => {
    const nodeId = tokens[1]
    if (!nodeId) {
      if (ctx.targetId) {
        ctx.setTarget(null)
        ctx.pushLine('Target cleared.', 'info')
      } else {
        ctx.pushLine('No target selected. Usage: use <node_id>', 'info')
        ctx.pushLine('Available beacons:', 'info')
        await showNodes(ctx)
      }
      return
    }
    await ctx.appStore.fetchBeacons()
    const beacon = ctx.appStore.beacons.find(
      (b: any) => (b.node_id || b.id) === nodeId ||
                   (b.node_id || b.id).startsWith(nodeId)
    )
    if (!beacon) {
      ctx.pushLine(`Beacon not found: ${nodeId}`, 'error')
      return
    }
    const id = beacon.node_id || beacon.id
    const lastSeen = Number(beacon.last_seen || 0)
    const now = Math.floor(Date.now() / 1000)
    if (lastSeen > 0 && (now - lastSeen) > 300) {
      ctx.pushLine(`Warning: beacon ${id} last seen ${now - lastSeen}s ago (offline?)`, 'error')
    }
    ctx.setTarget(id)
    ctx.pushLine(`Target set → ${id} (${beacon.hostname || beacon.computer || '-'})`, 'info')
  }
})

def({
  name: 'nodes',
  aliases: ['beacons'],
  description: 'List all connected beacons',
  category: 'manage',
  type: 'api',
  args: [],
  requiresTarget: false,
  execute: async (_tokens, ctx) => {
    await ctx.appStore.fetchBeacons()
    await showNodes(ctx)
  }
})

async function showNodes(ctx: CommandContext) {
  const beacons = ctx.appStore.beacons
  if (!beacons.length) {
    ctx.pushLine('No beacons connected.', 'info')
    return
  }
  const now = Math.floor(Date.now() / 1000)
  ctx.pushLine(`  ${'ID'.padEnd(40)} ${'HOST'.padEnd(16)} ${'IP'.padEnd(16)} ${'USER'.padEnd(14)} ${'OS'.padEnd(20)} STATUS`)
  ctx.pushLine('  ' + '─'.repeat(120))
  for (const b of beacons) {
    const id = (b.node_id || b.id || '').padEnd(40)
    const host = (b.hostname || b.computer || '-').padEnd(16)
    const ip = (b.ip || b.internal_ip || '-').padEnd(16)
    const user = (b.user || '-').padEnd(14)
    const os = (b.os_version || b.os || '-').padEnd(20)
    const lastSeen = Number(b.last_seen || 0)
    const alive = lastSeen > 0 && (now - lastSeen) < 300
    const status = alive ? '● ALIVE' : '○ DEAD'
    const marker = ctx.targetId === (b.node_id || b.id) ? '→ ' : '  '
    ctx.pushLine(`${marker}${id} ${host} ${ip} ${user} ${os} ${status}`)
  }
  ctx.pushLine(`\n  Total: ${beacons.length}`, 'info')
}

def({
  name: 'listeners',
  aliases: ['lsnr'],
  description: 'List all listeners',
  category: 'manage',
  type: 'api',
  args: [],
  requiresTarget: false,
  execute: async (_tokens, ctx) => {
    await ctx.appStore.fetchListeners()
    const listeners = ctx.appStore.listeners
    if (!listeners.length) {
      ctx.pushLine('No listeners configured.', 'info')
      return
    }
    ctx.pushLine(`  ${'ID'.padEnd(20)} ${'NAME'.padEnd(16)} ${'PROTOCOL'.padEnd(10)} ${'BIND'.padEnd(18)} ${'PORT'.padEnd(8)} STATUS`)
    ctx.pushLine('  ' + '─'.repeat(90))
    for (const l of listeners) {
      const id = (l.id || '').padEnd(20)
      const name = (l.name || '-').padEnd(16)
      const proto = (l.protocol || '-').padEnd(10)
      const bind = (l.bind_ip || '0.0.0.0').padEnd(18)
      const port = String(l.port || '-').padEnd(8)
      const status = l.status === 'running' ? '● RUNNING' : '○ STOPPED'
      ctx.pushLine(`  ${id} ${name} ${proto} ${bind} ${port} ${status}`)
    }
  }
})

def({
  name: 'bofs',
  aliases: ['plugins'],
  description: 'List all uploaded BOF plugins',
  category: 'manage',
  type: 'api',
  args: [],
  requiresTarget: false,
  execute: async (_tokens, ctx) => {
    await ctx.appStore.fetchBofs()
    const bofs = ctx.appStore.bofs
    if (!bofs.length) {
      ctx.pushLine('No BOFs uploaded.', 'info')
      return
    }
    // check which BOFs have registered commands
    const bofMap = new Map<string, CommandDef>()
    for (const c of commands) {
      if (c.bofName) bofMap.set(c.bofName, c)
    }
    ctx.pushLine(`  ${'NAME'.padEnd(30)} ${'SIZE'.padEnd(10)} COMMAND`)
    ctx.pushLine('  ' + '─'.repeat(60))
    for (const b of bofs) {
      const name = (b.name || '').padEnd(30)
      const size = (b.size ? `${(b.size / 1024).toFixed(1)}KB` : '-').padEnd(10)
      const cmd = bofMap.get(b.name)
      const cmdStr = cmd ? `→ ${cmd.name}` : ''
      ctx.pushLine(`  ${name} ${size} ${cmdStr}`)
    }
    ctx.pushLine(`\n  Total: ${bofs.length}`, 'info')
  }
})

def({
  name: 'logs',
  aliases: [],
  description: 'View or manage task logs',
  category: 'manage',
  type: 'api',
  args: [
    { name: 'action', required: false, description: '"clear" to clear all, or limit number (default 20)' }
  ],
  requiresTarget: false,
  destructive: true,
  execute: async (tokens, ctx) => {
    const action = tokens[1]
    if (action === 'clear') {
      if (!tokens.includes('--force')) {
        ctx.pushLine('This will delete ALL logs. Add --force to confirm.', 'error')
        ctx.pushLine('Usage: logs clear --force', 'info')
        return
      }
      try {
        await api.post('/logs/clear')
        await ctx.appStore.fetchLogs()
        ctx.pushLine('All logs cleared.', 'info')
      } catch {
        ctx.pushLine('Failed to clear logs.', 'error')
      }
      return
    }
    const limit = action ? parseInt(action) || 20 : 20
    await ctx.appStore.fetchLogs()
    const logs = ctx.appStore.logs.slice(0, limit)
    if (!logs.length) {
      ctx.pushLine('No logs.', 'info')
      return
    }
    for (const log of logs) {
      const ts = new Date(log.timestamp).toLocaleTimeString([], { hour12: false })
      const lvl = (log.log_type || 'info').toUpperCase().padEnd(7)
      const node = (log.node_id || '-').substring(0, 12).padEnd(14)
      const msg = log.message || ''
      ctx.pushLine(`  ${ts}  ${lvl} ${node} ${msg}`)
    }
    ctx.pushLine(`\n  Showing ${logs.length} of ${ctx.appStore.logs.length}`, 'info')
  }
})

def({
  name: 'debug',
  aliases: [],
  description: 'Toggle server debug logging',
  category: 'manage',
  type: 'api',
  args: [{ name: 'on|off', required: false, description: 'Enable or disable debug mode' }],
  requiresTarget: false,
  execute: async (tokens, ctx) => {
    if (!tokens[1]) {
      try {
        const res = await api.get('/debug')
        const d = res.data.data
        ctx.pushLine(`Debug mode: ${d.debug ? 'ON' : 'OFF'} (level: ${d.level})`, 'info')
      } catch {
        ctx.pushLine('Failed to query debug status.', 'error')
      }
      return
    }
    const on = tokens[1] === 'on' || tokens[1] === 'true' || tokens[1] === '1'
    try {
      const res = await api.put('/debug', { debug: on })
      const d = res.data.data
      ctx.pushLine(`Debug mode ${d.debug ? 'enabled' : 'disabled'} (level: ${d.level})`, 'info')
    } catch {
      ctx.pushLine('Failed to set debug mode.', 'error')
    }
  }
})

def({
  name: 'sleep',
  aliases: [],
  description: 'Set beacon sleep interval (seconds)',
  category: 'manage',
  type: 'api',
  args: [{ name: 'seconds', required: true, description: 'Sleep interval in seconds' }],
  requiresTarget: true,
  execute: async (tokens, ctx) => {
    const seconds = parseInt(tokens[1])
    if (isNaN(seconds) || seconds < 0) {
      ctx.pushLine('Invalid sleep value. Usage: sleep <seconds>', 'error')
      return
    }
    try {
      const taskId = `sleep-${Date.now()}-${Math.random().toString(36).slice(2, 11)}`
      await api.post('/tasks', {
        node_id: ctx.targetId,
        task: { SetSleep: { task_id: taskId, interval_ms: seconds * 1000 } }
      })
      ctx.pushLine(`Sleep set to ${seconds}s for ${ctx.targetId}`, 'info')
    } catch {
      ctx.pushLine('Failed to set sleep interval.', 'error')
    }
  }
})

def({
  name: 'stop',
  aliases: [],
  description: 'Stop (exit) target beacon',
  category: 'manage',
  type: 'api',
  args: [],
  requiresTarget: true,
  destructive: true,
  execute: async (tokens, ctx) => {
    if (!tokens.includes('--force')) {
      ctx.pushLine(`This will EXIT beacon ${ctx.targetId}. Add --force to confirm.`, 'error')
      ctx.pushLine('Usage: stop --force', 'info')
      return
    }
    try {
      await api.post(`/nodes/${ctx.targetId}/stop`)
      ctx.pushLine(`Exit command sent to ${ctx.targetId}`, 'info')
      ctx.setTarget(null)
    } catch {
      ctx.pushLine('Failed to stop beacon.', 'error')
    }
  }
})

def({
  name: 'delete',
  aliases: ['remove'],
  description: 'Delete target beacon from server',
  category: 'manage',
  type: 'api',
  args: [],
  requiresTarget: true,
  destructive: true,
  execute: async (tokens, ctx) => {
    if (!tokens.includes('--force')) {
      ctx.pushLine(`This will DELETE beacon ${ctx.targetId}. Add --force to confirm.`, 'error')
      ctx.pushLine('Usage: delete --force', 'info')
      return
    }
    try {
      await api.post(`/nodes/${ctx.targetId}/delete`)
      ctx.pushLine(`Beacon ${ctx.targetId} deleted.`, 'info')
      ctx.setTarget(null)
      await ctx.appStore.fetchBeacons()
    } catch {
      ctx.pushLine('Failed to delete beacon.', 'error')
    }
  }
})

// ── Dynamic BOF Commands ────────────────────────────────────────────────

/** BOF command metadata from server */
interface BofCommandMeta {
  bof_name: string
  cmd_name: string
  aliases: string
  description: string
  category: string
  args_json: string
  encode_type: string  // none | beacon_string | raw_string | raw_hex | raw_hex_short | beacon_string_multi
  destructive: boolean
  help_extra: string
  enabled: boolean
  plugin_name?: string
}

function bofExecute(
  bofName: string,
  pluginName: string,
  argsEncoder: (tokens: string[], ctx: CommandContext) => number[] | null
) {
  return async (tokens: string[], ctx: CommandContext) => {
    const available = ctx.appStore.bofs.find(
      (b: any) => b.name === bofName && (b.plugin_name || '') === pluginName
    )
    if (!available) {
      ctx.pushLine(`BOF not available: ${bofName}. Upload it first.`, 'error')
      return
    }
    const args = argsEncoder(tokens, ctx)
    if (args === null) return // encoder showed error/help
    ctx.setLoading(true)
    try {
      await auditTaskInput({
        source: `console:${tokens[0]}`,
        nodeId: ctx.targetId || '',
        input: tokens.join(' ')
      })
      const payload: any = {
        node_id: ctx.targetId,
        bof_name: bofName,
        plugin_name: pluginName,
      }
      if (args.length > 0) payload.args = args
      const res = await api.post('/bof/execute', payload)
      if (res.data.success) {
        const taskId = res.data.data
        ctx.setTaskBanner(taskId)
        await ctx.pollTask(taskId)
      } else {
        ctx.pushLine(`Execute failed: ${res.data.error || 'unknown'}`, 'error')
      }
    } catch {
      ctx.pushLine('BOF execution failed.', 'error')
    } finally {
      ctx.setLoading(false)
    }
  }
}

/** Build an args encoder based on encode_type and args definition */
function buildEncoder(
  meta: BofCommandMeta,
  parsedArgs: CommandArg[]
): (tokens: string[], ctx: CommandContext) => number[] | null {
  const { encode_type, cmd_name } = meta

  return (tokens: string[], ctx: CommandContext) => {
    // Validate required args
    const argTokens = tokens.slice(1).filter(t => t !== '--force')
    const requiredCount = parsedArgs.filter(a => a.required).length
    if (argTokens.length < requiredCount) {
      const usage = parsedArgs.map(a => a.required ? `<${a.name}>` : `[${a.name}]`).join(' ')
      ctx.pushLine(`Usage: ${cmd_name} ${usage}`, 'error')
      if (meta.help_extra) ctx.pushLine(meta.help_extra, 'info')
      return null
    }

    switch (encode_type) {
      case 'none':
        return []

      case 'beacon_string': {
        const input = argTokens.join(' ')
        if (!input) return []
        return encodeBeaconString(input)
      }

      case 'raw_string': {
        // RustStrike BOFs that take a single string arg read it as RAW UTF-8
        // bytes with no length prefix (e.g. cmd_exec, ls, download). Join the
        // tokens with a space so multi-word args (paths, commands) survive.
        const input = argTokens.join(' ')
        if (!input) return []
        return Array.from(new TextEncoder().encode(input))
      }

      case 'raw_hex': {
        const hexStr = argTokens.join(' ')
        if (!hexStr) {
          ctx.pushLine(`Usage: ${cmd_name} <hex bytes>`, 'error')
          return null
        }
        try {
          const raw = encodeRawBytes(hexStr)
          const len = raw.length
          return [len & 0xff, (len >> 8) & 0xff, (len >> 16) & 0xff, (len >> 24) & 0xff, ...raw]
        } catch (e: any) {
          ctx.pushLine(`Invalid hex: ${e.message}`, 'error')
          return null
        }
      }

      case 'raw_hex_short': {
        // [2-byte LE length][raw bytes] — the framing ShellcodeExecutor.vue
        // uses for shellcode_exec* (and matches the BOFs' 2-byte-LE arg reader).
        const hexStr = argTokens.join(' ')
        if (!hexStr) {
          ctx.pushLine(`Usage: ${cmd_name} <hex bytes>`, 'error')
          return null
        }
        try {
          const raw = encodeRawBytes(hexStr)
          const len = raw.length
          if (len > 0xffff) {
            ctx.pushLine(`Shellcode too large for 2-byte length (>65535 bytes)`, 'error')
            return null
          }
          return [len & 0xff, (len >> 8) & 0xff, ...raw]
        } catch (e: any) {
          ctx.pushLine(`Invalid hex: ${e.message}`, 'error')
          return null
        }
      }

      case 'beacon_string_multi': {
        const result: number[] = []
        for (const t of argTokens) {
          result.push(...encodeBeaconString(t))
        }
        return result
      }

      case 'cs_packed': {
        // Cobalt-Strike packed format: [4-byte BIG-endian length][bytes], no
        // null (the BOF null-terminates from the length). One blob for the
        // whole joined arg line — used by bofs/ BOFs that read a single
        // BeaconDataExtract blob (e.g. schtask_persist "name schedule").
        const input = argTokens.join(' ')
        if (!input) return []
        const bytes = Array.from(new TextEncoder().encode(input))
        const len = bytes.length
        return [(len >> 24) & 0xff, (len >> 16) & 0xff, (len >> 8) & 0xff, len & 0xff, ...bytes]
      }

      case 'cs_packed_multi': {
        // One CS-packed blob per token — for bofs/ BOFs that read N
        // BeaconDataExtract blobs (e.g. user_create_net <user> <pass>).
        const result: number[] = []
        for (const t of argTokens) {
          const bytes = Array.from(new TextEncoder().encode(t))
          const len = bytes.length
          result.push((len >> 24) & 0xff, (len >> 16) & 0xff, (len >> 8) & 0xff, len & 0xff, ...bytes)
        }
        return result
      }

      default:
        // Fallback: treat as beacon_string
        const fallback = argTokens.join(' ')
        if (!fallback) return []
        return encodeBeaconString(fallback)
    }
  }
}

/** Convert a server BofCommandMeta into a CommandDef and register it */
function registerBofCommand(meta: BofCommandMeta) {
  let parsedArgs: CommandArg[]
  try {
    parsedArgs = JSON.parse(meta.args_json)
  } catch {
    parsedArgs = []
  }

  const encoder = buildEncoder(meta, parsedArgs)
  const aliases = meta.aliases ? meta.aliases.split(',').map(a => a.trim()).filter(Boolean) : []

  const executeFn = meta.destructive
    ? async (tokens: string[], ctx: CommandContext) => {
        if (!tokens.includes('--force')) {
          ctx.pushLine(`⚠ Destructive command "${meta.cmd_name}" requires --force flag.`, 'error')
          ctx.pushLine(`Usage: ${meta.cmd_name} --force`, 'info')
          return
        }
        await bofExecute(meta.bof_name, meta.plugin_name || '', encoder)(tokens, ctx)
      }
    : bofExecute(meta.bof_name, meta.plugin_name || '', encoder)

  def({
    name: meta.cmd_name,
    aliases,
    description: meta.description || `Execute ${meta.bof_name}`,
    category: 'bof',
    type: 'bof',
    bofName: meta.bof_name,
    args: parsedArgs,
    requiresTarget: true,
    destructive: meta.destructive,
    execute: executeFn
  })
}

/** Remove all dynamically loaded BOF commands from the registry */
function clearBofCommands() {
  // Keep only non-bof commands (system + manage)
  const keep = commands.filter(c => c.type !== 'bof')
  commands.length = 0
  commands.push(...keep)
}

let _bofLoaded = false

/** Load BOF commands from server API. Call once on startup. */
export async function loadBofCommands(_appStore?: any): Promise<void> {
  try {
    const res = await api.get('/bof/commands')
    if (res.data.success && Array.isArray(res.data.data)) {
      clearBofCommands()
      for (const meta of res.data.data as BofCommandMeta[]) {
        if (!meta.enabled) continue
        // Skip if name conflicts with a system/manage command
        if (findCommand(meta.cmd_name)) continue
        registerBofCommand(meta)
      }
      _bofLoaded = true
    }
  } catch {
    // Silently fail — commands will be unavailable but system commands still work
    console.warn('[XStrike] Failed to load BOF commands from server')
  }

  // Also load plugin commands from manifests
  try {
    const pRes = await api.get('/plugins/manifests', { silentError: true } as any)
    if (pRes.data.success && Array.isArray(pRes.data.data)) {
      for (const manifest of pRes.data.data) {
        if (!manifest.commands) continue
        for (const cmd of manifest.commands) {
          if (!cmd.name || !cmd.action) continue
          if (findCommand(cmd.name)) continue // don't overwrite existing
          if (cmd.action.type === 'execute_bof' && cmd.action.bof_name) {
            const meta: BofCommandMeta = {
              bof_name: cmd.action.bof_name,
              cmd_name: cmd.name,
              aliases: (cmd.aliases || []).join(','),
              description: cmd.description || `Plugin command: ${cmd.name}`,
              category: 'bof',
              args_json: JSON.stringify(cmd.args || []),
              encode_type: cmd.action.encode_type || 'beacon_string',
              destructive: false,
              help_extra: '',
              enabled: true,
              plugin_name: manifest.name,
            }
            registerBofCommand(meta)
          }
        }
      }
    }
  } catch {
    // Silently fail
  }
}

/** Reload BOF commands (call after upload/delete/edit) */
export async function reloadBofCommands(appStore?: any): Promise<void> {
  await loadBofCommands(appStore)
}

/** Check if BOF commands have been loaded */
export function isBofCommandsLoaded(): boolean {
  return _bofLoaded
}

// ─── Registry lookup ────────────────────────────────────────────────────

export function findCommand(name: string): CommandDef | undefined {
  const lower = name.toLowerCase()
  return commands.find(c =>
    c.name === lower || c.aliases.includes(lower)
  )
}

export function getAllCommands(): CommandDef[] {
  return [...commands]
}

export function getCommandNames(): string[] {
  const names: string[] = []
  for (const c of commands) {
    names.push(c.name)
    names.push(...c.aliases)
  }
  return names
}

function showCommandHelp(cmd: CommandDef, ctx: CommandContext) {
  ctx.pushLine(`\n  ${cmd.name} — ${cmd.description}`, 'info')
  if (cmd.aliases.length) {
    ctx.pushLine(`  Aliases: ${cmd.aliases.join(', ')}`)
  }
  const argStr = cmd.args.map(a => a.required ? `<${a.name}>` : `[${a.name}]`).join(' ')
  ctx.pushLine(`  Usage: ${cmd.name} ${argStr}`)
  if (cmd.args.length) {
    ctx.pushLine('  Arguments:')
    for (const a of cmd.args) {
      ctx.pushLine(`    ${a.name.padEnd(14)} ${a.required ? '(required)' : '(optional)'}  ${a.description}`)
    }
  }
  if (cmd.requiresTarget) {
    ctx.pushLine('  ⚠ Requires a target beacon (use "use <node_id>" first)')
  }
  if (cmd.destructive) {
    ctx.pushLine('  ⚠ Destructive — requires --force flag')
  }
  if (cmd.bofName) {
    ctx.pushLine(`  BOF: ${cmd.bofName}`)
  }
  ctx.pushLine('')
}

/** Main dispatcher: parse input, find command, check target, execute */
export async function dispatch(rawInput: string, ctx: CommandContext): Promise<'clear' | void> {
  const tokens = tokenize(rawInput)
  if (!tokens.length) return

  const cmdName = tokens[0].toLowerCase()

  // Special: clear returns signal to wipe lines
  if (cmdName === 'clear') return 'clear'

  const cmd = findCommand(cmdName)
  if (!cmd) {
    ctx.pushLine(`Unknown command: ${cmdName}. Type "help" for available commands.`, 'error')
    return
  }

  // -h flag anywhere → show help
  if (tokens.includes('-h') || tokens.includes('--help')) {
    showCommandHelp(cmd, ctx)
    return
  }

  // Check target requirement
  if (cmd.requiresTarget && !ctx.targetId) {
    ctx.pushLine(`Command "${cmd.name}" requires a target beacon.`, 'error')
    ctx.pushLine('Use "use <node_id>" to select a target first.', 'info')
    return
  }

  // Validate target is alive for BOF commands
  if (cmd.requiresTarget && ctx.targetId) {
    const beacon = ctx.appStore.beacons.find(
      (b: any) => (b.node_id || b.id) === ctx.targetId
    )
    if (!beacon) {
      ctx.pushLine(`Target beacon ${ctx.targetId} not found. Clearing target.`, 'error')
      ctx.setTarget(null)
      return
    }
  }

  await cmd.execute(tokens, ctx)
}
