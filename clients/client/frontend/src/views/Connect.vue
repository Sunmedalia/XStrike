<template>
  <div class="connect-overlay">
    <div class="connect-box">
      <div class="connect-brand">
        <img :src="logoSrc" alt="XStrike" class="brand-gif" />
        <div class="mark">XStrike</div>
        <div class="subtitle">Remote Client</div>
      </div>

      <!-- Mode toggle -->
      <div class="mode-toggle">
        <button
          class="mode-btn"
          :class="{ active: mode === 'remote' }"
          @click="mode = 'remote'"
        >
          Remote Server
        </button>
        <button
          class="mode-btn"
          :class="{ active: mode === 'local' }"
          @click="mode = 'local'"
        >
          Local Server
        </button>
      </div>

      <!-- Remote connection form -->
      <form v-if="mode === 'remote'" @submit.prevent="handleConnect" class="connect-form">
        <div class="fg">
          <label>Server Host / IP</label>
          <input
            v-model="host"
            placeholder="e.g. 192.168.1.100 or c2.example.com"
            autocomplete="off"
            autofocus
            required
          />
        </div>
        <div class="fg">
          <label>Server Port</label>
          <input
            v-model.number="port"
            type="number"
            min="1"
            max="65535"
            placeholder="8080"
            required
          />
        </div>
        <div class="fg">
          <label>Random Path / Prefix</label>
          <input
            v-model="path"
            placeholder="e.g. a3f8b2c1 or /a3f8b2c1"
            autocomplete="off"
          />
          <div class="fg-help">Optional. Use this when the server is exposed behind a random management path.</div>
        </div>
        <div class="fg">
          <label>Username</label>
          <input v-model="username" autocomplete="username" required />
        </div>
        <div class="fg">
          <label>Password</label>
          <input v-model="password" type="password" autocomplete="current-password" required />
        </div>
        <label class="remember-label">
          <input type="checkbox" v-model="remember" />
          Remember connection
        </label>
        <button class="connect-btn" type="submit" :disabled="loading">
          {{ loading ? 'Connecting...' : 'Connect' }}
        </button>
      </form>

      <!-- Local mode: just go straight to login -->
      <div v-else class="local-section">
        <p class="local-desc">
          Connect to the local XStrike server running on this machine.
        </p>
        <button class="connect-btn" @click="goLocal">Enter Local Mode</button>
      </div>

      <div v-if="error" class="connect-err">{{ error }}</div>

      <!-- Saved connections -->
      <div v-if="mode === 'remote' && savedHost" class="saved-hint">
        Last: <strong>{{ savedHost }}:{{ savedPort }}{{ savedPath }}</strong>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref, onMounted } from 'vue'
import { useRouter } from 'vue-router'
import { normalizeRemotePath, useConnectionStore } from '../stores/connection'
import { asset } from '../runtime/env'
import axios from 'axios'
import { isDesktop } from '../runtime/platform'
import { MOCK_TOKEN, setMockMode } from '../services/mockMode'

const router = useRouter()
const connStore = useConnectionStore()

const mode = ref<'remote' | 'local'>('remote')
const host = ref(connStore.host || '')
const port = ref(connStore.port || 8080)
const path = ref(connStore.path || '')
const username = ref(connStore.username || '')
const password = ref('')
const remember = ref(connStore.remember ?? true)
const loading = ref(false)
const error = ref('')
const logoSrc = computed(() => {
  const theme = document.documentElement.getAttribute('data-theme') || localStorage.getItem('ghost-theme')
  return asset(theme === 'light' ? '/ico/xstrike-icon-light.png' : '/ico/xstrike-icon-dark.png')
})

const savedHost = ref(connStore.host || '')
const savedPort = ref(connStore.port || 8080)
const savedPath = ref(connStore.path || '')

onMounted(() => {
  // If already connected with valid token, skip to dashboard
  const token = localStorage.getItem('token')
  if (token && connStore.connected) {
    router.replace('/')
  }
})

async function handleConnect() {
  loading.value = true
  error.value = ''
  if (isDesktop()) {
    setMockMode(false)
    if (localStorage.getItem('token') === MOCK_TOKEN) {
      localStorage.removeItem('token')
    }
  }

  const scheme = port.value === 443 ? 'https' : 'http'
  const remotePath = normalizeRemotePath(path.value)
  const baseUrl = `${scheme}://${host.value}:${port.value}${remotePath}`
  const target = `${host.value}:${port.value}${remotePath}`

  try {
    // Test connection + authenticate against the remote server
    const res = await axios.post(`${baseUrl}/api/auth/login`, {
      username: username.value,
      password: password.value,
    }, { timeout: 10000 })

    if (res.data.success && res.data.data?.token) {
      // Save connection state
      connStore.setRemote(host.value, port.value, username.value, remember.value, remotePath)
      localStorage.setItem('token', res.data.data.token)

      // Navigate to dashboard
      router.push('/')
    } else {
      error.value = res.data.error || 'Authentication failed'
    }
  } catch (err: any) {
    if (err.code === 'ECONNABORTED' || err.message?.includes('timeout')) {
      error.value = `Connection timed out — is the server running at ${target}?`
    } else if (err.response?.status === 401) {
      error.value = 'Invalid username or password'
    } else if (err.response?.data?.error) {
      error.value = err.response.data.error
    } else if (err.message?.includes('Network Error')) {
      error.value = `Cannot reach server at ${target} — check address, path, and firewall`
    } else {
      error.value = err.message || 'Connection failed'
    }
  } finally {
    loading.value = false
  }
}

function goLocal() {
  connStore.setLocal()
  if (isDesktop()) {
    setMockMode(false)
    if (localStorage.getItem('token') === MOCK_TOKEN) {
      localStorage.removeItem('token')
    }
  }
  // In local mode, check if already has a token
  const token = localStorage.getItem('token')
  if (token) {
    router.push('/')
  } else {
    router.push('/login')
  }
}
</script>

<style scoped>
.connect-overlay {
  position: fixed;
  inset: 0;
  background: var(--bg);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 9999;
}
.connect-box {
  background: var(--bg-2);
  border: 1px solid var(--bd);
  border-radius: 8px;
  padding: 32px;
  width: 360px;
  box-shadow: 0 10px 30px rgba(0, 0, 0, 0.5);
}
.connect-brand {
  text-align: center;
  margin-bottom: 24px;
}
.brand-gif {
  width: 56px;
  height: 56px;
  object-fit: contain;
  margin-bottom: 8px;
}
.mark {
  font-family: var(--font-mono);
  font-size: 26px;
  font-weight: 800;
  letter-spacing: -1px;
  color: var(--pri);
}
.subtitle {
  font-size: 11px;
  color: var(--tx-3);
  text-transform: uppercase;
  letter-spacing: 0.15em;
  margin-top: 4px;
}

/* Mode toggle */
.mode-toggle {
  display: flex;
  gap: 0;
  margin-bottom: 20px;
  border: 1px solid var(--bd);
  border-radius: 4px;
  overflow: hidden;
}
.mode-btn {
  flex: 1;
  padding: 8px 12px;
  font-size: 11px;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  background: var(--bg-3);
  color: var(--tx-3);
  border: none;
  cursor: pointer;
  transition: background 0.15s, color 0.15s;
}
.mode-btn.active {
  background: var(--pri);
  color: #062235;
}
.mode-btn:not(.active):hover {
  background: var(--bg-4);
  color: var(--tx);
}

/* Form fields */
.connect-form {
  display: flex;
  flex-direction: column;
  gap: 2px;
}
.fg {
  margin-bottom: 12px;
}
.fg label {
  display: block;
  font-size: 11px;
  color: var(--tx-2);
  margin-bottom: 5px;
}
.fg input,
.fg select {
  width: 100%;
  background: var(--bg-3);
  border: 1px solid var(--bd);
  color: var(--tx);
  padding: 8px 12px;
  border-radius: 4px;
  outline: none;
  font-size: 13px;
  box-sizing: border-box;
}
.fg input:focus {
  border-color: var(--pri);
}
.fg-help {
  margin-top: 6px;
  font-size: 11px;
  color: var(--tx-4);
  line-height: 1.4;
}
.fg input[type='number'] {
  -moz-appearance: textfield;
}
.fg input[type='number']::-webkit-outer-spin-button,
.fg input[type='number']::-webkit-inner-spin-button {
  -webkit-appearance: none;
}

.remember-label {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 12px;
  color: var(--tx-3);
  margin-bottom: 16px;
  cursor: pointer;
}
.remember-label input[type='checkbox'] {
  accent-color: var(--pri);
}

.connect-btn {
  width: 100%;
  background: var(--pri);
  color: #062235;
  border: none;
  padding: 10px;
  border-radius: 4px;
  font-weight: 600;
  font-size: 13px;
  cursor: pointer;
  margin-top: 4px;
}
.connect-btn:hover {
  background: var(--pri-h);
}
.connect-btn:disabled {
  opacity: 0.6;
  cursor: not-allowed;
}

.connect-err {
  color: var(--red);
  font-size: 12px;
  text-align: center;
  margin-top: 16px;
  line-height: 1.4;
}

.saved-hint {
  font-size: 11px;
  color: var(--tx-4);
  text-align: center;
  margin-top: 12px;
}
.saved-hint strong {
  color: var(--tx-3);
}

/* Local mode */
.local-section {
  text-align: center;
}
.local-desc {
  font-size: 13px;
  color: var(--tx-3);
  margin-bottom: 20px;
  line-height: 1.5;
}
</style>
