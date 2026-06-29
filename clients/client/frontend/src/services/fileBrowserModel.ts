export interface FileEntry {
  name: string
  type?: string
  kind?: string
  is_dir?: boolean
  size?: number
  modified?: string
}

export interface ParsedFileList {
  cwd: string
  rows: FileEntry[]
}

export function normalizeWindowsPath(path: string): string {
  let s = (path || '').replace(/\//g, '\\').trim()
  if (!s) return 'C:\\'
  if (s.length >= 2 && s[1] === ':' && /[a-z]/.test(s[0])) {
    s = s[0].toUpperCase() + s.slice(1)
  }
  s = s.replace(/\\+$/, '')
  if (s.length === 2 && s[1] === ':') s = s + '\\'
  else if (s.length > 0) s = s + '\\'
  return s
}

export function getParentPath(path: string): string {
  const p = path.replace(/\\+$/, '')
  const parts = p.split('\\').filter(Boolean)
  if (parts.length <= 1) return ''
  parts.pop()
  return parts.join('\\') + '\\'
}

export function hasParentPath(path: string): boolean {
  return path.split('\\').filter(Boolean).length > 1
}

export function getPathDisplayName(path: string): string {
  const p = path.replace(/\\+$/, '')
  const parts = p.split('\\').filter(Boolean)
  return parts[parts.length - 1] || parts[0] || 'Root'
}

export function isDirectoryEntry(file: FileEntry): boolean {
  const marker = String(file?.type || file?.kind || '').trim().toUpperCase()
  return Boolean(file?.is_dir || marker === 'D' || marker === 'DIR' || marker === 'DIRECTORY' || marker === '<DIR>')
}

export function formatFileSize(bytes: number): string {
  if (!bytes) return '0 B'
  const k = 1024
  const sizes = ['B', 'KB', 'MB', 'GB']
  const i = Math.floor(Math.log(bytes) / Math.log(k))
  return `${parseFloat((bytes / Math.pow(k, i)).toFixed(1))} ${sizes[i]}`
}

export function parseFileListOutput(output: string): ParsedFileList {
  const rows: FileEntry[] = []
  let cwd = ''
  const lines = output.split('\n').map(line => line.trim()).filter(Boolean)
  for (const line of lines) {
    if (line.startsWith('CWD:')) {
      cwd = line.slice(4).trim()
      continue
    }
    const parts = line.split('\t')
    if (parts.length < 4) continue
    const [type, name, sizeRaw, epochRaw] = parts
    const marker = type.trim().toUpperCase()
    if (name === '.' || name === '..') continue
    const epoch = Number(epochRaw) || 0
    rows.push({
      name,
      type: marker,
      is_dir: marker === 'D' || marker === 'DIR' || marker === 'DIRECTORY' || marker === '<DIR>',
      size: Number(sizeRaw) || 0,
      modified: epoch ? new Date(epoch * 1000).toLocaleString() : ''
    })
  }
  return { cwd, rows }
}
