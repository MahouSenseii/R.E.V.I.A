import { BlockList, isIP } from 'node:net';

function normalize(value) {
  if (typeof value !== 'string' || value.includes('%') || !isIP(value)) return null;
  if (value.startsWith('::ffff:') && isIP(value.slice(7)) === 4) return value.slice(7);
  return isIP(value) === 6 ? new URL(`http://[${value}]`).hostname.slice(1, -1) : value;
}

export function trustedProxies(cidrs = []) {
  if (!Array.isArray(cidrs) || cidrs.length > 16) throw Error('Invalid trusted proxy list');
  const list = new BlockList();
  for (const cidr of cidrs) {
    if (typeof cidr !== 'string') throw Error('Invalid proxy CIDR');
    const [address, prefix, extra] = cidr.split('/'); const family = isIP(address);
    const bits = prefix === undefined ? (family === 4 ? 32 : 128) : Number(prefix);
    if (!family || extra !== undefined || !Number.isInteger(bits) || bits < 1 || bits > (family === 4 ? 32 : 128) || (prefix !== undefined && !/^\d+$/.test(prefix))) throw Error('Invalid proxy CIDR');
    list.addSubnet(address, bits, family === 4 ? 'ipv4' : 'ipv6');
  }
  return address => { const value = normalize(address); return value !== null && list.check(value, isIP(value) === 4 ? 'ipv4' : 'ipv6'); };
}

export function sourceAddress(req, trusted) {
  const peer = normalize(req.socket.remoteAddress) ?? 'unknown';
  if (!trusted(peer)) return peer;
  const forwarded = req.headers['x-forwarded-for'];
  if (typeof forwarded !== 'string' || forwarded.length > 1024) return peer;
  const chain = forwarded.split(',').map(value => normalize(value.trim()));
  if (!chain.length || chain.length > 16 || chain.some(value => value === null)) return peer;
  let current = peer;
  for (let index = chain.length - 1; index >= 0 && trusted(current); index--) current = chain[index];
  return current;
}
