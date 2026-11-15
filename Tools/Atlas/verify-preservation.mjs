import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {earlierRecords} from './data.mjs';
import {counts,capabilities} from './retained.mjs';
const base=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const git=process.env.ATLAS_GIT_BIN||'git';
const firstPublished='d51a2b38fb87f3d0dd63a5e4fcf2796de219ef4a';
// The pages no longer carry the dated records. They link to the commit that does, and
// this checks that the linked commit really holds them unchanged, by comparing it with
// the commit the records were first published from. Both must be in the clone, which a
// shallow clone does not guarantee.
for(const commit of [firstPublished,earlierRecords]){
  try{execFileSync(git,['cat-file','-e',commit+'^{commit}'],{cwd:base,stdio:'ignore'});}
  catch{throw new Error('Commit '+commit.slice(0,8)+' is not in this clone. Fetch full history (git fetch --unshallow) and run this again.');}
}
const at=(commit,name)=>execFileSync(git,['show',commit+':docs/atlas/'+name],{cwd:base,encoding:'utf8',maxBuffer:4e6});
const read=name=>fs.readFileSync(path.join(base,'docs/atlas',name),'utf8');
const normalize=s=>s.replaceAll('\r\n','\n');
const sha=s=>crypto.createHash('sha256').update(s).digest('hex');
const files=fs.readdirSync(path.join(base,'docs/atlas'));
const result={checked_utc:new Date().toISOString(),first_published:firstPublished,earlier_records_commit:earlierRecords,earlier_records:{}};
for(const name of ['index.html','research.html']){
  const original=at(firstPublished,name);
  const main=original.slice(original.indexOf('>',original.indexOf('<main'))+1,original.lastIndexOf('</main>')).replace(/<header>[\s\S]*?<\/header>/,'').replace(/<footer>[\s\S]*?<\/footer>/,'');
  const retained=at(earlierRecords,name).split('<!-- BEGIN RETAINED RECORD -->')[1].split('<!-- END RETAINED RECORD -->')[0];
  assert.equal(normalize(retained),normalize(main),'the linked commit does not hold the original record: '+name);
  result.earlier_records[name]={original_record_exact_at_linked_commit:true,normalized_sha256:sha(normalize(retained))};
}
const archived=JSON.parse(at(earlierRecords,'milestone.json'));
assert.deepEqual(archived.history,JSON.parse(at(firstPublished,'milestone.json')));
// The two parts of that snapshot the pages still show are exactly as it recorded them.
assert.deepEqual(capabilities,archived.history.capabilities);
assert.deepEqual(counts,archived.history.counts);
result.earlier_records.milestone_json={full_previous_snapshot_at_linked_commit:true,shown_capabilities_and_counts_match:true};
// The published pages are exactly what the generator writes. Line endings are a checkout
// setting, not content: Pages serves what Git stores.
const before=Object.fromEntries(files.map(f=>[f,sha(normalize(read(f)))]));
execFileSync(process.execPath,['Tools/Atlas/build.mjs'],{cwd:base});
const after=Object.fromEntries(files.map(f=>[f,sha(normalize(read(f)))]));
assert.deepEqual(after,before,'rebuild must be byte-stable');
result.rebuild_byte_stable=true;
result.files=files.map(f=>({name:f,bytes:Buffer.byteLength(normalize(read(f))),sha256:after[f]}));
result.artifact_bytes=result.files.reduce((sum,f)=>sum+f.bytes,0);
// A tracking document, not an archive. The publish check's own outer bound is 500,000.
assert.ok(result.artifact_bytes<250000,'the Atlas has grown to '+result.artifact_bytes+' bytes');
result.tracking_size_limit_passed=true;
// Only that this checkout has no edits to the workflow, staged or not. It says nothing
// about how the workflow differs from any earlier commit.
assert.equal(execFileSync(git,['diff','HEAD','--','.github/workflows/atlas-pages.yml'],{cwd:base,encoding:'utf8'}),'');
result.pages_workflow_matches_head=true;
console.log(JSON.stringify(result,null,2));
