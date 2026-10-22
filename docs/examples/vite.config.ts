import { defineConfig, loadEnv } from 'vite';

// Скопируйте в корень проекта фронтенда. При существующей конфигурации
// перенесите server.proxy и чтение ALFA_* в свой defineConfig.
// ALFA_API_TOKEN читается только процессом Vite, не клиентским bundle.
export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), 'ALFA_');
  const target = process.env.ALFA_BACKEND_URL || env.ALFA_BACKEND_URL || 'http://127.0.0.1:8080';
  const token = process.env.ALFA_API_TOKEN || env.ALFA_API_TOKEN || '';

  const backendProxy = () => ({
    target,
    changeOrigin: true,
    timeout: 240_000,
    proxyTimeout: 240_000,
    ...(token ? { headers: { Authorization: `Bearer ${token}` } } : {}),
  });

  return {
    server: {
      host: '127.0.0.1',
      port: 5173,
      strictPort: true,
      proxy: {
        '/api': backendProxy(),
        '/health': backendProxy(),
      },
    },
  };
});
