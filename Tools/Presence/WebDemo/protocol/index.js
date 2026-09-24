// Canonical executable version-1 schema. Every object rejects unknown fields.
export const VERSION = 1;
export const MAX_TEXT_BYTES = 8192;
export const MAX_RESPONSE_BYTES = 16384;
export const MAX_BODY_BYTES = 16384;
export const MAX_FRAME_BYTES = 32768;
export const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
export const STATES = ['offline', 'starting', 'online', 'busy', 'paused', 'unavailable'];
export const TERMINAL = ['completed', 'cancelled', 'expired', 'failed'];
export const object = (v, required, optional = []) => v !== null && typeof v === 'object' && !Array.isArray(v) && required.every(k => Object.hasOwn(v, k)) && Object.keys(v).every(k => required.includes(k) || optional.includes(k));
export const id = v => typeof v === 'string' && UUID.test(v);
export const text = v => typeof v === 'string' && v.trim().length > 0 && [...v].length <= 2000 && Buffer.byteLength(v) <= MAX_TEXT_BYTES && !/[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/u.test(v);
export const responseText = v => typeof v === 'string' && v.length > 0 && Buffer.byteLength(v) <= MAX_RESPONSE_BYTES;
export const sessionBody = v => object(v, ['inviteCode']) && typeof v.inviteCode === 'string' && v.inviteCode.length > 0 && v.inviteCode.length <= 256;
export const messageBody = v => object(v, ['text', 'idempotencyKey']) && text(v.text) && id(v.idempotencyKey);
export const statusBody = v => object(v, ['state']) && STATES.includes(v.state);
export function nativeResult(v, requestId) {
  return object(v, ['requestId', 'state'], ['text']) && v.requestId === requestId && TERMINAL.includes(v.state) && (v.state === 'completed' ? responseText(v.text) : !Object.hasOwn(v, 'text'));
}
export function frame(v, direction) {
  if (!v || v.version !== VERSION || !id(v.epoch)) return false;
  const base = ['version', 'type', 'epoch'];
  const ids = [...base, 'sessionId', 'requestId'];
  if (direction === 'relay') {
    if (v.type === 'welcome') return object(v, base);
    if (v.type === 'end') return object(v, [...base, 'sessionId']) && id(v.sessionId);
    if (v.type === 'cancel') return object(v, ids) && id(v.sessionId) && id(v.requestId);
    if (v.type === 'turn') return object(v, [...ids, 'text', 'deadline']) && id(v.sessionId) && id(v.requestId) && text(v.text) && Number.isSafeInteger(v.deadline) && v.deadline > 0;
  } else {
    if (v.type === 'ready') return object(v, [...base, 'state']) && STATES.includes(v.state);
    if (v.type === 'cancelled') return object(v, ids) && id(v.sessionId) && id(v.requestId);
    if (v.type === 'result') return object(v, [...ids, 'state'], ['text']) && id(v.sessionId) && id(v.requestId) && nativeResult({ requestId: v.requestId, state: v.state, ...(Object.hasOwn(v, 'text') ? { text: v.text } : {}) }, v.requestId);
  }
  return false;
}
