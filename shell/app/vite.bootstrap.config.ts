import { defineConfig } from 'vite';

// Standalone build for the iframe bootstrap script. The output is a single
// self-running IIFE that the renderer process embeds (via a generated C++
// header) and evaluates into each PrismaUI iframe context. It is deliberately
// kept separate from the shell page bundle (vite.config.ts) and is not shipped
// as a file.
export default defineConfig({
  build: {
    outDir: '../dist-bootstrap',
    emptyOutDir: true,
    lib: {
      entry: 'src/bootstrap.ts',
      name: 'PrismaBootstrap',
      formats: ['iife'],
      fileName: () => 'bootstrap.js',
    },
  },
});
