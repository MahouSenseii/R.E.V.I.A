// Explicit opt-in local smoke. Never runs in npm test or CI; owns only its children.
import assert from 'node:assert/strict';
import http from 'node:http';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { randomBytes } from 'node:crypto';
import { createRequire } from 'node:module';
import { mkdtemp, rm, readFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { resolve, join, extname, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRelay } from '../relay/server.js';
import { createBridge } from '../bridge/client.js';

const repo=fileURLToPath(new URL('../../../../',import.meta.url));
const portfolio=process.env.REVIA_WEB_PORTFOLIO_ROOT;
const modelPath=process.env.REVIA_WEB_REAL_MODEL;
assert.ok(portfolio&&modelPath,'Set REVIA_WEB_PORTFOLIO_ROOT and REVIA_WEB_REAL_MODEL to existing local paths');
const {chromium}=createRequire(join(portfolio,'package.json'))('@playwright/test');
const nativePath=process.env.REVIA_WEB_NATIVE_HOST||join(repo,'build/web-demo-check/ReviaWebGuestHost.exe');
const serverPath=process.env.REVIA_WEB_LLAMA_SERVER||join(repo,'ThirdParty/llama.cpp/llama-server.exe');
const secret=()=>randomBytes(32).toString('hex');
const localToken=secret(),hostToken=secret(),inviteCode=secret(),modelToken=secret();
const root=await mkdtemp(join(tmpdir(),'revia-live-smoke-'));
const pause=ms=>new Promise(r=>setTimeout(r,ms));
async function until(fn,ms,label){const end=Date.now()+ms;while(Date.now()<end){if(await fn())return;await pause(200);}throw Error('Timed out: '+label);}
const children=[];let relay,bridge,browser,staticServer;
function child(path,args,env){const p=spawn(path,args,{cwd:root,env:{...process.env,...env},windowsHide:true,stdio:['pipe','pipe','pipe']});children.push(p);p.on('error',error=>{p.spawnError=error;});p.stderr.resume();return p;}
try {
  const reserve=http.createServer();reserve.listen(0,'127.0.0.1');await once(reserve,'listening');const modelPort=reserve.address().port;await new Promise(r=>reserve.close(r));
  const model=child(serverPath,['--model',resolve(modelPath),'--host','127.0.0.1','--port',String(modelPort),'--alias','local-model','--ctx-size','8192','--parallel','1','--gpu-layers','auto','--no-webui','--log-disable','--reasoning','off'],{LLAMA_API_KEY:modelToken});model.stdout.resume();
  await until(async()=>{if(model.spawnError)throw model.spawnError;assert.equal(model.exitCode,null,'Model exited before readiness');try{return(await fetch(`http://127.0.0.1:${modelPort}/health`,{headers:{Authorization:`Bearer ${modelToken}`},signal:AbortSignal.timeout(1500)})).ok;}catch{return false;}},180000,'real model loading');
  console.log('Real local model ready.');
  const native=child(nativePath,[String(modelPort),'0'],{REVIA_WEB_LOCAL_TOKEN:localToken,REVIA_WEB_MODEL_TOKEN:modelToken});
  let stdout='',localPort;native.stdout.on('data',b=>stdout+=b);
  await until(()=>{if(native.spawnError)throw native.spawnError;assert.equal(native.exitCode,null,'Native exited before startup');const line=stdout.split('\n').slice(0,-1).find(s=>s.startsWith('{'));if(line)localPort=JSON.parse(line).port;return localPort;},10000,'native startup');
  const types={'.html':'text/html','.js':'text/javascript','.css':'text/css','.json':'application/json','.svg':'image/svg+xml','.webp':'image/webp'};
  const dist=resolve(portfolio,'dist');
  staticServer=http.createServer(async(req,res)=>{const path=new URL(req.url,'http://local').pathname.replace(/^\/Portfolio(?=\/)/,'');const file=resolve(dist,'.'+(path.endsWith('/')?path+'index.html':path));if(!file.startsWith(dist+sep)){res.writeHead(403).end();return;}try{const data=await readFile(file);res.writeHead(200,{'Content-Type':types[extname(file)]||'application/octet-stream'}).end(data);}catch{res.writeHead(404).end();}});
  staticServer.listen(0,'127.0.0.1');await once(staticServer,'listening');const origin=`http://127.0.0.1:${staticServer.address().port}`;
  relay=createRelay({mode:'development',bind:'127.0.0.1',origins:[origin],hostId:'smoke',hostToken,inviteCode});await relay.listen(0);const base=`http://127.0.0.1:${relay.server.address().port}`;
  bridge=createBridge({mode:'development',relayUrl:base.replace('http:','ws:')+'/v1/host',hostId:'smoke',hostToken,localUrl:`http://127.0.0.1:${localPort}`,localToken});bridge.start();
  await until(async()=>(await(await fetch(base+'/v1/status',{headers:{Origin:origin}})).json()).state==='online',15000,'native readiness');
  browser=await chromium.launch({headless:true});const page=await browser.newPage();
  await page.route('**/data/revia-demo.json',r=>r.fulfill({json:{enabled:true,relayUrl:'https://local-smoke.invalid'}}));
  // Test-only URL rewrite preserves production HTTPS-only validation. Responses
  // come from the real local relay; this does not verify public TLS or DNS.
  await page.route('https://local-smoke.invalid/**',async r=>{const req=r.request();const headers={...req.headers(),origin};delete headers.host;const result=await fetch(base+new URL(req.url()).pathname,{method:req.method(),headers,...(req.postData()?{body:req.postData()}:{}),signal:AbortSignal.timeout(15000)});await r.fulfill({status:result.status,headers:Object.fromEntries(result.headers),body:await result.text()});});
  await page.goto(origin+'/Portfolio/#revia.html');
  await page.getByLabel('Invitation code').fill(inviteCode);
  await page.getByRole('button',{name:'Start session'}).click();
  await page.getByLabel('Your message').fill('Hi Revia! In one short sentence, what makes a maple leaf interesting?');
  const started=Date.now();await page.getByRole('button',{name:'Send message',exact:true}).click();
  const reply=page.locator('[data-transcript] [data-speaker="Revia"] p');
  await reply.waitFor({state:'visible',timeout:125000});const text=await reply.textContent();
  assert.ok(text.trim());assert.ok(!text.includes('<think>'));assert.ok(!text.includes('reasoning_content'));
  const evidence={result:'passed',route:'/Portfolio/#revia.html',elapsedMs:Date.now()-started,reply:text,transport:'local HTTP/WS with test-only browser URL rewrite; public TLS unverified'};
  await page.getByRole('button',{name:'End session & clear',exact:true}).click();
  await browser.close();browser=null;
  await bridge.stop();bridge=null;
  await until(async()=>(await(await fetch(base+'/v1/status',{headers:{Origin:origin}})).json()).state==='offline',5000,'relay observed connector shutdown');
  const exit=once(native,'exit');native.stdin.write('quit\n');const ended=await Promise.race([exit,pause(5000).then(()=>null)]);assert.ok(ended,'Native shutdown timed out');assert.equal(ended[0],0);
  console.log(JSON.stringify(evidence));
} finally {
  await browser?.close();await bridge?.stop();await relay?.close();
  if(staticServer){staticServer.closeAllConnections();await new Promise(r=>staticServer.close(r));}
  for(const p of children){if(p.pid&&p.exitCode===null){const done=once(p,'exit');p.kill();await done;}}
  await rm(root,{recursive:true,force:true});
}
