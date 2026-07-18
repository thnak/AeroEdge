import { defineConfig } from "vitest/config";
import react from "@vitejs/plugin-react";

// Test config kept separate from vite.config.ts so the build's Vite types and Vitest's types don't
// clash. Not in tsconfig's include, so tsc --noEmit doesn't typecheck it.
export default defineConfig({
  plugins: [react()],
  test: {
    environment: "jsdom",
    globals: true,
  },
});
