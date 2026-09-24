import { createHash, timingSafeEqual } from 'node:crypto';
import { trustedProxies } from './proxy.js';
export const hash = value => createHash('sha256').update(value).digest('hex');
export const secretMatches = (value, digest) => typeof value === 'string' && value.length <= 512 && timingSafeEqual(Buffer.from(hash(value), 'hex'), Buffer.from(digest, 'hex'));
export const loopback = name => name === '127.0.0.1' || name === '[::1]' || name === '::1';
const bounds = {
  maxSessions: [10, 1, 10], maxWaiting: [4, 0, 4], maxRequests: [32, 1, 32],
  idleMs: [900000, 50, 900000], totalMs: [1800000, 50, 1800000], requestMs: [120000, 20, 120000],
  submissionsPerMinute: [6, 1, 6], sourceSubmissionsPerMinute: [30, 1, 60], issuePerMinute: [5, 1, 20],
  readsPerMinute: [300, 1, 1000], statusPerMinute: [120, 1, 1000], dailyCap: [200, 1, 1000],
  maxRateKeys: [2048, 1, 4096], maxRetainedBytes: [2097152, 32768, 8388608],
  heartbeatMs: [15000, 50, 30000], cancelAckMs: [5000, 50, 10000], sweepMs: [250, 5, 1000]
};
export function relayConfig(input = {}) {
  const mode = input.mode ?? 'production';
  if (!['production', 'development'].includes(mode)) throw Error('Invalid mode');
  const bind = input.bind ?? (mode === 'development' ? '127.0.0.1' : '0.0.0.0');
  if (mode === 'development' && !loopback(bind)) throw Error('Development must bind a loopback address');
  const hostTokenHash = input.hostTokenHash ?? (input.hostToken ? hash(input.hostToken) : '');
  const inviteHash = input.inviteHash ?? (input.inviteCode ? hash(input.inviteCode) : '');
  if (![hostTokenHash, inviteHash].every(v => /^[a-f0-9]{64}$/.test(v))) throw Error('Host and invite SHA-256 hashes required');
  if ((input.hostToken && Buffer.byteLength(input.hostToken) < 32) || (input.inviteCode && Buffer.byteLength(input.inviteCode) < 32)) throw Error('Credentials require at least 32 bytes');
  const hostId = input.hostId ?? '';
  if (!/^[a-zA-Z0-9_-]{1,64}$/.test(hostId)) throw Error('Invalid host ID');
  const origins = input.origins ?? [];
  if (!Array.isArray(origins) || origins.length < 1 || origins.length > 8 || !origins.every(value => {
    try { const u = new URL(value); return u.origin === value && !u.username && !u.password && (u.protocol === 'https:' || (mode === 'development' && u.protocol === 'http:' && loopback(u.hostname))); } catch { return false; }
  })) throw Error('Exact HTTPS browser origins required (HTTP loopback only in development)');
  const limits = {};
  if (Object.keys(input.limits ?? {}).some(k => !Object.hasOwn(bounds, k))) throw Error('Unknown limit');
  for (const [key, [fallback, min, max]] of Object.entries(bounds)) {
    const value = input.limits?.[key] ?? fallback;
    if (!Number.isSafeInteger(value) || value < min || value > max) throw Error(`Invalid limit: ${key}`);
    limits[key] = value;
  }
  const proxyTrust = trustedProxies(input.trustedProxyCidrs);
  return { mode, bind, hostTokenHash, inviteHash, hostId, origins: new Set(origins), limits, proxyTrust };
}
export function configFromEnv(env = process.env) {
  let limits = {};
  if (env.REVIA_WEB_LIMITS) { try { limits = JSON.parse(env.REVIA_WEB_LIMITS); } catch { throw Error('Invalid REVIA_WEB_LIMITS'); } }
  return relayConfig({ mode: env.NODE_ENV === 'development' ? 'development' : 'production', bind: env.REVIA_WEB_BIND, hostTokenHash: env.REVIA_WEB_HOST_TOKEN_SHA256, inviteHash: env.REVIA_WEB_INVITE_SHA256, hostId: env.REVIA_WEB_HOST_ID, origins: (env.REVIA_WEB_ORIGINS ?? '').split(',').filter(Boolean), trustedProxyCidrs: (env.REVIA_WEB_TRUSTED_PROXY_CIDRS ?? '').split(',').filter(Boolean), limits });
}
