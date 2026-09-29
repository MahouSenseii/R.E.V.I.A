import test from 'node:test';
import assert from 'node:assert/strict';
import net from 'node:net';
import { parseIrcMessage, roleFromTags, toChatMessage, SendPacer, TwitchChat } from './twitch.mjs';

const PRIVMSG = '@badge-info=;badges=moderator/1,subscriber/12;color=#FF0000;display-name=Ann;'
  + 'emotes=;id=abc-123;mod=1;room-id=1;subscriber=1;tmi-sent-ts=1700000000000;user-id=42;user-type=mod '
  + ':ann!ann@ann.tmi.twitch.tv PRIVMSG #revia :Revia, what is a mutex?';

test('an IRC line parses into tags, prefix, command, params and trailing text', () => {
  const parsed = parseIrcMessage(PRIVMSG + '\r\n');
  assert.equal(parsed.command, 'PRIVMSG');
  assert.deepEqual(parsed.params, ['#revia']);
  assert.equal(parsed.trailing, 'Revia, what is a mutex?');
  assert.equal(parsed.tags['display-name'], 'Ann');
  assert.equal(parsed.tags['user-id'], '42');
  assert.equal(parseIrcMessage('PING :tmi.twitch.tv').command, 'PING');
  assert.equal(parseIrcMessage('@system-msg=Ann\\ssubscribed\\sat\\sTier\\s1. :tmi.twitch.tv USERNOTICE #revia')
    .tags['system-msg'], 'Ann subscribed at Tier 1.');
});

test('badges become the roles the selector understands', () => {
  assert.equal(roleFromTags({ badges: 'broadcaster/1' }), 'broadcaster');
  assert.equal(roleFromTags({ badges: 'moderator/1,subscriber/3' }), 'moderator');
  assert.equal(roleFromTags({ badges: 'subscriber/3' }), 'supporter');
  assert.equal(roleFromTags({ bits: '100' }), 'supporter');
  assert.equal(roleFromTags({ badges: 'vip/1' }), 'supporter');
  assert.equal(roleFromTags({}), 'viewer');
});

test('a chat line becomes the message Revia is offered, with the address stripped', () => {
  const message = toChatMessage(parseIrcMessage(PRIVMSG));
  assert.deepEqual(message, { id: 'abc-123', channel: 'revia', authorId: '42', author: 'Ann',
    role: 'moderator', text: 'what is a mutex?', addressed: true });
  const overheard = toChatMessage(parseIrcMessage(':bob!bob@bob.tmi.twitch.tv PRIVMSG #revia :lol that boss'));
  assert.equal(overheard.addressed, false);
  assert.equal(overheard.text, 'lol that boss');
  assert.equal(overheard.authorId, 'bob');
  assert.equal(toChatMessage(parseIrcMessage(':bob!bob@bob.tmi.twitch.tv PRIVMSG #revia :lol'),
    { replyMode: 'conversation' }).addressed, true);
  const sub = toChatMessage(parseIrcMessage('@msg-id=resub;display-name=Cy;user-id=7;id=n1;system-msg=Cy\\ssubscribed\\sfor\\s3\\smonths! '
    + ':tmi.twitch.tv USERNOTICE #revia :love the stream'));
  assert.deepEqual(sub, { id: 'n1', channel: 'revia', authorId: '7', author: 'Cy', role: 'supporter',
    text: 'Cy subscribed for 3 months! -- "love the stream"', addressed: true });
  assert.equal(toChatMessage(parseIrcMessage(':tmi.twitch.tv 001 justinfan1 :Welcome')), null);
});

test('sending keeps a floor between messages and under the window', () => {
  let clock = 0;
  const pacer = new SendPacer({ perWindow: 3, windowMs: 30000, minimumGapMs: 1500, now: () => clock });
  assert.equal(pacer.delayMs(), 0); pacer.record();
  assert.equal(pacer.delayMs(), 1500);
  clock = 1500; pacer.record(); clock = 3000; pacer.record();
  assert.equal(pacer.delayMs(), 30000 - 3000);
  clock = 31000; assert.equal(pacer.delayMs(), 0);
});

// A fake Twitch on plain TCP: enough of the handshake to receive JOIN, deliver a chat
// line, answer a PING, and see the reply she sends back.
test('the client joins, relays a chat line, answers pings and sends the reply into the channel', async () => {
  const received = [];
  const server = net.createServer(socket => {
    socket.setEncoding('utf8');
    let buffer = '';
    socket.on('data', chunk => {
      buffer += chunk;
      let newline;
      while ((newline = buffer.indexOf('\n')) >= 0) {
        const line = buffer.slice(0, newline).replace(/\r$/, ''); buffer = buffer.slice(newline + 1);
        received.push(line);
        if (line.startsWith('JOIN')) {
          socket.write(':tmi.twitch.tv 001 bot :Welcome\r\n');
          socket.write(PRIVMSG + '\r\n');
          socket.write('PING :tmi.twitch.tv\r\n');
        }
      }
    });
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  const messages = [];
  const client = new TwitchChat({ host: '127.0.0.1', port, insecure: true, channel: 'Revia', login: 'bot' },
    { token: 'oauth:secret', log: () => {} });
  client.on('message', message => messages.push(message));
  const closed = client.connect();
  await new Promise(resolve => setTimeout(resolve, 300));
  assert.equal(messages.length, 1);
  assert.equal(messages[0].text, 'what is a mutex?');
  assert.ok(await client.say('A lock one\nthread holds.'));
  await new Promise(resolve => setTimeout(resolve, 100));
  client.close();
  await closed;
  server.close();
  assert.ok(received.includes('CAP REQ :twitch.tv/tags twitch.tv/commands'));
  assert.ok(received.includes('PASS oauth:secret'));
  assert.ok(received.includes('NICK bot'));
  assert.ok(received.includes('JOIN #revia'));
  assert.ok(received.includes('PONG :tmi.twitch.tv'));
  assert.ok(received.includes('PRIVMSG #revia :A lock one thread holds.'));
});

test('without a token the client reads as an anonymous nick and never sends', async () => {
  const client = new TwitchChat({ channel: 'revia' }, { token: undefined, log: () => {} });
  assert.match(client.login, /^justinfan\d+$/);
  assert.equal(await client.say('hello'), false);
});
