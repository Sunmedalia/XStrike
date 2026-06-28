<template>
  <router-view />
</template>

<style>
:root {
  --bg: #edf8ff;
  --bg-2: #fbfdff;
  --bg-3: #f1f9fe;
  --bg-4: #ddecf8;
  --bd: rgba(67, 151, 194, 0.18);
  --tx: #143045;
  --tx-2: #486b82;
  --tx-3: #7b9aae;
  --tx-4: #a8bdca;
  --pri: #5ebef2;
  --pri-h: #43aee6;
  --on-pri: #062235;
  --blue: #5ebef2;
  --red: #d85f5f;
  --amber: #a87419;
  --purple: #7b61ff;
  --cyan: #4cb9e8;
  --green: #1d9f53;
  --font-mono: 'JetBrains Mono', 'Fira Code', 'Cascadia Code', 'SF Mono', 'Consolas', monospace;
  --font-sans: 'Inter', system-ui, -apple-system, 'Segoe UI', sans-serif;
}

[data-theme='light'] {
  --bg: #edf8ff;
  --bg-2: #fbfcfe;
  --bg-3: #f1f9fe;
  --bg-4: #ddecf8;
  --bd: rgba(67, 151, 194, 0.18);
  --tx: #143045;
  --tx-2: #486b82;
  --tx-3: #7b9aae;
  --tx-4: #a8bdca;
  --pri: #5ebef2;
  --pri-h: #43aee6;
  --on-pri: #062235;
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
