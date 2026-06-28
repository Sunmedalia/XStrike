/**
 * Mock data for the desktop / demo console.
 *
 * Centralised here so UI components never hard-code data. Every entry mirrors
 * the shape the real `/api` would return, so swapping `mockData` for live
 * `api` calls in `stores/app.ts` is a one-line change per action.
 *
 * Sessions follow the RustStrike Control Desk sample set (WKSTN-042, SRV-DB-01,
 * …). The shape matches what `normalizeBeacon()` in `stores/app.ts` expects:
 * each item is `{ engine, node_info: {…} }`.
 */

const now = Math.floor(Date.now() / 1000)
const ago = (s: number) => now - s

export interface MockBeacon {
  engine: string
  node_info: {
    node_id: string
    hostname: string
    ip: string
    external_ip: string
    process: string
    process_name?: string
    pid: number
    os: string
    arch: string
    user: string
    domain: string
    privilege: 'User' | 'Admin' | 'System' | 'Root'
    last_seen: number
    listener: string
    checkin_interval: number
    started_at: number
    cwd: string
    integrity: number
  }
}

export const mockBeacons: MockBeacon[] = [
  {
    engine: 'windows',
    node_info: {
      node_id: 'WKSTN-042', hostname: 'WKSTN-042', ip: '10.0.4.21',
      external_ip: '203.0.113.21', process: 'explorer.exe', pid: 4822,
      os: 'Windows 11', arch: 'x64', user: 'alice', domain: 'CORP',
      privilege: 'User', last_seen: ago(8), listener: 'http-443',
      checkin_interval: 30, started_at: ago(7200), cwd: 'C:\\Users\\alice',
      integrity: 1,
    },
  },
  {
    engine: 'windows',
    node_info: {
      node_id: 'SRV-DB-01', hostname: 'SRV-DB-01', ip: '10.0.2.15',
      external_ip: '203.0.113.15', process: 'sqlservr.exe', pid: 1190,
      os: 'Windows Server 2022', arch: 'x64', user: 'svc_backup', domain: 'CORP',
      privilege: 'Admin', last_seen: ago(140), listener: 'http-443',
      checkin_interval: 60, started_at: ago(86400), cwd: 'C:\\Windows\\System32',
      integrity: 2,
    },
  },
  {
    engine: 'linux',
    node_info: {
      node_id: 'DEV-LINUX-7', hostname: 'DEV-LINUX-7', ip: '10.0.8.33',
      external_ip: '203.0.113.33', process: 'bash', pid: 9981,
      os: 'Ubuntu 24.04', arch: 'x64', user: 'devops', domain: '',
      privilege: 'User', last_seen: ago(12), listener: 'tcp-4444',
      checkin_interval: 30, started_at: ago(5400), cwd: '/home/devops',
      integrity: 1,
    },
  },
  {
    engine: 'macos',
    node_info: {
      node_id: 'MACBOOK-M2', hostname: 'MACBOOK-M2', ip: '10.0.5.44',
      external_ip: '203.0.113.44', process: 'Terminal', pid: 2240,
      os: 'macOS 15', arch: 'arm64', user: 'j.chen', domain: '',
      privilege: 'User', last_seen: ago(3600), listener: 'http-443',
      checkin_interval: 60, started_at: ago(7200), cwd: '/Users/jchen',
      integrity: 1,
    },
  },
  {
    engine: 'windows',
    node_info: {
      node_id: 'FINANCE-013', hostname: 'FINANCE-013', ip: '10.0.6.18',
      external_ip: '203.0.113.18', process: 'chrome.exe', pid: 7765,
      os: 'Windows 10', arch: 'x64', user: 'maria', domain: 'CORP',
      privilege: 'User', last_seen: ago(20), listener: 'http-443',
      checkin_interval: 30, started_at: ago(10800), cwd: 'C:\\Users\\maria',
      integrity: 1,
    },
  },
  {
    engine: 'linux',
    node_info: {
      node_id: 'BUILD-SRV-2', hostname: 'BUILD-SRV-2', ip: '10.0.9.12',
      external_ip: '203.0.113.12', process: 'systemd', pid: 1,
      os: 'Debian 12', arch: 'x64', user: 'builder', domain: '',
      privilege: 'Root', last_seen: ago(210), listener: 'tcp-4444',
      checkin_interval: 60, started_at: ago(172800), cwd: '/root',
      integrity: 3,
    },
  },
  {
    engine: 'windows',
    node_info: {
      node_id: 'QA-LAPTOP-5', hostname: 'QA-LAPTOP-5', ip: '10.0.3.74',
      external_ip: '203.0.113.74', process: 'powershell.exe', pid: 3310,
      os: 'Windows 11', arch: 'x64', user: 'tester', domain: 'CORP',
      privilege: 'Admin', last_seen: ago(5), listener: 'http-443',
      checkin_interval: 30, started_at: ago(3600), cwd: 'C:\\Users\\tester',
      integrity: 2,
    },
  },
  {
    engine: 'linux',
    node_info: {
      node_id: 'EDGE-NODE-1', hostname: 'EDGE-NODE-1', ip: '10.0.7.90',
      external_ip: '203.0.113.90', process: 'nginx', pid: 418,
      os: 'Ubuntu 22.04', arch: 'x64', user: 'edge', domain: '',
      privilege: 'Root', last_seen: ago(900), listener: 'tcp-4444',
      checkin_interval: 60, started_at: ago(259200), cwd: '/etc/nginx',
      integrity: 3,
    },
  },
]

export const mockListeners = [
  { id: 'L-001', name: 'http-443', protocol: 'http', host: '0.0.0.0', port: 443, status: 'running' },
  { id: 'L-002', name: 'tcp-4444', protocol: 'tcp', host: '0.0.0.0', port: 4444, status: 'running' },
  { id: 'L-003', name: 'https-8443', protocol: 'https', host: '0.0.0.0', port: 8443, status: 'stopped' },
]

export const mockBofs = [
  { name: 'whoami', size: 2048 },
  { name: 'cmd_exec', size: 8192 },
  { name: 'nbtscan', size: 12288 },
  { name: 'ipconfig', size: 4096 },
  { name: 'ps', size: 6144 },
]

export interface MockLog {
  created_at: number
  level: 'info' | 'task' | 'warn' | 'error' | 'system'
  node_id: string
  data: string
}

const base = now - 600
export const mockLogs: MockLog[] = [
  { created_at: base + 0, level: 'info', node_id: 'WKSTN-042', data: 'Session WKSTN-042 checked in' },
  { created_at: base + 8, level: 'task', node_id: 'WKSTN-042', data: 'whoami completed on WKSTN-042' },
  { created_at: base + 54, level: 'warn', node_id: 'SRV-DB-01', data: 'SRV-DB-01 missed expected check-in' },
  { created_at: base + 92, level: 'info', node_id: 'DEV-LINUX-7', data: 'File upload queued for DEV-LINUX-7' },
  { created_at: base + 120, level: 'task', node_id: 'QA-LAPTOP-5', data: 'ipconfig completed on QA-LAPTOP-5' },
  { created_at: base + 200, level: 'error', node_id: 'EDGE-NODE-1', data: 'EDGE-NODE-1 beacon error: pipe closed' },
  { created_at: base + 260, level: 'info', node_id: 'FINANCE-013', data: 'Session FINANCE-013 checked in' },
]

/**
 * In-memory event log used by mock task / upload submissions so the Event
 * Stream updates when the operator queues work in demo mode.
 */
export const mockEventLog: MockLog[] = [...mockLogs]

let mockSeq = 1000
export function pushMockEvent(level: MockLog['level'], node_id: string, data: string) {
  mockEventLog.unshift({
    created_at: Math.floor(Date.now() / 1000),
    level,
    node_id,
    data,
  })
  if (mockEventLog.length > 500) mockEventLog.length = 500
  mockSeq++
}

/** Simulated network latency for refresh loading states (~800ms per spec). */
export const MOCK_LATENCY_MS = 800
