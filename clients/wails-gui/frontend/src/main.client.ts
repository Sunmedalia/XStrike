/**
 * Client-mode entry point.
 *
 * Identical to main.ts but forces remote-client mode:
 * - Hash-based routing (works from any static server / file://)
 * - Always starts at the Connect page
 * - No local-server assumption
 */

import { createApp } from 'vue'
import { createPinia } from 'pinia'
import { createRouter, createWebHashHistory } from 'vue-router'
import './assets/main.css'
import App from './App.vue'
import { useConnectionStore } from './stores/connection'
import { setRouter } from './services/api'

// ── Theme ──
const THEME_KEY = 'ghost-theme'
const savedTheme = localStorage.getItem(THEME_KEY)
document.documentElement.setAttribute(
  'data-theme',
  savedTheme === 'light' ? 'light' : 'dark'
)

// ── Pinia (must be created before router guards access stores) ──
const pinia = createPinia()

// ── Router (hash history — works on any static host / file://) ──
// Use lazy imports to avoid circular dependency:
//   Dashboard → stores → api.ts → router/index.ts → Dashboard (circular!)
const router = createRouter({
  history: createWebHashHistory(),
  routes: [
    { path: '/connect', name: 'Connect', component: () => import('./views/Connect.vue') },
    { path: '/login',   name: 'Login',   component: () => import('./views/Login.vue') },
    { path: '/',        name: 'Dashboard', component: () => import('./views/Dashboard.vue'), meta: { requiresAuth: true } },
  ],
})

router.beforeEach((to, _from, next) => {
  const token = localStorage.getItem('token')
  const connStore = useConnectionStore()

  if (to.meta.requiresAuth && !token) {
    next({ name: 'Connect' })
  } else if (to.name === 'Login' && !connStore.connected) {
    next({ name: 'Connect' })
  } else if (to.name === 'Login' && token) {
    next({ name: 'Dashboard' })
  } else if (to.name === 'Connect' && token && connStore.connected) {
    next({ name: 'Dashboard' })
  } else {
    next()
  }
})

// ── Mount ──
const app = createApp(App)
app.use(pinia)
app.use(router)

// Register router with api.ts so 401 responses can redirect
setRouter(router)

app.mount('#app')
