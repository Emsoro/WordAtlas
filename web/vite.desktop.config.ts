import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

/**
 * 桌面外壳构建（TauriCPP 用）。
 *
 * 与 vite.config.ts 的区别：public/ 不再整体复制进产物 —— 那里有 600MB+
 * 的 atlas 媒体，运行期由 exe 同目录的 atlas/ 提供（见 TauriCPP/src/window.cpp）。
 * 产物只有 index.html + assets/*，由 build.ps1 补上品牌图/音效/图标后嵌入 exe。
 */
export default defineConfig({
  base: "/",
  publicDir: false,
  plugins: [react()],
  build: {
    outDir: "../frontend",
    emptyOutDir: true,
    copyPublicDir: false,
    chunkSizeWarningLimit: 600,
  },
});
