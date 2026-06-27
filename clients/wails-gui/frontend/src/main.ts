import { createApp } from 'vue'
import { createPinia } from 'pinia'
import router from './router'
import './assets/main.css'
import App from './App.vue'
import { setRouter } from './services/api'

const THEME_KEY = 'ghost-theme'
const savedTheme = localStorage.getItem(THEME_KEY)
const initialTheme = savedTheme === 'light' ? 'light' : 'dark'
document.documentElement.setAttribute('data-theme', initialTheme)

const app = createApp(App)

app.use(createPinia())
app.use(router)

// Register router with api.ts so 401 responses can redirect
setRouter(router)

app.mount('#app')
