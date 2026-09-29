// The presence envelope, as a stream connector writes and reads it.
//
// Every connector in this folder speaks to Revia the same way the Discord one does: one
// JSON object per message into her Presence inbox, one reply file per message out of
// her outbox. Nothing here knows a platform token; nothing here decides what she says.
import { writeFile, rename, readFile, readdir, unlink, stat } from 'node:fs/promises';
import path from 'node:path';

const SAFE_TOKEN = /[^A-Za-z0-9._-]/g;

export function safeToken(value) {
  return String(value ?? '').replace(SAFE_TOKEN, '-').slice(0, 96) || 'missing-id';
}

// Whether a chat line is aimed at her: her name as a word, an @mention, or a command.
export function addressedToRevia(text, name = 'Revia') {
  const lowered = String(text ?? '').toLowerCase();
  const nick = String(name).toLowerCase();
  if (lowered.startsWith(`!${nick}`) || lowered.startsWith(`!ask`)) return true;
  if (lowered.includes(`@${nick}`)) return true;
  const word = new RegExp(`(^|[^a-z0-9])${nick.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}([^a-z0-9]|$)`);
  return word.test(lowered);
}

// Strips the address itself, so "Revia, what's a mutex?" reaches her as the question.
export function stripAddress(text, name = 'Revia') {
  const nick = name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  return String(text ?? '')
    .replace(new RegExp(`^\\s*!(?:${nick}|ask)\\b[\\s,:]*`, 'i'), '')
    .replace(new RegExp(`^\\s*@?${nick}\\b[\\s,:]*`, 'i'), '')
    .trim() || String(text ?? '').trim();
}

export function validateConfig(raw) {
  const c = { ...raw };
  if (!path.isAbsolute(c.inbox ?? '') || !path.isAbsolute(c.outbox ?? '') || c.inbox === c.outbox)
    throw new Error('Configure distinct absolute Presence inbox and outbox paths.');
  c.name = typeof c.name === 'string' && c.name.trim() ? c.name.trim() : 'Revia';
  c.replyMode = c.replyMode ?? 'addressed';
  if (!['addressed', 'conversation'].includes(c.replyMode))
    throw new Error('replyMode must be "addressed" or "conversation".');
  c.replyTimeoutSeconds = Number(c.replyTimeoutSeconds ?? 180);
  if (!(c.replyTimeoutSeconds >= 5 && c.replyTimeoutSeconds <= 600))
    throw new Error('replyTimeoutSeconds must be between 5 and 600.');
  for (const key of ['twitch', 'youtube', 'obs']) {
    if (c[key] !== undefined && (typeof c[key] !== 'object' || c[key] === null))
      throw new Error(`${key} must be an object when present.`);
  }
  return c;
}

export class PresenceEnvelope {
  constructor(config) { this.config = config; this.pending = new Map(); }

  // Writes one message for Revia. Atomic: the file appears complete or not at all.
  async submit({ id, source, channel, authorId, author, role, text, addressed }) {
    const safeId = safeToken(id);
    const event = {
      version: 1, id: safeId, source, channel: safeToken(channel),
      author_id: safeToken(authorId), author: String(author ?? 'viewer').slice(0, 64),
      role: role ?? 'viewer', addressed_to_revia: Boolean(addressed),
      text: String(text ?? '').slice(0, 4000),
    };
    const pendingFile = path.join(this.config.inbox, `${source}-${safeId}.pending`);
    const inputFile = path.join(this.config.inbox, `${source}-${safeId}.json`);
    await writeFile(pendingFile, JSON.stringify(event), { flag: 'wx' });
    await rename(pendingFile, inputFile);
    return event;
  }

  // The reply for one message, or null when she did not answer it in time (the
  // selector may have passed it over, which is not an error).
  async awaitReply(source, id, signal) {
    const replyFile = path.join(this.config.outbox, `${source}-reply-${safeToken(id)}.json`);
    const deadline = Date.now() + this.config.replyTimeoutSeconds * 1000;
    while (Date.now() < deadline) {
      signal?.throwIfAborted();
      try {
        const reply = JSON.parse(await readFile(replyFile, 'utf8'));
        await unlink(replyFile).catch(() => {});
        return reply;
      } catch (error) {
        if (error.code !== 'ENOENT' && !(error instanceof SyntaxError)) throw error;
      }
      await new Promise(resolve => setTimeout(resolve, 250));
    }
    return null;
  }

  // Replies that arrived for messages nobody is waiting on (a restart, a timeout):
  // drained so the outbox does not fill.
  async sweep(source, olderThanMs = 10 * 60 * 1000) {
    let removed = 0;
    for (const name of await readdir(this.config.outbox).catch(() => [])) {
      if (!name.startsWith(`${source}-reply-`) || !name.endsWith('.json')) continue;
      const file = path.join(this.config.outbox, name);
      const info = await stat(file).catch(() => null);
      if (info && Date.now() - info.mtimeMs > olderThanMs) { await unlink(file).catch(() => {}); removed += 1; }
    }
    return removed;
  }
}
