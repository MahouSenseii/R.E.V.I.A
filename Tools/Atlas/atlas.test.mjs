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

test('the retained main checkpoint keeps its source and scoped CI result',()=>{
  const j=record();
  assert.equal(j.current.source_commit,'375ea55fcb4d64bcd6a9c90c851f29cf50ae2c53');
  assert.equal(j.current.source_pr,45);
  assert.deepEqual(j.current.ci,{run:42216017963,jobs_passed:6,passed:155,failed:0,optional_skips:2});
  assert.equal(j.source.installed_app_updated,false);
  assert.equal(j.source.release_qualified,false);
});
test('research reports the finished comparison and closeout without overclaiming or publishing local evidence',()=>{
  const r=record().current.research;
  assert.equal(r.comparison_complete,true);
  assert.equal(r.graded,true);
  assert.equal(r.saved_responses,32);
  assert.equal(r.integration,'None');
  assert.equal(r.candidate,'None');
  assert.equal(r.local_studies,'STATE-011–020');
  assert.equal(r.proposed_study,'None');
  const page=read('research.html');
  assert.match(page,/963\/1,008/);
  assert.match(page,/1,008\/1,008/);
  assert.match(page,/17\/32/);
  assert.match(page,/not proof that RNL cannot work/);
  assert.match(page,/not proof that every hybrid is useless/);
  assert.match(page,/sealed test set was never opened/);
});
test('all public files preserve the original artifact and privacy boundary',()=>{
  assert.deepEqual(fs.readdirSync(root).sort(),files);
  // Counted as Git stores and Pages serves it, whatever line endings this checkout uses.
  const bytes=files.reduce((n,f)=>n+Buffer.byteLength(read(f).replaceAll('\r\n','\n')),0);
  assert.ok(bytes<500000,'artifact '+bytes+' exceeds original bound');
  // The Atlas tracks what was done and what happened. It is not the archive: when an
  // update takes it past this size, older entries move out to the repository history.
  assert.ok(bytes<250000,'the Atlas has grown to '+bytes+' bytes; it is meant to stay a small tracking document');
  for(const f of files)assert.ok(!denied.test(read(f)),'private/local content in '+f);
  for(const f of files)assert.equal(fs.lstatSync(path.join(root,f)).isSymbolicLink(),false);
  assert.equal(record().product_delivery,'Merged source on GitHub main; installed app unchanged; no application deployment');
  assert.equal(record().integration.installed_app_updated,false);
  assert.equal(record().integration.runtime_release_qualified,false);
  const c=record().counts;
  assert.equal(c.passing_controlled_evidence_after_rechecks+c.remaining_setup_qualifications+c.not_run_build_blocked+c.not_selected_safety,c.registered);
  assert.equal(c.raw_zero_exit+c.raw_failed,c.executed_unique);
});
test('earlier records are linked from the pages, not copied into every update',()=>{
  const earlier='42853a828502793bc53edf32ea7179547e32ea1f';
  assert.equal(record().history,undefined);
  assert.equal(record().earlier_records.commit,earlier);
  assert.equal(record().earlier_records.url,'https://github.com/MahouSenseii/R.E.V.I.A/tree/'+earlier+'/docs/atlas');
  for(const name of ['index.html','research.html']){
    assert.doesNotMatch(read(name),/RETAINED RECORD|class="legacy"|record-drawer/);
    assert.ok(read(name).includes('/tree/'+earlier+'/docs/atlas'),'no link to the earlier records in '+name);
    const ids=[...read(name).matchAll(/\bid="([^"]+)"/g)].map(m=>m[1]);
    assert.equal(new Set(ids).size,ids.length,'duplicate IDs in '+name);
    for(const m of read(name).matchAll(/href="#([^"]+)"/g)) assert.ok(ids.includes(m[1]),'broken fragment #'+m[1]+' in '+name);
  }
});
test('release snapshot keeps historical publication evidence separate',()=>{
  const pages=record().current.pages;
  assert.equal(pages.state,'Atlas release snapshot');
  assert.deepEqual(pages.verified_publication,{
    run:41961109918,
    source_commit:'ac755b28d635676e398623841b7c4c49eee5416c',
    url:'https://mahousenseii.github.io/R.E.V.I.A/',
    verified_utc:'2026-11-05T03:23:23.246Z'
  });
  assert.equal(pages.previous_failure.run,41893563372);
  for(const name of ['index.html','research.html']){
    const active=read(name);
    assert.match(active,/Atlas release snapshot/);
    assert.match(active,/Normal update verified, not launched/);
    assert.doesNotMatch(active,/Local redesign preview|The deployment stalled\.|This redesign is a local review copy|This redesign is local only/);
  }
  const active=read('index.html');
  assert.match(active,/Earlier deployment failure \(4 November 2026\)/);
  assert.match(active,/actions\/runs\/41961109918/);
});

test('November 8 installation and permitted checks retain historical scope and limits',()=>{
  const j=record(),v=j.current.validation;
  assert.equal(j.current.as_of,'2026-11-08');
  assert.equal(v.candidate_base,'c5ef5599d8fb22f90bb2fd2c3b7622bf22f18e19');
  assert.equal(v.includes_uncommitted_changes,true);
  assert.deepEqual(v.native,{eligible:128,attempted:128,passing_receipts:128,failed:0,skipped:0,incomplete:0,timeouts:0,wrapper_seconds:503.9956644,unchanged_pins:1854});
  assert.equal(v.earlier_native.first_pass_passed,122);
  assert.equal(v.earlier_native.short_path_recoveries,5);
  assert.equal(v.build.targets,124);
  assert.equal(v.memory_guard.result,'GREEN');
  assert.equal(v.memory_guard.new_rejects,4);
  assert.equal(v.memory_guard.controls,13);
  assert.equal(v.normal_update.applied,true);
  assert.equal(v.normal_update.independently_verified,true);
  assert.equal(v.normal_update.safeguard_tests_passed,80);
  assert.equal(v.normal_update.files,2);
  assert.equal(v.normal_update.installed_utc,'2026-11-08T02:14:56Z');
  assert.equal(v.normal_update.safeguard_success,1);
  assert.equal(v.normal_update.safeguard_expected_holds,79);
  assert.equal(v.normal_update.independent_checks,20137);
  assert.equal(v.normal_update.ui_loaders_passed,2);
  assert.equal(v.normal_update.displaced_originals_preserved,true);
  assert.equal(v.earlier_qualified_native.unchanged_pins,1573);
  assert.equal(v.earlier_normal_update.files,4);
  assert.match(v.native_limit,/31 excluded/);
  assert.match(v.native_limit,/16 microphone functions/);
  assert.match(v.native_scope,/83 passes and case 84 unknown/);
  assert.equal(v.normal_update.normal_app_launched,false);
  assert.equal(v.normal_update.config_or_memory_changed,false);
  assert.doesNotMatch(read('index.html'),/postcheck pending|verification pending|verification is pending|postcheck is pending/i);
  assert.deepEqual(v.catalog,{configured:159,source_expected:160,missing:'Revia.RetrievalBenchmarkContracts (NumPy unavailable)'});
  assert.equal(v.model.semantic_result,'PARTIAL');
  assert.equal(v.model.pending_proposals,0);
  assert.equal(v.model.acceptance_receipts,1);
  assert.equal(v.model.restored_chat_turns,0);
  assert.equal(v.model.native_work_drained_turns,12);
  assert.equal(v.model.background_completion_proven,false);
  assert.equal(v.model.embedding_backfill_failures,1);
  assert.equal(v.model.turns,12);
  assert.equal(v.model.processes,3);
  assert.equal(v.model.acceptance_attempted,true);
  assert.equal(v.rnl.python_cases_passed,182);
  assert.equal(v.rnl.helper_cases_passed,35);
  assert.equal(v.rnl.real_training_cases_excluded,1);
  assert.equal(v.rnl.original_artifacts_unchanged,1143);
  assert.equal(v.rnl.training_performed,false);
  assert.equal(v.fixes,'Submission status and bounded save-claim fixes installed; wider wording and rapid-chat retention open');
  assert.equal(v.save_claim_followup.review,'ACCEPT');
  assert.equal(v.save_claim_followup.installed,true);
  assert.equal(v.save_claim_followup.new_full_native_qualification,true);
  assert.equal(v.save_claim_followup.changed_feature_files,9);
  assert.equal(v.save_claim_followup.request_forms,2);
  assert.equal(v.save_claim_followup.acknowledgement_forms,4);
  assert.deepEqual(v.save_claim_followup.gates.map(g=>[g.passed,g.seconds]),[[2,29.89],[2,28.54]]);
  assert.equal(v.save_claim_followup.physical_audio_verified,false);
  assert.match(read('index.html'),/two separate runs/);
  assert.equal(v.submission_followup.result,'GREEN');
  assert.equal(v.submission_followup.focused_suites_passed,6);
  assert.equal(v.submission_followup.seconds,38.11);
  assert.equal(v.submission_followup.changed_source_test_files,9);
  assert.equal(v.submission_followup.review,'PASS');
  assert.equal(v.submission_followup.installed,true);
  assert.equal(v.submission_followup.new_full_native_qualification,true);
  assert.match(read('index.html'),/6\/6 focused suites passed in 38\.11 seconds/);
  assert.match(read('index.html'),/Both follow-ups are installed/);
  assert.match(read('index.html'),/79 expected HOLDs/);
  assert.match(read('index.html'),/missing staged-font warnings/);
  assert.doesNotMatch(read('index.html'),/follow-ups remain uninstalled|source-only guard|follow-up is not installed/);
  assert.equal(v.memory_observation.result,'GREEN');
  assert.equal(v.memory_observation.seconds,0.836);
  assert.equal(v.memory_observation.test_functions,9);
  assert.equal(v.memory_observation.retained_admission_proposal_saved,true);
  assert.equal(v.memory_observation.revoked_admission_save_attempted,false);
  assert.equal(v.memory_observation.safety_or_admission_changed,false);
  assert.equal(v.memory_observation.actual_model_replay,'Completed; semantic PARTIAL');
  assert.match(v.native_scope,/later Atlas-only edits/);
  assert.equal(v.video.passed,true);
  assert.equal(v.video.source_and_binary_unchanged,true);
  assert.equal(v.video.seconds,3.20);
  assert.equal(v.video.original_failed_runs_retained,2);
  assert.equal(v.video.physical_media_verified,false);
  assert.equal(v.video.test_binary_sha256,'71734117F1AB1380CF4E57AF62F1AAA344F582556C7A85425A625DE6CF2DA376');
  for(const name of ['index.html','research.html']){
    const active=read(name);
    assert.match(active,/8 November 2026/);
    assert.doesNotMatch(active,/Work is paused|Research paused|No further study is planned|Published Atlas snapshot/);
  }
  assert.match(read('index.html'),/128\/128/);
  assert.match(read('index.html'),/VideoPreview/);
  assert.match(read('index.html'),/libopenh264\.dll/);
  assert.doesNotMatch(read('index.html'),/VideoPreview still fails|VideoPreview remains failed|remaining playback failure/);
  assert.match(read('index.html'),/PARTIAL/);
  assert.match(read('index.html'),/0\.836 seconds/);
  assert.match(read('index.html'),/correction survived restart/);
  assert.match(read('index.html'),/one durable acceptance receipt and no pending proposal/);
  assert.doesNotMatch(read('index.html'),/observation-only real-model replay is pending|actual-model replay is pending/);
  assert.match(read('research.html'),/182\/182/);
});
test('work in review and settled checks say what happened and what to do next',()=>{
  const j=record(),page=read('index.html');
  assert.ok(Number.isInteger(j.in_review.pr));
  for(const x of j.in_review.items) for(const k of ['title','summary','detail','next']) assert.ok(x[k]&&!/[A-Z]{4,}_[A-Z]{4,}/.test(x[k]),'in review: '+k);
  assert.equal(page.includes('id="in-review"'),j.in_review.items.length>0);
  assert.ok(j.ruled_out.length>0);
  for(const x of j.ruled_out){
    for(const k of ['date','area','tried','result','retry']) assert.ok(x[k],'settled: '+k);
    assert.match(x.date,/^\d{4}-\d{2}-\d{2}$/);
  }
  assert.ok(page.includes('id="ruled-out"'));
  assert.equal((page.match(/<strong>Try again\?<\/strong>/g)||[]).length,j.ruled_out.length);
});
