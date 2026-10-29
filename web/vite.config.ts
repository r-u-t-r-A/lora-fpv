import { defineConfig } from "vite";

// The built GUI is copied to each VRX and served from GET /. Relative paths
// so it works from any VRX origin.
export default defineConfig({
  base: "./",
  build: { target: "es2022", outDir: "dist", assetsInlineLimit: 0 },
  server: { port: 5173 },
});
