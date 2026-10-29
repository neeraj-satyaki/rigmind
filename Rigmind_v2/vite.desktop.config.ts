// Static SPA build for the native WebView2 host (RigMind_Desktop). Lovable's vite.config.ts stays untouched.
import { defineConfig } from "@lovable.dev/vite-tanstack-config";

export default defineConfig({
  tanstackStart: {
    spa: { enabled: true, prerender: { enabled: false } },
  },
  build: { outDir: "../RigMind_Desktop/web", emptyOutDir: true },
});
