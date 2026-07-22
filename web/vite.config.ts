import path from "path"
import { defineConfig } from "vite"
import react from "@vitejs/plugin-react"
import tailwindcss from "@tailwindcss/vite"

// https://vite.dev/config/
export default defineConfig({
  plugins: [react(), tailwindcss()],
  resolve: {
    alias: {
      "@": path.resolve(__dirname, "./src"),
    },
  },
  // dev: фронт (:5173) проксирует /api на Go-бэкенд (:8080), чтобы не ловить CORS.
  // в проде бэкенд сам раздаёт собранный dist — прокси не нужен.
  server: {
    proxy: {
      "/api": "http://localhost:8080",
    },
  },
})
