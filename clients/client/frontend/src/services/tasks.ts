import api from './api'
import { auditTaskInput } from './taskAudit'

export interface BofEntryLike {
  name: string
  plugin_name?: string
}

export interface TaskResult {
  id?: string
  status?: string
  output?: string
  error?: string
  success: boolean
}

export interface PollTaskOptions {
  maxRetry?: number
  intervalMs?: number
}

export interface RunBofOptions {
  nodeId: string | number
  bof: BofEntryLike
  args?: number[]
  source?: string
  auditInput?: string
}

export function findBofByPattern(bofs: BofEntryLike[], pattern: RegExp): BofEntryLike | undefined {
  return bofs.find(b => pattern.test(b.name))
}

export async function pollTaskResult(
  taskId: string,
  { maxRetry = 120, intervalMs = 1000 }: PollTaskOptions = {}
): Promise<TaskResult> {
  for (let i = 0; i < maxRetry; i++) {
    try {
      const res = await api.get(`/tasks/${taskId}`, {
        silentError: true,
        validateStatus: (s: number) => s === 200 || s === 404
      } as any)
      if (res.status !== 404 && res.data?.success && res.data?.data) {
        return res.data.data as TaskResult
      }
    } catch (err: any) {
      if (err?.response?.status !== 404) throw err
    }
    await new Promise(resolve => setTimeout(resolve, intervalMs))
  }
  throw new Error('Task timeout')
}

export async function queueBofTask({ nodeId, bof, args, source, auditInput }: RunBofOptions): Promise<string> {
  if (source) {
    await auditTaskInput({
      source,
      nodeId: String(nodeId),
      input: auditInput ?? ''
    })
  }

  const payload: any = {
    node_id: nodeId,
    bof_name: bof.name,
    plugin_name: bof.plugin_name || ''
  }
  if (args) payload.args = args

  const res = await api.post('/bof/execute', payload)
  if (!res.data?.success || !res.data?.data) {
    throw new Error(res.data?.error || 'Failed to queue BOF task')
  }
  return String(res.data.data)
}

export async function runBofTask(
  options: RunBofOptions,
  pollOptions?: PollTaskOptions
): Promise<TaskResult> {
  const taskId = await queueBofTask(options)
  return pollTaskResult(taskId, pollOptions)
}
