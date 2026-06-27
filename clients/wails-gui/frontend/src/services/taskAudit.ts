import api from './api'

export type TaskAuditPayload = {
  source: string
  nodeId: string
  input: string
}

export const auditTaskInput = async (payload: TaskAuditPayload) => {
  const line = `[${payload.source}] node=${payload.nodeId} input=${payload.input}`
  window.dispatchEvent(new CustomEvent('ghost:task-input', { detail: { ...payload, line } }))
  try {
    await api.post('/logs', { message: line, level: 'task' }, { silentError: true } as any)
  } catch {
    // best effort
  }
}

