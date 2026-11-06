import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../docs/atlas');
const read=name=>fs.readFileSync(path.join(root,name),'utf8');
const record=()=>JSON.parse(read('milestone.json'));
const files=['atlas.css','atlas.js','index.html','milestone.json','public.css','research.html'];
const denied=/([a-z]:[\\/](?:Users|Windows|Program Files)|\/home\/|\/Users\/|libfile_|file_00000000|https?:\/\/localhost|https?:\/\/127\.0\.0\.1|BEGIN [A-Z ]*PRIVATE KEY|(?:api[_-]?key|access[_-]?token)\s*[=:])/i;

test('the current entry point uses the verified source and scoped CI result',()=>{
  const j=record();
  assert.equal(j.current.source_commit,'375ea55fcb4d64bcd6a9c90c851f29cf50ae2c53');
  assert.equal(j.current.source_pr,45);
  assert.deepEqual(j.current.ci,{run:42216017963,jobs_passed:6,passed:155,failed:0,optional_skips:2});
  assert.equal(j.source.installed_app_updated,false);
  assert.equal(j.source.release_qualified,false);
});
test('research does not turn saved replies into a comparison or local evidence into public files',()=>{
  const r=record().current.research;
  assert.equal(r.comparison_complete,false);
  assert.equal(r.graded,false);
  assert.equal(r.saved_responses,32);
  assert.equal(r.integration,'None');
  assert.equal(r.local_studies,'STATE-011–016');
  assert.equal(r.proposed_study,'STATE-017');
  assert.match(read('research.html'),/963\/1,008/);
  assert.match(read('research.html'),/1,008\/1,008/);
});
test('all public files preserve the original artifact and privacy boundary',()=>{
  assert.deepEqual(fs.readdirSync(root).sort(),files);
  const bytes=files.reduce((n,f)=>n+Buffer.byteLength(read(f)),0);
  assert.ok(bytes<500000,'artifact '+bytes+' exceeds original bound');
  for(const f of files)assert.ok(!denied.test(read(f)),'private/local content in '+f);
  for(const f of files)assert.equal(fs.lstatSync(path.join(root,f)).isSymbolicLink(),false);
  assert.equal(record().product_delivery,'Merged source on GitHub main; installed app unchanged; no application deployment');
  assert.equal(record().integration.installed_app_updated,false);
  assert.equal(record().integration.runtime_release_qualified,false);
  const c=record().counts;
  assert.equal(c.passing_controlled_evidence_after_rechecks+c.remaining_setup_qualifications+c.not_run_build_blocked+c.not_selected_safety,c.registered);
  assert.equal(c.raw_zero_exit+c.raw_failed,c.executed_unique);
});
test('dated evidence is retained without presenting it as the current snapshot',()=>{
  assert.equal(record().history.recorded_utc,'2026-11-03T16:37:17.085871+00:00');
  for(const name of ['index.html','research.html']){
    assert.match(read(name),/BEGIN RETAINED RECORD/);
    assert.match(read(name),/Dated records/);
    const ids=[...read(name).matchAll(/\bid="([^"]+)"/g)].map(m=>m[1]);
    assert.equal(new Set(ids).size,ids.length,'duplicate IDs in '+name);
    for(const m of read(name).matchAll(/href="#([^"]+)"/g)) assert.ok(ids.includes(m[1]),'broken fragment #'+m[1]+' in '+name);
  }
});
test('current website status identifies the verified live checkpoint and dates the earlier failure',()=>{
  const pages=record().current.pages;
  assert.equal(pages.state,'published');
  assert.deepEqual(pages.verified_publication,{
    run:41961109918,
    source_commit:'ac755b28d635676e398623841b7c4c49eee5416c',
    url:'https://mahousenseii.github.io/R.E.V.I.A/',
    verified_utc:'2026-11-05T03:23:23.246Z'
  });
  assert.equal(pages.previous_failure.run,41893563372);
  for(const name of ['index.html','research.html']){
    const active=read(name).split('<!-- BEGIN RETAINED RECORD -->')[0];
    assert.match(active,/Published Atlas snapshot/);
    assert.doesNotMatch(active,/Local redesign preview|The deployment stalled\.|This redesign is a local review copy|This redesign is local only/);
  }
  const active=read('index.html').split('<!-- BEGIN RETAINED RECORD -->')[0];
  assert.match(active,/Earlier deployment failure \(4 November 2026\)/);
  assert.match(active,/actions\/runs\/41961109918/);
});
