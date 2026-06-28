import { defineStore } from 'pinia'

export interface MenuItem {
  label: string
  icon?: any
  action: () => void
  danger?: boolean
  divider?: boolean
}

export const useContextMenuStore = defineStore('contextMenu', {
  state: () => ({
    isOpen: false,
    x: 0,
    y: 0,
    items: [] as MenuItem[]
  }),
  actions: {
    show(x: number, y: number, items: MenuItem[]) {
      this.x = x
      this.y = y
      this.items = items
      this.isOpen = true
      
      const close = () => {
        this.isOpen = false
        window.removeEventListener('click', close)
      }
      setTimeout(() => window.addEventListener('click', close), 10)
    }
  }
})
