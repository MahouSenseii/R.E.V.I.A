import { bridgeConfigFromEnv, createBridge } from './client.js';
try {
  const config = bridgeConfigFromEnv();
  if (process.argv.includes('--check')) console.log('Connector configuration valid; no network connection made.');
  else {
    const bridge = createBridge(config); bridge.start(); console.log('Revia outbound connector started.');
    for (const signal of ['SIGINT', 'SIGTERM']) process.once(signal, async () => { await bridge.stop(); process.exitCode = 0; });
  }
} catch { console.error('Connector configuration/startup failed. Check the documented required settings.'); process.exitCode = 1; }
