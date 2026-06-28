/**
 * Vite config for the standalone Ghost Client build.
 *
 * Differences from the default (server-embedded) build:
 *   - Entry: index.client.html → main.client.ts
 *   - Output: dist-client/
 *   - Uses hash routing (no server-side rewrite rules needed)
 *   - No dev-server proxy (all requests go to the remote server)
 *   - Can be served from any static HTTP server or opened via file://
 */

import { defineConfig, type Plugin } from 'vite'
import vue from '@vitejs/plugin-vue'
import path from 'path'
import fs from 'fs'

/**
 * In dev mode, rewrite `/` and `/index.html` to the client entry
 * so the Vite dev server serves it at the root URL.
 */
function serveClientIndex(): Plugin {
  return {
    name: 'serve-client-index',
    configureServer(server) {
      server.middlewares.use((req, _res, next) => {
        if (req.url === '/' || req.url === '/index.html') {
          req.url = '/index.client.html'
        }
        next()
      })
    },
  }
}

/**
 * Vite outputs HTML files using the source filename (index.client.html).
 * Tauri expects index.html. This plugin copies the output so both exist.
 */
function copyAsIndexHtml(): Plugin {
  return {
    name: 'copy-as-index-html',
    closeBundle() {
      const outDir = path.resolve(__dirname, 'dist-client')
      const src = path.join(outDir, 'index.client.html')
      const dest = path.join(outDir, 'index.html')
      if (fs.existsSync(src) && !fs.existsSync(dest)) {
        fs.copyFileSync(src, dest)
      }
    },
  }
}

export default defineConfig({
  base: './',
  plugins: [vue(), serveClientIndex(), copyAsIndexHtml()],
  resolve: {
    alias: {
      '@': path.resolve(__dirname, './src'),
    },
  },
  server: {
    port: 5174,
    strictPort: true,
  },
  build: {
    outDir: 'dist-client',
    emptyOutDir: true,
    rollupOptions: {
      input: {
        index: path.resolve(__dirname, 'index.client.html'),
      },
    },
  },
})
