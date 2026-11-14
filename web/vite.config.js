import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// The browser talks only to the Vite server; it forwards API and live-feed traffic to the gateway.
export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      '/api': 'http://127.0.0.1:3000',
      '/socket.io': { target: 'http://127.0.0.1:3000', ws: true },
    },
  },
});