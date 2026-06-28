<template>
  <router-view />
</template>

<style>
:root {
  --bg: #151922;
  --bg-2: #1d232d;
  --bg-3: #242b36;
  --bg-4: #2d3542;
  --bd: rgba(191, 203, 218, 0.14);
  --tx: #e6edf5;
  --tx-2: #a8b3c3;
  --tx-3: #748196;
  --tx-4: #4c5869;
  --pri: #8bd8ff;
  --pri-h: #66c7f5;
  --blue: #8bd8ff;
  --red: #f85149;
  --amber: #e0b154;
  --purple: #a78bfa;
  --cyan: #8bd8ff;
  --green: #50c878;
  --font-mono: 'JetBrains Mono', 'Fira Code', 'Cascadia Code', 'SF Mono', 'Consolas', monospace;
  --font-sans: 'Inter', system-ui, -apple-system, 'Segoe UI', sans-serif;
}

[data-theme='light'] {
  --bg: #eef1f5;
  --bg-2: #fbfcfe;
  --bg-3: #f3f5f8;
  --bg-4: #e7ebf0;
  --bd: rgba(43, 59, 79, 0.14);
  --tx: #17212f;
  --tx-2: #3b4d63;
  --tx-3: #5e718a;
  --tx-4: #8698af;
  --pri: #5ebef2;
  --pri-h: #43aee6;
  --blue: #5ebef2;
  --red: #db4f4f;
  --amber: #b8811a;
  --purple: #7b61ff;
  --cyan: #4cb9e8;
  --green: #1d9f53;
}

body {
  margin: 0;
  padding: 0;
  background: var(--bg);
  color: var(--tx);
  font-family: var(--font-sans);
  overflow: hidden;
  height: 100vh;
}

#app {
  height: 100%;
}
</style>
