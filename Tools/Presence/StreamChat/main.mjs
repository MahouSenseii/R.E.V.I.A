// Twitch, YouTube and OBS for a running Revia.
//
//   node main.mjs config.json [--check]
//
// Reads chat from whichever platforms the config enables, hands each message to Revia
// through her Presence inbox, sends the reply she writes to the outbox back to the
// platform, and keeps OBS in step with her avatar state and caption. Tokens come from
// the environment only: TWITCH_OAUTH_TOKEN, YOUTUBE_API_KEY, YOUTUBE_OAUTH_TOKEN,
// OBS_WEBSOCKET_PASSWORD.
import { readFile, stat } from 'node:fs/promises';
import path from 'node:path';
import { validateConfig, PresenceEnvelope } from './envelope.mjs';
import { TwitchChat } from './twitch.mjs';
import { YouTubeChat } from './youtube.mjs';
import { ObsClient } from './obs.mjs';

const config = validateConfig(JSON.parse(await readFile(process.argv[2] ?? new URL('./config.json', import.meta.url), 'utf8')));
for (const directory of [config.inbox, config.outbox]) {
  if (!(await stat(directory).catch(() => null))?.isDirectory())
    throw new Error(`Start Revia first and point the config at its Presence directories (missing ${directory}).`);
}
const enabled = ['twitch', 'youtube', 'obs'].filter(key => config[key]?.enabled);
if (process.argv.includes('--check')) {
  console.log(`Configuration is valid. Enabled: ${enabled.join(', ') || 'nothing'}. No connection opened.`);
  process.exit(0);
}
if (enabled.length === 0) throw new Error('Enable at least one of twitch, youtube, obs in the config.');

const envelope = new PresenceEnvelope(config);
const shutdown = new AbortController();
const clients = [];
let stopping = false;
function stop(reason) {
  if (stopping) return;
  stopping = true; shutdown.abort();
  for (const client of clients) client.close?.();
  console.log(reason);
}
process.once('SIGINT', () => stop('Stream connectors stopped.'));
process.once('SIGTERM', () => stop('Stream connectors stopped.'));

// One message in, one reply out: the platform never sees anything Revia did not write
// to the outbox, and a message she passed over gets no reply at all.
function relay(source, client) {
  return async message => {
    try {
      await envelope.submit({ ...message, source });
      const reply = await envelope.awaitReply(source, message.id, shutdown.signal);
      if (reply?.succeeded && reply.text) await client.say(reply.text);
      else if (reply && reply.reason) console.log(`${source}: not answered (${reply.reason}).`);
    } catch (error) { if (!shutdown.signal.aborted) console.log(`${source}: ${error.message}`); }
  };
}

const runs = [];
if (config.twitch?.enabled) {
  const twitch = new TwitchChat({ ...config.twitch, name: config.name, replyMode: config.replyMode });
  twitch.on('message', relay('stream', twitch));
  clients.push(twitch);
  runs.push(twitch.connect().then(() => stop('Twitch chat closed the connection; restart the connector.')));
}
if (config.youtube?.enabled) {
  const youtube = new YouTubeChat({ ...config.youtube, name: config.name, replyMode: config.replyMode });
  youtube.on('message', relay('stream', youtube));
  clients.push(youtube);
  runs.push(youtube.run(shutdown.signal));
}
if (config.obs?.enabled) {
  const obs = new ObsClient(config.obs);
  clients.push(obs);
  const statePath = config.obs.statePath ?? path.join(path.dirname(config.inbox), 'avatar_state.json');
  const captionPath = config.obs.captionPath ?? path.join(path.dirname(config.inbox), 'caption.txt');
  runs.push(obs.connect().then(async () => {
    while (!shutdown.signal.aborted) {
      await obs.sync({ statePath, captionPath }).catch(error => console.log(`OBS: ${error.message}`));
      await new Promise(resolve => setTimeout(resolve, 500));
    }
  }));
}
setInterval(() => envelope.sweep('stream').catch(() => {}), 60000).unref();
await Promise.allSettled(runs);
