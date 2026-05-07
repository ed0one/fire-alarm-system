import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      // Forward /api/* → backend server so no CORS issues in dev
      '/api': 'http://localhost:3001',
    },
  },
})
