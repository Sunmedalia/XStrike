import { defineStore } from 'pinia'
import { markRaw } from 'vue'

export const useModalStore = defineStore('modal', {
  state: () => ({
    isOpen: false,
    component: null as any,
    props: {} as any
  }),
  actions: {
    open(component: any, props: any = {}) {
      this.component = markRaw(component)
      this.props = props
      this.isOpen = true
    },
    close() {
      this.isOpen = false
      this.component = null
      this.props = {}
    }
  }
})
