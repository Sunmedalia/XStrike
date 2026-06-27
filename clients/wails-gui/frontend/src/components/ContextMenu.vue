<template>
  <div v-if="store.isOpen" class="ctx-menu" :style="{ top: store.y + 'px', left: store.x + 'px' }">
    <div v-for="(item, i) in store.items" :key="i">
      <div v-if="item.divider" class="divider"></div>
      <div v-else class="item" :class="{ danger: item.danger }" @click="item.action">
        <component :is="item.icon" v-if="item.icon" :size="14" class="icon" />
        <span>{{ item.label }}</span>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { useContextMenuStore } from '../stores/contextMenu'
const store = useContextMenuStore()
</script>

<style scoped>
.ctx-menu {
  position: fixed;
  z-index: 10000;
  background: var(--bg-2);
  border: 1px solid var(--bd);
  border-radius: 4px;
  padding: 4px;
  min-width: 160px;
  box-shadow: 0 10px 30px rgba(0,0,0,0.5);
}
.item {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 6px 12px;
  font-size: 12px;
  color: var(--tx-2);
  cursor: pointer;
  border-radius: 3px;
}
.item:hover { background: var(--bg-4); color: var(--tx); }
.item.danger:hover { background: var(--red); color: white; }
.icon { opacity: 0.7; }
.divider { height: 1px; background: var(--bd); margin: 4px 0; }
</style>
