import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { mkdtemp, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { InteractiveBrowser } from './interactiveHost.mjs';

test('isolated live form preserves hidden state and refuses stale targets', { timeout: 45000 }, async () => {
  const profile = await mkdtemp(path.join(os.tmpdir(), 'revia-interactive-test-'));
  const server = http.createServer((request, response) => {
    response.setHeader('content-type', 'text/html; charset=utf-8');
    response.end('<!doctype html><title>Owned browser fixture</title><label>Name<input id="name"></label>' +
      '<button id="save">Save</button><p id="result">Waiting</p><script>' +
      'let hidden="secret fixture state";document.querySelector("#save").onclick=()=>document.querySelector("#result").textContent=' +
      'document.querySelector("#name").value==="Ada"?"Saved Ada with "+hidden:"Wrong input";</script>');
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const url = `http://127.0.0.1:${server.address().port}/`;
  const grant = { enabled:true,navigate:true,interact:true,allowLoopback:true,approvedOrigins:[new URL(url).origin],
    timeoutMs:10000,maxTextBytes:4096,maxElements:40,maxValueBytes:4096 };
  const browser = new InteractiveBrowser(profile);
  try {
    const navigate = await browser.operate({operation:'navigate',url,grant});
    assert.equal(navigate.succeeded,true,navigate.message);
    const first=navigate.receipt;
    const field=first.elements.find(value=>value.editable);
    assert.ok(field,'The fixture input was not observed.');
    const fill=await browser.operate({operation:'fill',url,session:first.session,generation:first.generation,element:field.id,value:'Ada',grant});
    assert.equal(fill.succeeded,true,fill.message);
    const readback=fill.receipt.elements.find(value=>value.id===field.id);
    assert.ok(readback,'A field lost its stable identity after entering content.');
    assert.equal(readback.valueAvailable,true,'Exact browser field readback was unavailable.');
    assert.equal(readback.value,'Ada','The current receipt did not independently read the entered value.');
    const stale=await browser.operate({operation:'click',url,session:first.session,generation:first.generation,element:field.id,value:'',grant}).catch(error=>({succeeded:false,message:error.message}));
    assert.equal(stale.succeeded,false);
    const button=fill.receipt.elements.find(value=>value.name==='Save');
    const click=await browser.operate({operation:'click',url,session:fill.receipt.session,generation:fill.receipt.generation,element:button.id,value:'',grant});
    assert.equal(click.succeeded,true,click.message);
    assert.match(click.receipt.text,/Saved Ada with secret fixture state/);
    await browser.evaluate('document.querySelector("#save").textContent="Changed"');
    const changed=await browser.operate({operation:'click',url,session:click.receipt.session,generation:click.receipt.generation,
      element:click.receipt.elements.find(value=>value.name==='Save').id,value:'',grant});
    assert.equal(changed.succeeded,false);
    assert.equal(changed.attempted,false);
  } finally {
    await browser.stop(); server.closeAllConnections(); await new Promise(resolve=>server.close(resolve));
    if(path.dirname(profile)===os.tmpdir()&&path.basename(profile).startsWith('revia-interactive-test-'))
      await rm(profile,{recursive:true,force:true,maxRetries:10,retryDelay:200});
  }
});
