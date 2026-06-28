<template>
  <div class="dusk-login" :class="{ light: isLight }">
    <section class="login-card">
      <button class="theme-toggle" @click="toggleTheme" :title="isLight ? 'Dark' : 'Light'">
        <Moon v-if="isLight" :size="16" />
        <Sun v-else :size="16" />
      </button>
      <div class="brand">
        <img :src="logoSrc" alt="" class="logo" />
        <div class="name">XStrike</div>
      </div>
      <form @submit.prevent="handleLogin">
        <div class="field">
          <label>Username</label>
          <div class="input-wrap">
            <User :size="15" />
            <input v-model="username" autocomplete="username" autofocus required />
          </div>
        </div>
        <div class="field">
          <label>Password</label>
          <div class="input-wrap">
            <LockKeyhole :size="15" />
            <input v-model="password" type="password" autocomplete="current-password" required />
          </div>
        </div>
        <button class="submit" type="submit" :disabled="loading">
          {{ loading ? 'Signing in…' : 'Sign In' }}
        </button>
      </form>
      <div v-if="error" class="err">{{ error }}</div>
      <button type="button" class="demo" @click="enterDemo">Enter demo console</button>
    </section>
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue'
import { useRouter } from 'vue-router'
import { LockKeyhole, Moon, Sun, User } from 'lucide-vue-next'
import { useConnectionStore } from '../stores/connection'
import api from '../services/api'
import { asset } from '../services/base'

const THEME_KEY = 'ghost-theme'
const isLight = ref(localStorage.getItem(THEME_KEY) === 'light')
const logoSrc = computed(() => asset(isLight.value ? '/ico/xstrike-icon-light.png' : '/ico/xstrike-icon-dark.png'))
const toggleTheme = () => {
  isLight.value = !isLight.value
  localStorage.setItem(THEME_KEY, isLight.value ? 'light' : 'dark')
  document.documentElement.setAttribute('data-theme', isLight.value ? 'light' : 'dark')
}

const username = ref('')
const password = ref('')
const loading = ref(false)
const error = ref('')
const router = useRouter()
const connStore = useConnectionStore()

const handleLogin = async () => {
  loading.value = true
  error.value = ''
  try {
    const res = await api.post('/auth/login', { username: username.value, password: password.value })
    if (res.data.success) {
      localStorage.setItem('token', res.data.data.token)
      // Mark connected if in remote mode
      if (connStore.isRemote) {
        connStore.markConnected()
      }
      router.push('/')
    }
  } catch (err: any) {
    error.value = err.response?.data?.error || 'Login failed'
  } finally {
    loading.value = false
  }
}

// Skip auth entirely and boot into the mock-data console.
const enterDemo = () => {
  localStorage.setItem('ghost-demo', '1')
  router.push('/')
}
</script>

<style scoped>
.dusk-login {
  --bg:    #151922;
  --panel: #1d232d;
  --field: #242b36;
  --bd:    rgba(191, 203, 218, 0.14);
  --tx:    #edf1f6;
  --tx-2:  #aeb9c7;
  --tx-3:  #778394;
  --pri:   #4fc3ad;
  --pri-h: #3aaf98;
  --red:   #ee6b6b;
  --font-mono: 'JetBrains Mono', 'Fira Code', monospace;
  --font-sans: 'Inter', system-ui, -apple-system, sans-serif;

  position: fixed;
  inset: 0;
  background: var(--bg);
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 28px;
  font-family: var(--font-sans);
  color: var(--tx);
}
.dusk-login.light {
  --bg:    #eef1f5;
  --panel: #fbfcfe;
  --field: #f3f5f8;
  --bd:    rgba(43, 59, 79, 0.14);
  --tx:    #17202b;
  --tx-2:  #526173;
  --tx-3:  #8491a2;
  --pri:   #168b78;
  --pri-h: #117665;
  --red:   #c94c4c;
}

.login-card {
  position: relative;
  width: min(360px, 100%);
  padding: 34px 30px 28px;
  border: 1px solid var(--bd);
  border-radius: 12px;
  background: var(--panel);
  box-shadow: 0 24px 70px rgba(0, 0, 0, 0.28);
}

.logo {
  width: 72px;
  height: 72px;
  object-fit: contain;
  border-radius: 18px;
}
.name {
  font-family: var(--font-mono);
  font-size: 24px;
  font-weight: 700;
  color: var(--pri);
}

.brand {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 12px;
  margin-bottom: 28px;
}

.field {
  margin-bottom: 16px;
}
.field label {
  display: block;
  font-size: 11px;
  color: var(--tx-2);
  margin-bottom: 6px;
  letter-spacing: 0.02em;
}
.field input {
  width: 100%;
  background: transparent;
  border: none;
  color: var(--tx);
  outline: none;
  font-size: 13px;
}
.input-wrap {
  display: flex;
  align-items: center;
  gap: 9px;
  width: 100%;
  background: var(--field);
  border: 1px solid var(--bd);
  color: var(--tx-3);
  padding: 0 12px;
  height: 42px;
  border-radius: 8px;
  transition: border-color 0.15s, box-shadow 0.15s, background 0.15s;
}
.input-wrap:focus-within {
  border-color: var(--pri);
  box-shadow: 0 0 0 3px color-mix(in srgb, var(--pri) 16%, transparent);
}

.submit {
  width: 100%;
  background: var(--pri);
  color: #ffffff;
  border: none;
  height: 42px;
  border-radius: 8px;
  font-weight: 700;
  font-size: 13px;
  cursor: pointer;
  margin-top: 6px;
  transition: transform 0.15s, filter 0.15s, opacity 0.15s;
}
.submit:hover { background: var(--pri-h); transform: translateY(-1px); }
.submit:disabled { opacity: 0.6; cursor: not-allowed; }

.err {
  color: var(--red);
  font-size: 12px;
  text-align: center;
  margin-top: 14px;
}

.theme-toggle {
  position: absolute;
  top: 18px;
  right: 18px;
  width: 32px;
  height: 32px;
  display: grid;
  place-items: center;
  background: var(--field);
  border: 1px solid var(--bd);
  border-radius: 8px;
  color: var(--tx-2);
  cursor: pointer;
  transition: color 0.15s, border-color 0.15s;
}
.theme-toggle:hover { color: var(--tx); border-color: var(--pri); }

.demo {
  margin-top: 14px;
  width: 100%;
  background: transparent;
  border: 1px dashed var(--bd);
  border-radius: 8px;
  color: var(--tx-3);
  padding: 9px 10px;
  font-size: 12px;
  cursor: pointer;
  transition: color 0.15s, border-color 0.15s;
}
.demo:hover { color: var(--pri); border-color: var(--pri); }

</style>
