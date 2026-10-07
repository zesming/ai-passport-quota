import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import { readFile } from 'node:fs/promises';

const deviceFiles = {
  '/device-settings.html': new URL('../main/portable_setup.html', import.meta.url),
  '/portable_setup.mjs': new URL('../main/portable_setup.mjs', import.meta.url),
  '/portable_serial.mjs': new URL('../main/portable_serial.mjs', import.meta.url),
};
function sharedDeviceSettings() {
  return {
    name: 'shared-device-settings',
    async generateBundle() {
      for (const [url, source] of Object.entries(deviceFiles)) {
        this.emitFile({ type: 'asset', fileName: url.slice(1), source: await readFile(source, 'utf8') });
      }
    },
    configureServer(server) {
      server.middlewares.use(async (request, response, next) => {
        const pathname = new URL(request.url, 'http://localhost').pathname;
        if (!Object.hasOwn(deviceFiles, pathname) || request.method !== 'GET') return next();
        try {
          response.setHeader('Content-Type', pathname.endsWith('.html') ? 'text/html;charset=utf-8' : 'text/javascript;charset=utf-8');
          response.setHeader('Cache-Control', 'no-store');
          response.end(await readFile(deviceFiles[pathname]));
        } catch { response.statusCode = 503; response.end('Device settings unavailable'); }
      });
    },
  };
}

export default defineConfig({
  build: {
    outDir: "dist/client",
  },
  optimizeDeps: {
    include: ["react", "react-dom/client"],
  },
  server: {
    host: "127.0.0.1",
    port: 5173,
    strictPort: true,
    proxy: {
      "/api": { target: "http://127.0.0.1:4317", changeOrigin: true },
    },
    warmup: {
      clientFiles: ["./src/main.jsx"],
    },
  },
  plugins: [react(), sharedDeviceSettings()],
});
