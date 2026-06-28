import { createRouter, createWebHistory } from 'vue-router'
import Dashboard from '../views/Dashboard.vue'
import Login from '../views/Login.vue'
import Connect from '../views/Connect.vue'
import { useConnectionStore } from '../stores/connection'
import { ROUTER_BASE } from '../runtime/env'

const router = createRouter({
  history: createWebHistory(ROUTER_BASE),
  routes: [
    {
      path: '/connect',
      name: 'Connect',
      component: Connect,
    },
    {
      path: '/login',
      name: 'Login',
      component: Login,
    },
    {
      path: '/',
      name: 'Dashboard',
      component: Dashboard,
      meta: { requiresAuth: true }
    },
  ],
})

router.beforeEach((to, _from, next) => {
  const token = localStorage.getItem('token')
  const connStore = useConnectionStore()

  if (to.meta.requiresAuth && !token) {
    // No token — where to redirect depends on connection mode.
    if (connStore.isRemote && !connStore.connected) {
      next({ name: 'Connect' })
    } else {
      next({ name: 'Login' })
    }
  } else if (to.name === 'Login' && token) {
    next({ name: 'Dashboard' })
  } else if (to.name === 'Connect' && token && connStore.connected) {
    // Already connected — skip connect page
    next({ name: 'Dashboard' })
  } else {
    next()
  }
})

export default router
