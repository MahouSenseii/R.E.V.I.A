import test from 'node:test';
import assert from 'node:assert/strict';
import { roleFromAuthor, toChatMessage, YouTubeChat } from './youtube.mjs';

const item = (over = {}) => ({ id: 'y1', snippet: { type: 'textMessageEvent', displayMessage: 'Revia, hi' },
  authorDetails: { channelId: 'UC1', displayName: 'Ann', isChatOwner: false, isChatModerator: true, isChatSponsor: false },
  ...over });

test('author flags become roles and a message keeps its author id', () => {
  assert.equal(roleFromAuthor({ isChatOwner: true }), 'broadcaster');
  assert.equal(roleFromAuthor({ isChatModerator: true }), 'moderator');
  assert.equal(roleFromAuthor({ isChatSponsor: true }), 'supporter');
  assert.equal(roleFromAuthor({}), 'viewer');
  assert.deepEqual(toChatMessage(item()), { id: 'y1', channel: 'live', authorId: 'UC1', author: 'Ann',
    role: 'moderator', text: 'hi', addressed: true });
  const superChat = toChatMessage(item({ snippet: { type: 'superChatEvent', displayMessage: 'love it',
    superChatDetails: { amountDisplayString: '$5.00' } } }));
  assert.equal(superChat.role, 'supporter');
  assert.equal(superChat.addressed, true);
  assert.equal(superChat.text, 'Ann sent a super chat ($5.00): love it');
  assert.equal(toChatMessage(item({ snippet: { type: 'messageDeletedEvent' } })), null);
});

test('polling resolves the live chat, skips the history page, then relays new messages', async () => {
  const calls = [];
  const fetchImpl = async (url, init) => {
    calls.push({ url: String(url), init });
    const respond = body => ({ ok: true, status: 200, json: async () => body });
    if (String(url).includes('/videos?')) return respond({ items: [{ liveStreamingDetails: { activeLiveChatId: 'chat-1' } }] });
    if (init?.method === 'POST') return respond({});
    const page = calls.filter(call => call.url.includes('/liveChat/messages?')).length;
    return respond({ nextPageToken: `p${page}`, pollingIntervalMillis: 3000,
      items: page === 1 ? [item({ id: 'old' })] : [item({ id: 'new' })] });
  };
  const chat = new YouTubeChat({ videoId: 'vid' }, { apiKey: 'key', oauthToken: 'tok', fetchImpl, log: () => {} });
  const seen = [];
  chat.on('message', message => seen.push(message.id));
  const first = await chat.poll();
  assert.deepEqual(first.messages, []);
  assert.equal(first.waitMs, 3000);
  const second = await chat.poll();
  assert.equal(second.messages.length, 1);
  assert.deepEqual(seen, ['new']);
  assert.ok(calls[2].url.includes('pageToken=p1'));
  assert.ok(await chat.say('A lock.'));
  const post = calls.at(-1);
  assert.equal(post.init.headers.Authorization, 'Bearer tok');
  assert.equal(JSON.parse(post.init.body).snippet.liveChatId, 'chat-1');
  const readOnly = new YouTubeChat({ liveChatId: 'chat-1' }, { apiKey: 'key', oauthToken: undefined, fetchImpl, log: () => {} });
  assert.equal(await readOnly.say('hello'), false);
});
