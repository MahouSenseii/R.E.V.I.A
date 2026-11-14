// Local static QA only. Binds loopback and serves just the six public Atlas files.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../../docs/atlas');
const allowed=new Set(['index.html','research.html','atlas.css','public.css','atlas.js','milestone.json']);
const types={'.html':'text/html','.css':'text/css','.js':'text/javascript','.json':'application/json'};
const server=http.createServer((req,res)=>{
  // A request line such as "//" is not a URL at all; it is refused rather than crashing the preview.
  let name;
  try{name=new URL(req.url,'http://local').pathname.slice(1)||'index.html';}
  catch{res.writeHead(400);res.end('Bad request');return;}
  if(!allowed.has(name)){res.writeHead(404);res.end('Not found');return;}
  res.writeHead(200,{'Content-Type':types[path.extname(name)]+'; charset=utf-8','Cache-Control':'no-store'});
  fs.createReadStream(path.join(root,name)).pipe(res);
});
server.listen(8765,'127.0.0.1',()=>console.log('Atlas preview: http://127.0.0.1:8765'));
