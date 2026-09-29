// YouTube live chat through the Data API v3: polled, because that is what the API is.
//
// Reading needs an API key (YOUTUBE_API_KEY); each poll costs one quota unit of the
// default 10,000 a day, and the API tells us how often to ask. Replying needs an OAuth
// user token (YOUTUBE_OAUTH_TOKEN) and costs 50 units a message, so replies are off
// unless that token is present, and even then they are what she says, not every
// message she reads.
import { addressedToRevia, stripAddress } from './envelope.mjs';

const API = 'https://www.googleapis.com/youtube/v3';

export function roleFromAuthor(author = {}) {
  if (author.isChatOwner) return 'broadcaster';
  if (author.isChatModerator) return 'moderator';
  if (author.isChatSponsor) return 'supporter';
  return 'viewer';
}

// One liveChatMessages item as the message Revia is offered, or null when it is not a
// text message (a deleted message, a ban event).
export function toChatMessage(item, { name = 'Revia', replyMode = 'addressed' } = {}) {
  const snippet = item?.snippet ?? {};
  const author = item?.authorDetails ?? {};
  let text = snippet.displayMessage ?? snippet.textMessageDetails?.messageText ?? '';
  let role = roleFromAuthor(author);
  let addressed = replyMode === 'conversation' || addressedToRevia(text, name);
  if (snippet.type === 'superChatEvent' || snippet.type === 'superStickerEvent' ||
      snippet.type === 'newSponsorEvent' || snippet.type === 'memberMilestoneChatEvent') {
    role = 'supporter';
    addressed = true;
    const amount = snippet.superChatDetails?.amountDisplayString;
    text = `${author.displayName ?? 'someone'} ${snippet.type === 'newSponsorEvent' ? 'became a member' : 'sent a super chat'}${amount ? ` (${amount})` : ''}${text ? `: ${text}` : ''}`;
  } else if (snippet.type && snippet.type !== 'textMessageEvent') {
    return null;
  }
  if (!text) return null;
  return {
    id: item.id, channel: 'live', authorId: author.channelId ?? 'unknown',
    author: author.displayName ?? 'viewer', role,
    text: addressed ? stripAddress(text, name) : text, addressed,
  };
}

export class YouTubeChat {
  constructor(config, { apiKey = process.env.YOUTUBE_API_KEY, oauthToken = process.env.YOUTUBE_OAUTH_TOKEN,
    fetchImpl = globalThis.fetch, log = console.log } = {}) {
    this.config = config; this.apiKey = apiKey; this.oauthToken = oauthToken;
    this.fetch = fetchImpl; this.log = log; this.liveChatId = config.liveChatId ?? null;
    this.pageToken = null; this.handlers = { message: [] }; this.stopped = false;
  }
  on(event, handler) { this.handlers[event].push(handler); return this; }

  async resolveLiveChatId() {
    if (this.liveChatId) return this.liveChatId;
    if (!this.config.videoId) throw new Error('YouTube: set videoId (the live stream) or liveChatId.');
    const url = `${API}/videos?part=liveStreamingDetails&id=${encodeURIComponent(this.config.videoId)}&key=${this.apiKey}`;
    const response = await this.fetch(url);
    if (!response.ok) throw new Error(`YouTube: videos.list failed with ${response.status}.`);
    const body = await response.json();
    this.liveChatId = body.items?.[0]?.liveStreamingDetails?.activeLiveChatId ?? null;
    if (!this.liveChatId) throw new Error('YouTube: that video has no active live chat.');
    return this.liveChatId;
  }

  // One poll: the new messages, and how long the API asks us to wait.
  async poll() {
    const chatId = await this.resolveLiveChatId();
    const params = new URLSearchParams({ liveChatId: chatId, part: 'snippet,authorDetails', key: this.apiKey });
    if (this.pageToken) params.set('pageToken', this.pageToken);
    const response = await this.fetch(`${API}/liveChat/messages?${params}`);
    if (!response.ok) throw new Error(`YouTube: liveChatMessages.list failed with ${response.status}.`);
    const body = await response.json();
    const first = this.pageToken === null;
    this.pageToken = body.nextPageToken ?? this.pageToken;
    // The first page is history; she joins the conversation from now on.
    const items = first ? [] : (body.items ?? []);
    const messages = items.map(item => toChatMessage(item, this.config)).filter(Boolean);
    for (const message of messages) for (const handler of this.handlers.message) handler(message);
    return { messages, waitMs: Math.max(2000, Number(body.pollingIntervalMillis ?? 5000)) };
  }

  async run(signal) {
    if (!this.apiKey) throw new Error('YouTube: set YOUTUBE_API_KEY in the environment.');
    this.log(`YouTube: reading live chat${this.oauthToken ? ' and replying' : ' (read-only: no YOUTUBE_OAUTH_TOKEN)'}.`);
    while (!signal?.aborted) {
      let waitMs = 5000;
      try { ({ waitMs } = await this.poll()); }
      catch (error) { this.log(`YouTube: ${error.message}`); waitMs = 15000; }
      await new Promise(resolve => setTimeout(resolve, waitMs));
    }
  }

  async say(text) {
    if (!this.oauthToken) { this.log(`YouTube (read-only, not sent): ${text}`); return false; }
    const response = await this.fetch(`${API}/liveChat/messages?part=snippet`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${this.oauthToken}` },
      body: JSON.stringify({ snippet: { liveChatId: await this.resolveLiveChatId(), type: 'textMessageEvent',
        textMessageDetails: { messageText: text.slice(0, 200) } } }),
    });
    if (!response.ok) this.log(`YouTube: the reply was refused with ${response.status}.`);
    return response.ok;
  }
}
