import { spawn } from 'node:child_process';
import http from 'node:http';
import path from 'node:path';

const executable = process.argv[2];
if (!executable) throw new Error('Pass the native browser fixture executable.');
const server = http.createServer((request, response) => {
  response.setHeader('content-type','text/html; charset=utf-8');
  response.end('<!doctype html><title>Native fixture</title><label>Name<input id="name"></label><button id="save">Save</button><p id="result">Waiting</p>' +
    '<script>let hidden="secret fixture state";document.querySelector("#save").onclick=()=>document.querySelector("#result").textContent=' +
    'document.querySelector("#name").value==="Ada"?"Saved Ada with "+hidden:"Wrong input";</script>');
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const child=spawn(path.resolve(executable),[],{stdio:'inherit',windowsHide:true,
  env:{...process.env,REVIA_BROWSER_FIXTURE_URL:`http://127.0.0.1:${server.address().port}/`}});
const timer=setTimeout(()=>child.kill(),45000);
const code=await new Promise(resolve=>{child.once('exit',resolve);child.once('error',()=>resolve(1));});
clearTimeout(timer);server.closeAllConnections();await new Promise(resolve=>server.close(resolve));
process.exit(code??1);
