import { encodeBeaconString } from './bofEncoding'
import type { CommandArg, CommandContext } from './commandRegistry'

export interface BofCommandMeta {
  bof_name: string
  cmd_name: string
  aliases: string
  description: string
  category: string
  args_json: string
  encode_type: string
  destructive: boolean
  help_extra: string
  enabled: boolean
  plugin_name?: string
}

export type BofArgsEncoder = (tokens: string[], ctx: CommandContext) => number[] | null

function encodeRawBytes(hex: string): number[] {
  const cleaned = hex.replace(/\s+/g, '')
  const bytes: number[] = []
  for (let i = 0; i < cleaned.length; i += 2) {
    const b = parseInt(cleaned.substring(i, i + 2), 16)
    if (isNaN(b)) throw new Error(`Invalid hex at position ${i}: "${cleaned.substring(i, i + 2)}"`)
    bytes.push(b)
  }
  return bytes
}

function encodeCsPackedBlob(input: string): number[] {
  const bytes = Array.from(new TextEncoder().encode(input))
  const len = bytes.length
  return [(len >> 24) & 0xff, (len >> 16) & 0xff, (len >> 8) & 0xff, len & 0xff, ...bytes]
}

export function buildBofArgsEncoder(meta: BofCommandMeta, parsedArgs: CommandArg[]): BofArgsEncoder {
  const { encode_type, cmd_name } = meta

  return (tokens: string[], ctx: CommandContext) => {
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
        return input ? encodeBeaconString(input) : []
      }

      case 'raw_string': {
        const input = argTokens.join(' ')
        return input ? Array.from(new TextEncoder().encode(input)) : []
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
        const hexStr = argTokens.join(' ')
        if (!hexStr) {
          ctx.pushLine(`Usage: ${cmd_name} <hex bytes>`, 'error')
          return null
        }
        try {
          const raw = encodeRawBytes(hexStr)
          const len = raw.length
          if (len > 0xffff) {
            ctx.pushLine('Shellcode too large for 2-byte length (>65535 bytes)', 'error')
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
        const input = argTokens.join(' ')
        return input ? encodeCsPackedBlob(input) : []
      }

      case 'cs_packed_multi': {
        const result: number[] = []
        for (const t of argTokens) {
          result.push(...encodeCsPackedBlob(t))
        }
        return result
      }

      default: {
        const fallback = argTokens.join(' ')
        return fallback ? encodeBeaconString(fallback) : []
      }
    }
  }
}
