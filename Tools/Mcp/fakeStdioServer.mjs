// A stdio MCP server that answers the three things the bridge and Revia use, for the
// bridge's own test: initialize, tools/list and tools/call (which echoes its arguments).
import readline from 'node:readline';

const lines = readline.createInterface({ input: process.stdin });
lines.on('line', line => {
  let message;
  try { message = JSON.parse(line); } catch { return; }
  if (!Object.hasOwn(message, 'id')) return;
  const reply = { jsonrpc: '2.0', id: message.id };
  if (message.method === 'initialize') {
    reply.result = { protocolVersion: '2025-06-18', capabilities: { tools: {} }, serverInfo: { name: 'fake-stdio', version: '0' } };
  } else if (message.method === 'tools/list') {
    reply.result = { tools: [{ name: 'echo', description: 'Echoes its arguments.', inputSchema: { type: 'object', properties: { text: { type: 'string' } }, required: ['text'] } }] };
  } else if (message.method === 'tools/call') {
    reply.result = { content: [{ type: 'text', text: JSON.stringify(message.params?.arguments ?? {}) }], isError: false };
  } else {
    reply.error = { code: -32601, message: 'Method not found' };
  }
  process.stdout.write(JSON.stringify(reply) + '\n');
});
