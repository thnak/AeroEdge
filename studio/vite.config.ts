/// <reference types="node" />
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Studio dev server proxies /api to the aero-runtime daemon so the browser talks ONLY to aero-api
// (013 T2). Override the target with VITE_API_URL when running the daemon elsewhere.
export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      "/api": {
        target: process.env.VITE_API_URL ?? "http://127.0.0.1:8080",
        changeOrigin: true,
        rewrite: (p) => p.replace(/^\/api/, ""),
      },
    },
  },
});
