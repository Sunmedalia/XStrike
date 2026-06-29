import type { PluginAction } from '../stores/plugin'
import { encodeBeaconString } from './bofEncoding'
import { pollTaskResult, queueBofTask, type TaskResult } from './tasks'

export type PluginFormData = Record<string, unknown>

export function buildPluginArgs(action: PluginAction, formData: PluginFormData = {}): number[] | undefined {
  if (!action.args_map || typeof action.args_map !== 'object') return undefined
  const entries = Object.entries(action.args_map).sort((a, b) => a[1] - b[1])
  const args: number[] = []
  for (const [fieldName] of entries) {
    args.push(...encodeBeaconString(String(formData[fieldName] || '')))
  }
  return args.length ? args : undefined
}

export function buildPluginAuditInput(formData: PluginFormData = {}): string {
  const input = Object.entries(formData)
    .map(([key, value]) => `${key}=${String(value)}`)
    .join(' ')
    .trim()
  return input || '(none)'
}

export async function queuePluginBofAction(
  action: PluginAction,
  targetId: string,
  formData: PluginFormData = {}
): Promise<string> {
  if (!action.bof_name) throw new Error('No BOF name specified in action')
  return queueBofTask({
    nodeId: targetId,
    bof: { name: action.bof_name, plugin_name: action.plugin_name || '' },
    args: buildPluginArgs(action, formData),
    source: `plugin:${action.bof_name}`,
    auditInput: buildPluginAuditInput(formData),
  })
}

export async function runPluginBofAction(
  action: PluginAction,
  targetId: string,
  formData: PluginFormData = {}
): Promise<TaskResult> {
  const taskId = await queuePluginBofAction(action, targetId, formData)
  return pollTaskResult(taskId, { maxRetry: 60 })
}
