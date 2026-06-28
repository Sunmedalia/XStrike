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

[data-theme='dark'] {
  --bg: #101820;
  --bg-2: #17242e;
  --bg-3: #1d2f3b;
  --bg-4: #27404f;
  --bd: rgba(139, 216, 255, 0.16);
  --tx: #eaf6fc;
  --tx-2: #b6c8d4;
  --tx-3: #8299a8;
  --tx-4: #556b7a;
  --pri: #8bd8ff;
  --pri-h: #66c7f5;
  --on-pri: #062235;
  --blue: #8bd8ff;
  --red: #f26f6f;
  --amber: #e0b154;
  --purple: #a78bfa;
  --cyan: #8bd8ff;
  --green: #50c878;
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
