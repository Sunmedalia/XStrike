import { defineStore } from 'pinia'

export interface Toast {
  id: number
  message: string
  type: 'success' | 'error' | 'info' | 'warning'
}

const MAX_TOASTS = 4

export const useToastStore = defineStore('toast', {
  state: () => ({
    toasts: [] as Toast[]
  }),
  actions: {
    show(message: string, type: Toast['type'] = 'info') {
      const id = Date.now()
      // Evict oldest if at cap
      if (this.toasts.length >= MAX_TOASTS) {
        this.toasts.shift()
      }
      this.toasts.push({ id, message, type })
      setTimeout(() => this.remove(id), 3000)
    },
    remove(id: number) {
      this.toasts = this.toasts.filter(t => t.id !== id)
    },
    success(msg: string) { this.show(msg, 'success') },
    error(msg: string) { this.show(msg, 'error') },
    info(msg: string) { this.show(msg, 'info') },
    warning(msg: string) { this.show(msg, 'warning') }
  }
})
