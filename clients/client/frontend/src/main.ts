import { createApp } from 'vue'
import { createPinia } from 'pinia'
import router from './router'
import './assets/main.css'
import App from './App.vue'
import { setRouter } from './services/api'

const THEME_KEY = 'ghost-theme'
const THEME_MIGRATION_KEY = 'xstrike-theme-migration'
const THEME_MIGRATION_VALUE = 'light-blue-console'
let savedTheme = localStorage.getItem(THEME_KEY)
if (localStorage.getItem(THEME_MIGRATION_KEY) !== THEME_MIGRATION_VALUE) {
  savedTheme = 'light'
  localStorage.setItem(THEME_KEY, savedTheme)
  localStorage.setItem(THEME_MIGRATION_KEY, THEME_MIGRATION_VALUE)
}
const initialTheme = savedTheme === 'dark' ? 'dark' : 'light'
document.documentElement.setAttribute('data-theme', initialTheme)

const app = createApp(App)

app.use(createPinia())
app.use(router)

// Register router with api.ts so 401 responses can redirect
setRouter(router)

app.mount('#app')
