import {defineConfig} from 'vite'
import vue from '@vitejs/plugin-vue'

// https://vitejs.dev/config/
// Fixed host/port so Wails' dev-server health check can reach Vite directly
// (the default "auto" URL detection parses Vite's stdout, which is unreliable
// on non-UTF8/GBK Windows terminals and causes endless "Waiting for frontend
// DevServer to be ready").
export default defineConfig({
  plugins: [vue()],
  server: {
    host: '127.0.0.1',
    port: 5173,
    strictPort: true,
  },
})
