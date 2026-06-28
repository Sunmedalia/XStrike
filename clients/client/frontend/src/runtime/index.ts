export { isDesktop, isWeb, getPlatform } from './platform'
export type { Platform } from './platform'
export {
  getServerOrigin,
  getApiBaseUrl,
  getApiUrl,
  getWsUrl,
  getDefaultCallbackHost,
  getAuthHeaders,
} from './env'
export { apiFetch, downloadBlob } from './network'
export { useConnectionStore } from '../stores/connection'
export type { ConnectionMode, ConnectionInfo } from '../stores/connection'
