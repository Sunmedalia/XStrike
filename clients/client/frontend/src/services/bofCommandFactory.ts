import { buildBofArgsEncoder, type BofArgsEncoder, type BofCommandMeta } from './bofCommandEncoding'
import { queueBofTask } from './tasks'
import type { CommandArg, CommandContext, CommandDef } from './commandRegistry'

function bofExecute(
  bofName: string,
  pluginName: string,
  argsEncoder: BofArgsEncoder
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
    if (args === null) return
    ctx.setLoading(true)
    try {
      const taskId = await queueBofTask({
        nodeId: ctx.targetId || '',
        bof: { name: bofName, plugin_name: pluginName },
        args: args.length > 0 ? args : undefined,
        source: `console:${tokens[0]}`,
        auditInput: tokens.join(' ')
      })
      ctx.setTaskBanner(taskId)
      await ctx.pollTask(taskId)
    } catch {
      ctx.pushLine('BOF execution failed.', 'error')
    } finally {
      ctx.setLoading(false)
    }
  }
}

function parseCommandArgs(argsJson: string): CommandArg[] {
  try {
    return JSON.parse(argsJson)
  } catch {
    return []
  }
}

export function createBofCommand(meta: BofCommandMeta): CommandDef {
  const parsedArgs = parseCommandArgs(meta.args_json)
  const encoder = buildBofArgsEncoder(meta, parsedArgs)
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

  return {
    name: meta.cmd_name,
    aliases,
    description: meta.description || `Execute ${meta.bof_name}`,
    category: 'bof',
    type: 'bof',
    bofName: meta.bof_name,
    args: parsedArgs,
    requiresTarget: true,
    destructive: meta.destructive,
    execute: executeFn,
  }
}
