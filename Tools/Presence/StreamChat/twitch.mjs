// Twitch chat over IRC, which is what Twitch still serves at irc.chat.twitch.tv:6697.
//
// Read-only needs no token at all (an anonymous justinfan nick); replying needs a user
// access token with chat:read and chat:edit, given as TWITCH_OAUTH_TOKEN in the
// environment, never in a file. Twitch's own limit for a regular account is 20
// messages per 30 seconds; the sender here keeps under it.
import net from 'node:net';
import tls from 'node:tls';
import { addressedToRevia, stripAddress } from './envelope.mjs';

// One IRC line into its tags, prefix, command, parameters and trailing text.
export function parseIrcMessage(line) {
  let rest = String(line).replace(/[\r\n]+$/, '');
  const tags = {};
  if (rest.startsWith('@')) {
    const end = rest.indexOf(' ');
    for (const pair of rest.slice(1, end).split(';')) {
      const equals = pair.indexOf('=');
      const key = equals < 0 ? pair : pair.slice(0, equals);
      const value = equals < 0 ? '' : pair.slice(equals + 1)
        .replace(/\\s/g, ' ').replace(/\\:/g, ';').replace(/\\\\/g, '\\');
      tags[key] = value;
    }
    rest = rest.slice(end + 1);
  }
  let prefix = '';
  if (rest.startsWith(':')) {
    const end = rest.indexOf(' ');
    prefix = rest.slice(1, end);
    rest = rest.slice(end + 1);
  }
  let trailing = null;
  const colon = rest.indexOf(' :');
  if (colon >= 0) { trailing = rest.slice(colon + 2); rest = rest.slice(0, colon); }
  const [command, ...params] = rest.split(' ').filter(Boolean);
  return { tags, prefix, command: command ?? '', params, trailing };
}

// The role Revia's selector understands, from the badges Twitch attaches.
export function roleFromTags(tags) {
  const badges = String(tags.badges ?? '');
  if (badges.includes('broadcaster/')) return 'broadcaster';
  if (tags.mod === '1' || badges.includes('moderator/')) return 'moderator';
  if (tags.bits || badges.includes('subscriber/') || badges.includes('founder/') ||
      badges.includes('vip/') || tags.subscriber === '1') return 'supporter';
  return 'viewer';
}

// A PRIVMSG or a USERNOTICE (sub, gift, raid) as the message Revia is offered, or null.
export function toChatMessage(parsed, { name = 'Revia', replyMode = 'addressed' } = {}) {
  if (parsed.command === 'PRIVMSG' && parsed.trailing) {
    const login = parsed.prefix.split('!')[0];
    const author = parsed.tags['display-name'] || login;
    const text = parsed.trailing;
    const addressed = replyMode === 'conversation' || addressedToRevia(text, name);
    return {
      id: parsed.tags.id || `${parsed.tags['tmi-sent-ts'] || Date.now()}-${login}`,
      channel: (parsed.params[0] ?? '#chat').replace(/^#/, ''),
      authorId: parsed.tags['user-id'] || login,
      author, role: roleFromTags(parsed.tags),
      text: addressed ? stripAddress(text, name) : text,
      addressed,
    };
  }
  if (parsed.command === 'USERNOTICE') {
    const kind = parsed.tags['msg-id'] ?? '';
    if (!['sub', 'resub', 'subgift', 'submysterygift', 'raid', 'anongift', 'giftpaidupgrade'].includes(kind)) return null;
    const author = parsed.tags['display-name'] || parsed.tags.login || 'someone';
    const system = parsed.tags['system-msg'] || `${author} ${kind}`;
    return {
      id: parsed.tags.id || `${Date.now()}-${kind}`,
      channel: (parsed.params[0] ?? '#chat').replace(/^#/, ''),
      authorId: parsed.tags['user-id'] || author,
      author, role: 'supporter',
      text: parsed.trailing ? `${system} -- "${parsed.trailing}"` : system,
      addressed: true,
    };
  }
  return null;
}

// Keeps under Twitch's 20 per 30 s, with a floor between sends so a burst of replies
// reads as chat and not as a bot.
export class SendPacer {
  constructor({ perWindow = 18, windowMs = 30000, minimumGapMs = 1500, now = () => Date.now() } = {}) {
    this.perWindow = perWindow; this.windowMs = windowMs; this.minimumGapMs = minimumGapMs;
    this.now = now; this.sent = [];
  }
  delayMs() {
    const now = this.now();
    this.sent = this.sent.filter(at => now - at < this.windowMs);
    let wait = 0;
    if (this.sent.length > 0) wait = Math.max(wait, this.sent[this.sent.length - 1] + this.minimumGapMs - now);
    if (this.sent.length >= this.perWindow) wait = Math.max(wait, this.sent[0] + this.windowMs - now);
    return Math.max(0, wait);
  }
  record() { this.sent.push(this.now()); }
}

export class TwitchChat {
  // `channel` without the #. `insecure` is for the test's plain-TCP fake server only.
  constructor(config, { token = process.env.TWITCH_OAUTH_TOKEN, log = console.log } = {}) {
    this.config = config; this.token = token; this.log = log;
    this.socket = null; this.buffer = ''; this.pacer = new SendPacer();
    this.handlers = { message: [] };
    this.login = token && config.login ? config.login : `justinfan${Math.floor(10000 + Math.random() * 80000)}`;
  }
  on(event, handler) { this.handlers[event].push(handler); return this; }

  connect() {
    const host = this.config.host ?? 'irc.chat.twitch.tv';
    const port = Number(this.config.port ?? 6697);
    const socket = this.config.insecure ? net.connect({ host, port }) : tls.connect({ host, port, servername: host });
    this.socket = socket;
    socket.setEncoding('utf8');
    socket.on('data', chunk => this.receive(chunk));
    socket.once(this.config.insecure ? 'connect' : 'secureConnect', () => {
      this.raw('CAP REQ :twitch.tv/tags twitch.tv/commands');
      if (this.token) this.raw(`PASS oauth:${this.token.replace(/^oauth:/, '')}`);
      this.raw(`NICK ${this.login}`);
      this.raw(`JOIN #${this.config.channel.toLowerCase()}`);
      this.log(`Twitch: joined #${this.config.channel} as ${this.login}${this.token ? '' : ' (read-only)'}.`);
    });
    return new Promise((resolve, reject) => {
      socket.once('error', reject);
      socket.once('close', () => resolve());
    });
  }

  receive(chunk) {
    this.buffer += chunk;
    let newline;
    while ((newline = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, newline);
      this.buffer = this.buffer.slice(newline + 1);
      const parsed = parseIrcMessage(line);
      if (parsed.command === 'PING') { this.raw(`PONG :${parsed.trailing ?? 'tmi.twitch.tv'}`); continue; }
      if (parsed.command === 'NOTICE' && /authentication failed/i.test(parsed.trailing ?? '')) {
        this.log('Twitch: the token was refused. Check TWITCH_OAUTH_TOKEN and its scopes.');
      }
      const message = toChatMessage(parsed, this.config);
      if (message) for (const handler of this.handlers.message) handler(message);
    }
  }

  raw(line) { this.socket?.write(`${line}\r\n`); }

  // A reply into the channel, paced. Silently a no-op when read-only.
  async say(text) {
    if (!this.token) { this.log(`Twitch (read-only, not sent): ${text}`); return false; }
    const wait = this.pacer.delayMs();
    if (wait > 0) await new Promise(resolve => setTimeout(resolve, wait));
    this.pacer.record();
    // One line: IRC has no newlines, and Twitch caps a message at 500 characters.
    this.raw(`PRIVMSG #${this.config.channel.toLowerCase()} :${text.replace(/\s+/g, ' ').slice(0, 480)}`);
    return true;
  }

  close() { this.socket?.end(); }
}
