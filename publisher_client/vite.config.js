import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  server: {
    host: true, // 모바일 접속을 위해 0.0.0.0 바인딩
  },
})
