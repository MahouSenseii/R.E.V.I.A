import { configFromEnv } from './config.js';
import { createRelay } from './server.js';
try {
  const config = configFromEnv();
  const port = Number(process.env.PORT ?? 8787);
  if (!Number.isSafeInteger(port) || port < 1 || port > 65535) throw Error('Invalid PORT');
  if (process.argv.includes('--check')) console.log('Relay configuration valid; no network connection made.');
  else {
    const relay = createRelay(config); await relay.listen(port); console.log('Revia relay listening.');
    for (const signal of ['SIGINT', 'SIGTERM']) process.once(signal, async () => { await relay.close(); process.exitCode = 0; });
  }
} catch { console.error('Relay configuration/startup failed. Check the documented required settings.'); process.exitCode = 1; }
