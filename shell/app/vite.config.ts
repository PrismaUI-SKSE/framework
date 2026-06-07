import { defineConfig } from 'vite';

const fileUrlHtmlPlugin = {
  name: 'prismaui-file-url-html',
  transformIndexHtml: {
    order: 'post' as const,
    handler(html: string): string {
      return html
        .replace(/<script\s+type="module"\s+crossorigin\s+src=/g, '<script defer src=')
        .replace(/\s+crossorigin(?=[\s>])/g, '');
    },
  },
};

export default defineConfig({
  base: './',
  plugins: [fileUrlHtmlPlugin],
  build: {
    outDir: '../dist',
    emptyOutDir: true,
    assetsInlineLimit: 0,
    modulePreload: false,
    rollupOptions: {
      output: {
        entryFileNames: 'assets/[name].js',
        chunkFileNames: 'assets/[name].js',
        assetFileNames: 'assets/[name].[ext]',
      },
    },
  },
});
