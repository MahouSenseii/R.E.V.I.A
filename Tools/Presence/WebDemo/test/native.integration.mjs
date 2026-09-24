import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { randomUUID } from 'node:crypto';
import { mkdtemp, rm, access } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRelay } from '../relay/server.js';
import { createBridge } from '../bridge/client.js';

const binary = process.env.REVIA_WEB_NATIVE_HOST || fileURLToPath(new URL('../../../../build/web-demo-check/ReviaWebGuestHost.exe', import.meta.url));
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(fn, label) {
  for (let i=0;i<300;i++) { const result=await fn(); if(result)return result; await pause(20); }
  assert.fail(`Timed out: ${label}`);
}

test('browser HTTP -> relay -> loopback WS bridge -> native ReplyPublic -> serialized model, with two guests and owner preemption', {timeout:60000}, async t => {
  await access(binary);
  const root=await mkdtemp(join(tmpdir(),'revia-web-integration-'));
  const requests=[];let hold=false;
  const model=http.createServer(async (req,res)=>{
    res.setHeader('Content-Type','application/json');
    if(req.url==='/health')return res.end('{"status":"ok","slots_idle":1}');
    if(req.url==='/v1/models')return res.end('{"data":[{"id":"local-model"}]}');
    if(req.url!=='/v1/chat/completions'){res.statusCode=404;return res.end('{}');}
    let raw='';for await(const chunk of req)raw+=chunk;
    const body=JSON.parse(raw);requests.push(body);
    res.setHeader('Content-Type','text/event-stream');
    if(hold){res.write('data: '+JSON.stringify({choices:[{delta:{reasoning_content:'INTERNAL_REASONING_CANARY'},finish_reason:null}]})+'\n\n');return;}
    const text='A maple leaf has a striking branching pattern.';
    res.end('data: '+JSON.stringify({choices:[{delta:{content:text},finish_reason:'stop'}]})+'\n\ndata: [DONE]\n\n');
  });
  model.listen(0,'127.0.0.1');await once(model,'listening');
  const localToken='local-test-only-'.repeat(4),hostToken='host-test-only-'.repeat(4),inviteCode='invite-test-only-'.repeat(4);
  const native=spawn(binary,[String(model.address().port),'0'],{cwd:root,env:{...process.env,REVIA_WEB_LOCAL_TOKEN:localToken},windowsHide:true,stdio:['pipe','pipe','pipe']});
  let stdout='',stderr='';native.stdout.on('data',b=>stdout+=b);native.stderr.on('data',b=>stderr+=b);
  const exited=once(native,'exit');
  let relay,bridge,lastNative;
  const realFetch=globalThis.fetch;
  globalThis.fetch=async (...args)=>{const result=await realFetch(...args);if(String(args[0]).endsWith('/web/v1/turn'))lastNative={status:result.status,body:await result.clone().json()};return result;};
  t.after(async()=>{
    await bridge?.stop();await relay?.close();
    globalThis.fetch=realFetch;
    if(native.exitCode===null){native.stdin.write('quit\n');const ended=await Promise.race([exited,pause(5000).then(()=>null)]);if(!ended)native.kill();}
    model.closeAllConnections();await new Promise(resolve=>model.close(resolve));
    await rm(root,{recursive:true,force:true});
  });
  const localPort=await until(()=>{const line=stdout.split('\n').find(s=>s.startsWith('{'));return line&&JSON.parse(line).port;},'native startup');
  relay=createRelay({mode:'development',bind:'127.0.0.1',origins:['http://127.0.0.1:9000'],hostId:'demo',hostToken,inviteCode,limits:{issuePerMinute:20}});
  await relay.listen(0);const base=`http://127.0.0.1:${relay.server.address().port}`;
  bridge=createBridge({mode:'development',relayUrl:base.replace('http:','ws:')+'/v1/host',hostId:'demo',hostToken,localUrl:`http://127.0.0.1:${localPort}`,localToken,heartbeatMs:100});bridge.start();
  async function api(path,token,body,method=body?'POST':'GET'){
    const r=await fetch(base+path,{method,headers:{Origin:'http://127.0.0.1:9000',...(token?{Authorization:`Bearer ${token}`} : {}),...(body?{'Content-Type':'application/json'}:{})},...(body?{body:JSON.stringify(body)}:{})});
    return {status:r.status,body:r.status===204?null:await r.json()};
  }
  await until(async()=>(await api('/v1/status')).body.state==='online','native readiness propagated');
  const a=(await api('/v1/sessions',null,{inviteCode})).body,b=(await api('/v1/sessions',null,{inviteCode})).body;
  const key=randomUUID(),first=(await api('/v1/messages',a.token,{text:'GUEST_A_CANARY: Describe maple leaves.',idempotencyKey:key})).body;
  const retry=(await api('/v1/messages',a.token,{text:'GUEST_A_CANARY: Describe maple leaves.',idempotencyKey:key})).body;
  assert.equal(retry.requestId,first.requestId);
  await until(async()=>{const r=await api('/v1/messages/'+first.requestId,a.token);if(['failed','cancelled','expired'].includes(r.body.state))assert.fail('First native request ended: '+JSON.stringify({result:r,lastNative,requests:requests.length,nativeStderr:stderr}));return r.body.state==='completed';},'first final reply');
  assert.equal(requests.length,1,'Lost-response retry does not duplicate model execution');
  assert.equal((await api('/v1/messages/'+first.requestId,a.token)).body.text,'A maple leaf has a striking branching pattern.');
  assert.equal((await api('/v1/messages/'+first.requestId,b.token)).status,404);
  const second=(await api('/v1/messages',b.token,{text:'Describe maple leaves.',idempotencyKey:randomUUID()})).body;
  await until(async()=>(await api('/v1/messages/'+second.requestId,b.token)).body.state==='completed','second guest reply');
  assert.ok(!JSON.stringify(requests.at(-1)).includes('GUEST_A_CANARY'));
  assert.ok(JSON.stringify(requests.at(-1)).includes('Revia'));
  assert.ok(!requests.at(-1).tools);
  hold=true;
  const pending=(await api('/v1/messages',a.token,{text:'CANCELLED_INPUT_CANARY',idempotencyKey:randomUUID()})).body;
  await until(()=>requests.length===3,'native generation running');native.stdin.write('busy\n');
  await until(async()=>(await api('/v1/messages/'+pending.requestId,a.token)).body.state==='cancelled','owner preempted guest');
  assert.ok(!JSON.stringify((await api('/v1/messages/'+pending.requestId,a.token)).body).includes('CANARY'));
  hold=false;native.stdin.write('ready\n');await until(async()=>(await api('/v1/status')).body.state==='online','owner finished');
  const after=(await api('/v1/messages',a.token,{text:'What shape is the leaf?',idempotencyKey:randomUUID()})).body;
  await until(async()=>(await api('/v1/messages/'+after.requestId,a.token)).body.state==='completed','after preemption');
  assert.ok(!JSON.stringify(requests.at(-1)).includes('CANCELLED_INPUT_CANARY'));
  await api('/v1/session',a.token,undefined,'DELETE');
  assert.equal((await api('/v1/messages/'+first.requestId,a.token)).status,401);
  for(let i=0;i<11;i++){
    const visitor=(await api('/v1/sessions',null,{inviteCode})).body;
    const turn=(await api('/v1/messages',visitor.token,{text:'Describe a maple leaf.',idempotencyKey:randomUUID()})).body;
    await until(async()=>{const r=await api('/v1/messages/'+turn.requestId,visitor.token);assert.notEqual(r.body.state,'failed','Ending guests must release native live capacity');return r.body.state==='completed';},'sequential guest '+i);
    await api('/v1/session',visitor.token,undefined,'DELETE');
  }
  const disabled=await fetch(`http://127.0.0.1:${localPort}/web/v1/control`,{method:'POST',headers:{Authorization:`Bearer ${localToken}`,'Content-Type':'application/json'},body:JSON.stringify({enabled:false,paused:false})});
  assert.equal((await disabled.json()).state,'offline');
  await until(async()=>(await api('/v1/status')).body.state==='offline','owner disable stopped connector');
  assert.equal((await api('/v1/messages/'+second.requestId,b.token)).status,401);
  native.stdin.write('quit\n');const [code]=await exited;assert.equal(code,0);assert.equal(stderr,'');
});
