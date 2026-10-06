const phases = new Set(['offline', 'idle', 'listening', 'thinking', 'responding', 'speaking',
    'acting', 'waiting', 'blocked', 'error']);
const clamp = (value, min = 0, max = 1) => Math.max(min, Math.min(max, value));

export const parameterDefinitions = [
    ['ReviaMouthGate', 'Output-audio opening; legacy voices use a speaking gate. No visemes.', 0, 1],
    ['ReviaJoy', 'Happy, excited and playful expression strength.', 0, 1],
    ['ReviaSadness', 'Sad, lonely and sulky expression strength.', 0, 1],
    ['ReviaAnger', 'Angry and frustrated expression strength.', 0, 1],
    ['ReviaFocus', 'Focused, curious and concerned expression strength.', 0, 1],
    ['ReviaListening', 'Current listening activity cue.', 0, 1],
    ['ReviaGazeX', 'Screen attention offset; not measured eye tracking.', -1, 1],
    ['ReviaEngagement', 'Conversation momentum for restrained idle motion.', 0, 1]
].map(([parameterName, explanation, min, max]) => ({ parameterName, explanation, min, max, defaultValue: 0 }));

export function validateEndpoint(endpoint)
{
    const url = new URL(endpoint);
    if (url.protocol !== 'ws:' || !['localhost', '127.0.0.1', '[::1]'].includes(url.hostname) ||
        url.username || url.password || url.pathname !== '/' || url.search || url.hash)
        throw new Error('The renderer endpoint must be a plain loopback WebSocket URL.');
    return endpoint;
}

export function mapSnapshot(snapshot, now = Date.now())
{
    const values = Object.fromEntries(parameterDefinitions.map(item => [item.parameterName, 0]));
    const timestamp = Date.parse(snapshot?.timestamp);
    const valid = snapshot?.version === 1 && Number.isSafeInteger(snapshot.sequence) && snapshot.sequence >= 0 &&
        Number.isFinite(timestamp) && timestamp <= now + 5000 && phases.has(snapshot.phase) &&
        ['mouth', 'affect_intensity', 'conversation_momentum'].every(key => Number.isFinite(snapshot[key]));
    if (!valid || snapshot.phase === 'offline')
        return { faceFound: false, mode: 'set', parameterValues: entries(values) };

    // Presence is event-only; this bounds a stuck mouth without claiming producer liveness.
    const transientFresh = now - timestamp <= 15000;
    const strength = clamp(snapshot.affect_intensity);
    const expression = snapshot.expression;
    if (['happy', 'excited', 'playful'].includes(expression)) values.ReviaJoy = strength;
    if (['sad', 'melancholy', 'lonely', 'sulky', 'bored'].includes(expression)) values.ReviaSadness = strength;
    if (['angry', 'frustrated'].includes(expression)) values.ReviaAnger = strength;
    if (['focused', 'curious', 'concerned', 'confused'].includes(expression)) values.ReviaFocus = strength;
    if (snapshot.phase === 'thinking') values.ReviaFocus = Math.max(values.ReviaFocus, 0.35);
    values.ReviaEngagement = clamp(snapshot.conversation_momentum);
    values.ReviaGazeX = snapshot.gaze_target === 'screen' ? 0.35 : 0;
    values.ReviaMouthGate = snapshot.mouth_track != null
        ? playbackMouth(snapshot.mouth_track, snapshot.speaking === true, now)
        : transientFresh && snapshot.phase === 'speaking' && snapshot.speaking === true ? clamp(snapshot.mouth) : 0;
    values.ReviaListening = transientFresh && snapshot.phase === 'listening' && snapshot.listening === true ? 1 : 0;
    return { faceFound: true, mode: 'set', parameterValues: entries(values) };
}

function playbackMouth(track, speaking, now)
{
    if (!speaking || !Number.isSafeInteger(track.started_at_ms) || track.started_at_ms <= 0 ||
        track.started_at_ms > now + 5000 || track.interval_ms !== 50 ||
        !Array.isArray(track.values) || track.values.length === 0 || track.values.length > 2400 ||
        !track.values.every(value => Number.isInteger(value) && value >= 0 && value <= 255)) return 0;
    const index = Math.floor((now - track.started_at_ms) / track.interval_ms);
    return index >= 0 && index < track.values.length ? track.values[index] / 255 : 0;
}

function entries(values)
{
    return Object.entries(values).map(([id, value]) => ({ id, value, weight: 1 }));
}
