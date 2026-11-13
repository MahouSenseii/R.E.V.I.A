import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {repo,pin,researchPin,earlierRecords,current,changes,issues,studies,publicFindings,inReview,ruledOut} from './data.mjs';
import {counts,capabilities} from './retained.mjs';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../../docs/atlas');
const publication=current.pages.verified_publication;
const short=pin.slice(0,8),ci=current.ci,two=n=>String(n).padStart(2,'0');
const count=n=>['Zero','One','Two','Three','Four','Five','Six','Seven','Eight','Nine','Ten','Eleven','Twelve'][n]??String(n);
const write=(name,text)=>fs.writeFileSync(path.join(root,name),text.replaceAll('\r\n','\n'),'utf8');
const e=s=>String(s).replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;').replaceAll('"','&quot;');
const link=(href,label,cls='')=>'<a class="'+cls+'" href="'+e(href)+'">'+label+'</a>';
const pill=(text,tone='neutral')=>'<span class="pill '+tone+'">'+e(text)+'</span>';
const detail=text=>'<details data-technical><summary>Detail &amp; evidence scope</summary><p>'+e(text)+'</p></details>';
// The Atlas tracks what was done and what happened. The full dated records and the
// earlier milestone snapshot are not copied into each update; they stay in the
// repository at this commit, and the pages link to it.
const earlier=repo+'/tree/'+earlierRecords+'/docs/atlas';

function nav(research){
  const page=research?'index.html':'';
  const item=(hash,title,num)=>'<a href="'+page+'#'+hash+'"><span class="nav-index">'+num+'</span>'+title+'</a>';
  return '<aside id="site-nav" class="sidebar" aria-label="Atlas navigation"><div class="sidebar-top">'+
    link('index.html','<span class="brand-mark" aria-hidden="true">R<span>✧</span></span><span class="brand-name">REVIA <small>ATLAS</small></span>','brand')+
    '<button class="nav-close icon-button" type="button" aria-label="Close navigation">×</button></div>'+
    '<p class="nav-caption">THE PROJECT, CLEARLY.</p><nav aria-label="Primary">'+
    item('overview','Overview','01')+item('revia','Revia progress','02')+item('systems','Capabilities','03')+
    '<a href="research.html"'+(research?' aria-current="page"':'')+'><span class="nav-index">04</span>RNL research<span class="nav-arrow">↗</span></a>'+
    item('verification','Evidence &amp; checks','05')+item('open-work','Open work','06')+item('handoff','AI handoff','07')+
    '<div class="nav-rule"></div>'+link(earlier,'<span class="nav-index">↳</span>Earlier records ↗')+
    '</nav><div class="sidebar-bottom"><span class="pause-dot"></span><strong>Update applied</strong><p>Update verified.<br>Snapshot: 8 November 2026</p>'+
    link(repo,'Source on GitHub ↗')+link('https://mahousenseii.github.io/Portfolio/','Quentin’s portfolio ↗')+'</div></aside>';
}
function head(title,research=false){
  return '<!doctype html>\n<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'+
    '<meta name="description" content="A clear view of Revia and RNL: current source, measured findings, open questions and the next steps."><meta name="theme-color" content="#fbf8f1">'+
    '<title>'+e(title)+' · Revia Atlas</title><link rel="stylesheet" href="atlas.css"><script src="atlas.js" defer></script>'+
    '<noscript><style>.mobile-bar .nav-toggle,.search-tools,.search-status,#copy-handoff{display:none}@media(max-width:800px){.sidebar{position:static;width:auto;max-width:none;transform:none;visibility:visible}.sidebar-bottom,.sidebar-top,.nav-caption{display:none}.sidebar nav{grid-template-columns:1fr 1fr}}</style></noscript></head><body>'+
    '<a class="skip-link" href="#main-content">Skip to content</a><div class="mobile-bar">'+link('index.html','REVIA <span>ATLAS</span>')+
    '<button class="nav-toggle" type="button" aria-expanded="false" aria-controls="site-nav">Menu <span aria-hidden="true">☰</span></button></div>'+
    '<button class="nav-backdrop" type="button" aria-label="Close navigation" hidden></button>'+nav(research)+
    '<main id="main-content"><div class="topline"><span>QUENTIN F. DAVIS <i>/</i> PROJECT NOTES</span><span class="snapshot">VERIFIED CHECKPOINT · 08 NOV 2026</span></div>'+
    '<div class="review-note"><span class="review-indicator" aria-hidden="true"></span> Atlas release snapshot <span class="review-note-tail">· Normal update verified, not launched.</span></div>';
}
function section(id,kicker,title,intro,body){
  return '<section class="chapter" id="'+id+'" aria-labelledby="'+id+'-title"><div class="section-heading"><div><p class="eyebrow">'+kicker+'</p><h2 id="'+id+'-title">'+title+'</h2></div></div>'+
    (intro?'<p class="section-intro">'+intro+'</p>':'')+body+'</section>';
}
function search(label){
  return '<div class="search-tools"><label class="search-label" for="site-search"><span aria-hidden="true">⌕</span><input type="search" id="site-search" placeholder="'+label+'" autocomplete="off"></label><button type="button" id="clear-search">Clear</button><label class="sr-only" for="site-search">'+label+'</label>'+
    '<button type="button" id="technical-toggle" aria-expanded="false">Show technical detail</button></div><p class="search-status" id="search-status" role="status" aria-live="polite"></p><p id="search-empty" class="empty-state" hidden>No matching records. Try a shorter phrase or clear the search.</p>';
}
function foot(){
  return '<footer class="site-footer"><div><strong>REVIA ATLAS</strong><span>Built by Quentin F. Davis. Progress with its evidence attached.</span></div><div>'+
    link('milestone.json','Structured snapshot ↓')+link(repo+'/tree/'+pin+'/docs','Source documentation ↗')+'</div></footer></main></body></html>\n';
}
function changeRows(){
  return changes.map(c=>'<article class="update searchable" data-search="'+e(['Revia',c.pr,c.title,c.summary,c.detail,c.next].join(' '))+'">'+
    '<div class="update-ref">'+pill('Merged','positive')+link(repo+'/pull/'+c.pr,'PR #'+c.pr)+'</div><div class="update-body"><h3>'+e(c.title)+'</h3><p>'+e(c.summary)+'</p>'+
    detail(c.detail)+'<p class="next-line"><strong>Next</strong> '+e(c.next)+'</p></div></article>').join('');
}
function capabilityRows(){
  const groups=new Map();
  for(const c of capabilities){if(!groups.has(c.group))groups.set(c.group,[]);groups.get(c.group).push(c);}
  return '<div class="capability-groups">'+[...groups.entries()].map(([group,caps])=>'<details class="capability-group" data-search-group><summary><span>'+e(group)+'</span><span class="group-count">'+caps.length+' areas <i>+</i></span></summary><div>'+
    caps.map(c=>{
      const blocked=c.verification.state==='blocked';
      const summary=c.id==='CAP-014'?'Delegation and continued chat were observed, but a useful accepted real-model report is still unverified.':c.verification.summary;
      return '<article class="capability searchable" data-search="'+e([c.name,c.purpose,c.implementation.summary,summary,c.next_step].join(' '))+'"><div><h3>'+e(c.name)+'</h3><p>'+e(c.purpose)+'</p></div><div>'+pill(blocked?'Runtime blocked':c.implementation.state==='partial'?'Partial implementation':'Present in source',blocked?'negative':'neutral')+
        '<p>'+e(summary)+'</p>'+detail(c.implementation.summary+' Next: '+c.next_step)+'</div></article>';
    }).join('')+'</div></details>').join('')+'</div>';
}
function reviewRows(){
  return inReview.items.map(c=>'<article class="update searchable" data-search="'+e(['Revia',inReview.pr,c.title,c.summary,c.detail,c.next].join(' '))+'">'+
    '<div class="update-ref">'+pill('In review')+link(repo+'/pull/'+inReview.pr,'PR #'+inReview.pr)+'</div><div class="update-body"><h3>'+e(c.title)+'</h3><p>'+e(c.summary)+'</p>'+
    detail(c.detail)+'<p class="next-line"><strong>Next</strong> '+e(c.next)+'</p></div></article>').join('');
}
function ruledOutRows(){
  return '<div class="issue-list">'+ruledOut.map((x,i)=>'<article class="issue searchable" data-search="'+e([x.area,x.tried,x.result,x.retry].join(' '))+'"><span class="issue-number">'+two(i+1)+'</span><div><p class="small-label">'+e(x.area)+' <span>· '+e(x.date)+'</span></p><h3>'+e(x.tried)+'</h3><p>'+e(x.result)+'</p><p class="next-line"><strong>Try again?</strong> '+e(x.retry)+'</p></div></article>').join('')+'</div>';
}
function issueRows(){
  return '<div class="issue-list">'+issues.map((x,i)=>'<article class="issue searchable" data-search="'+e([x.area,x.title,x.summary,x.detail,x.next].join(' '))+'"><span class="issue-number">'+two(i+1)+'</span><div><p class="small-label">'+e(x.area)+' <span>· '+e(x.status)+'</span></p><h3>'+e(x.title)+'</h3><p>'+e(x.summary)+'</p>'+detail(x.detail)+'<p class="next-line"><strong>Next</strong> '+e(x.next)+'</p></div></article>').join('')+'</div>';
}
const sourceDetails='<dl class="evidence-dl"><div><dt>Retained main revision</dt><dd>'+link(repo+'/commit/'+pin,short)+' · merged PR #'+current.source_pr+'</dd></div><div><dt>Earlier source CI</dt><dd>'+link(repo+'/actions/runs/'+ci.run,'CI run '+ci.run)+' · six successful jobs</dd></div><div><dt>That CI result</dt><dd>'+ci.passed+' passed · '+ci.failed+' failed · '+ci.optional_skips+' optional video skips</dd></div><div><dt>Installed app then</dt><dd>Unchanged at that earlier checkpoint</dd></div><div><dt>RNL integration</dt><dd>None · no candidate selected</dd></div></dl>';
let index=head('Project overview')+
  '<section class="hero" id="overview" aria-labelledby="page-title"><div class="hero-copy"><p class="eyebrow"><span class="gold-diamond"></span> A LOCAL-FIRST COMPANION, IN PROGRESS</p><h1 id="page-title">Revia,<br><em>in focus.</em></h1><p class="hero-lead">What changed. What we learned.<br>What still needs to work.</p><p class="hero-description">One readable place for Revia’s engineering progress and RNL’s research — with the findings, limits and next steps kept together.</p><div class="hero-actions">'+link('#revia','Explore the progress <span aria-hidden="true">↓</span>','button primary')+link('research.html','Read the research ↗','text-link')+'</div></div>'+
  '<div class="hero-aside"><div class="atlas-orbit" aria-hidden="true"><span class="orbit-small">REACTIVE · EMOTIONAL · VIRTUAL</span><span class="orbit-r">R</span><span class="orbit-star">✧</span><span class="orbit-bottom">INTERACTIVE ASSISTANT</span></div><p>SOURCE CHANGES. RESEARCH FINDINGS.<br>A CLEAR RECORD OF BOTH.</p></div></section>'+
  '<div class="status-strip"><div><span class="stat-value">128/128</span><span class="stat-label">Fresh native passes</span></div><div><span class="stat-value">12</span><span class="stat-label">Model turns: partial</span></div><div><span class="stat-value">182</span><span class="stat-label">RNL Python passes</span></div><div class="stat-context"><span class="small-label">LOCAL CANDIDATE CHECK</span><span>8 November 2026</span><span>Separate scopes, not a combined total</span></div></div>'+
  '<div class="workstream-grid"><article class="workstream"><p class="eyebrow">01 / ENGINEERING</p><div class="workstream-heading"><h2>Revia</h2>'+pill('Update verified')+'</div><p class="workstream-lead">A narrow save claim is qualified before delivery.</p><p>Both memory-status fixes are installed and independently verified. Native status reports submission, not saving; a finite guard qualifies two correction-request forms and four acknowledgements. Wider wording and rapid-message retention remain open.</p>'+link('#local-validation','Read the current checks','card-link')+'</article>'+
  '<article class="workstream research-stream"><p class="eyebrow">02 / RESEARCH</p><div class="workstream-heading"><h2>RNL</h2>'+pill('Regression complete','positive')+'</div><p class="workstream-lead">Correctness evidence, not a quality breakthrough.</p><p>182 Python cases passed and original research artifacts are unchanged. Two bounded-read helper defects were repaired. Further improvements remain separate from training or app integration.</p>'+link('research.html','Read results and limits','card-link')+'</article></div>'+
  '<div class="reading-note"><span aria-hidden="true">↳</span><p><strong>How to read this Atlas</strong> “Merged” describes source code. “Measured” applies to a named test. Neither means the installed application is qualified. This is a manually checked snapshot.</p></div>'+
  section('local-validation','8 NOVEMBER 2026 / VERIFIED UPDATE','What passed, and what still fails.','Both fixes passed the fresh permitted native batch and exact installation checks. Earlier model results remain separate; neither proves general reply quality, physical audio or unrestricted release readiness.',
    '<div class="evidence-grid"><article class="evidence-panel"><h3>Fresh native run: 128/128 passed</h3><p>Zero failed, skipped, incomplete or timed-out registrations. All 1,854 pins remained unchanged before installation. The 124-target Qt13 Debug build uses -O0 -g0 with assertions. Wrapper time: 504.00 seconds. An interrupted prior run had 83 passes and case 84 unknown; it was not combined with this run.</p>'+detail(current.validation.native_limit+' '+current.validation.exclusions+' Earlier source history remains: 122/128 initially, five short-path recoveries, and VideoPreview passing after missing libopenh264.dll staging. Earlier interpreted and RNL results are separate, not extra fresh registrations. Later Atlas-only edits do not change the application checkpoint.')+'</article>'+
    '<article class="evidence-panel"><h3>Earlier acceptance PASS; replies PARTIAL</h3><p>Twelve real synthetic turns. The approved correction survived restart with one durable acceptance receipt and no pending proposal. Old/new records remain; restored chat turns: zero. The save claim preceded confirmation by 2.572 seconds. A pronoun mix-up, typo, poor introduction and two guesses instead of one remain.</p>'+detail('One exact acceptance; no acceptance retry. A preserved interruption preceded continuation, so this was not an uninterrupted replay. All three canonical facts have embeddings; a historical archive failure recovered at dataset level, with only the newest reply tail pending in each phase. Broader background completion and general semantic grounding are unproven. No training, real-user capture, cloud or physical-audio verification.')+'</article></div>')+
  section('submission-followup','8 NOVEMBER 2026 / INSTALLED FOLLOW-UPS','Bounded acknowledgements, not a save guarantee.','Both follow-ups are installed after fresh permitted 128-test qualification and independent post-install review.',
    '<div class="evidence-panel"><p>The save-claim guard qualifies two explicit request forms and four acknowledgement forms with an uncertainty statement. Attributed-private scope is required; invoked tools are exempt. Safety, JSON, quoted text and output bounds remain intact.</p><p>Four focused registrations passed in two separate runs: OutputBoundaries + StructuredReply, 2/2 in 29.89 seconds; ContextFitting + QualityFeedback, 2/2 in 28.54 seconds. Final integration review accepted the nine-file feature with no Critical or Important findings.</p>'+detail('Tests cover actual r2 delivery/history/observer/raw/display identity, queued speech, 24 revocation cells, eight tool/fallback controls and raw-feedback-digest refusal. Queued speech is not physical playback. Unsupported wording, implicit-prior/fiction ambiguity and rapid-message retention remain; an optional harmless-prefix tool-barrier test can be strengthened. No model-quality or training claim.')+'<p>Earlier native submission follow-up: 6/6 focused suites passed in 38.11 seconds. It is now installed. Its false-queued regression became green; two test observers migrated to Submitted with a real positive control and prior assertions preserved. Native status reports handoff, not a durable save.</p></div>')+
  section('memory-observation','7 NOVEMBER 2026 / FOCUSED FOLLOW-UP','Completion tracking and a narrow candidate guard passed.','Observation check: 0.836 seconds. The candidate guard passed four new rejections, 13 controls and nine existing functions, then the fresh native batch.',
    '<div class="evidence-panel"><p>A participant memory candidate is rejected before save when its value is absent from raw current input. Observation checks distinguish active work from an idle queue and retain correct behavior when admission is revoked.</p>'+detail('Narrow containment only. Quoted/context-only inputs and rapid-chat admission remain limitations; no general grounding or truthful-save guarantee. The new updater passed 80 synthetic controls: one success and 79 expected HOLDs, with 20,137 independent checks. Separately, two current/legacy offscreen UI loaders passed with nonfatal missing staged-font warnings. They prove construction/loading and clean exit, not parser acceptance or physical UI/audio. The two-EXE update completed at 02:14:56 UTC on 8 November; independent postcheck passed. Backups and displaced originals remain; helpers, runtime files and shortcut are unchanged. The normal app was not launched. Config/memory were not inspected or written by the fixed program-only update.')+'</div>')+
  search('Find a change, capability or open issue')+
  (inReview.items.length?section('in-review','IN REVIEW / '+e(inReview.as_of),'Done and measured, not merged yet.',count(inReview.items.length)+' changes are on PR #'+inReview.pr+'. They are not in the retained main revision or the installed app, and their measurements are small live samples.',reviewRows()):'')+
  section('revia','01 / REVIA PROGRESS','Earlier merged changes, plainly.',count(changes.length)+' merged fixes are retained through PR #'+current.source_pr+'. Each record keeps the source change separate from what still needs checking.',changeRows()+
    '<p class="section-end">'+link(earlier,'Earlier repairs, tooling and retained failures ↗')+'</p>')+
  section('systems','02 / CAPABILITIES','What Revia is built to do.','The source contains 20 capability areas. These describe the implementation; live use and release readiness have their own evidence.',capabilityRows())+
  section('verification','03 / EVIDENCE &amp; CHECKS','A pass needs a scope.','The earlier pinned main CI is retained below. Current qualification and the verified normal update are above.', '<div class="evidence-grid"><article class="evidence-panel"><h3>Earlier main checkpoint</h3>'+sourceDetails+'</article>'+
    '<article class="evidence-panel deployment-panel"><p class="eyebrow">WEBSITE PUBLICATION</p><h3>Earlier website<br>publication receipt.</h3><p>Historical publication checkpoint: 5 November 2026. The reviewed Atlas from PR #35 was published successfully, and all six public files matched the reviewed source.</p>'+link(publication.url,'Open live Atlas ↗')+
    '<p class="muted">Publication evidence: '+link(repo+'/actions/runs/'+publication.run,'verified Pages run')+'. Current deployment: '+link(repo+'/actions/workflows/atlas-pages.yml','Pages workflow')+'. Website and app updates are separate.</p>'+
    '<details data-technical><summary>Earlier deployment failure (4 November 2026)</summary><p>The earlier Pages run had two deployment attempts that stalled and were cancelled at the ten-minute timeout. Its underlying backend cause remains unverified. The successful 5 November publication superseded that earlier failure. No new live-site check is claimed here.</p>'+
    link(repo+'/actions/runs/'+current.pages.previous_failure.run,'Earlier cancelled run ↗')+'<p class="muted">The '+link(repo+'/actions/runs/'+current.pages.previous_failure.preceding_successful_run,'preceding historical run')+' also succeeded.</p></details></article></div>'+
    '<details data-technical><summary>Metric definitions and historical counts</summary><p>The '+ci.passed+' pass / '+ci.optional_skips+' skip result belongs to the source-pinned CI run above. It is not a new local test run, a capability score or a performance measurement. The older 153-registration controlled checkpoint is kept as the counts in the JSON snapshot and in full in the earlier records. Test discovery is not test completion.</p></details>')+
  section('open-work','04 / OPEN WORK','The unfinished work stays visible.','The update is independently verified, not launched. Reply-quality and physical-workflow limits remain visible.',issueRows())+
  section('ruled-out','TRIED AND SETTLED','Read this before checking something again.','Each entry is something that was tried and failed, or was decided against, with what would make it worth another look.',ruledOutRows())+
  section('handoff','05 / PICK UP THE THREAD','A clear start for the next AI.','Begin with the verified state, then follow the evidence before running anything.', '<div class="handoff-panel"><div><p class="eyebrow">START HERE</p><h3>Understand first.<br>Continue deliberately.</h3><p>Read the handoff, check the current GitHub head against the pinned revision, and inspect the relevant source. Preserve research artifacts and saved responses; distinguish old studies from current regression checks.</p>'+
    link(repo+'/blob/'+pin+'/README.md','Retained repository README','text-link')+'</div><ol class="handoff-steps"><li><span>01</span><div><strong>Current local candidate</strong><p>Base c5ef5599 plus uncommitted changes. Fresh native 128/128; exact acceptance/restart passed; replies partial. Normal update independently verified, not launched.</p></div></li><li><span>02</span><div><strong>Research boundary</strong><p>182 Python cases passed, plus separately scoped audits and helpers. No training or normal-app integration is established by these checks.</p></div></li><li><span>03</span><div><strong>Next useful check</strong><p>Keep verified update receipts and reply-quality limits visible; retain the exact scope of each result.</p></div></li></ol></div>'+
    '<div class="handoff-links">'+link('milestone.json','Read the structured snapshot ↓')+link('research.html#research-handoff','RNL handoff &amp; evidence boundary →')+
    '<button type="button" id="copy-handoff">Copy handoff summary</button><span id="copy-status" role="status"></span></div>'+
    '<pre id="handoff-text" class="handoff-text">REVIA ATLAS - 2026-11-08\nAtlas release snapshot. Normal two-EXE update completed at 02:14:56 UTC; independent postcheck passed; backups/displaced originals retained. Not launched. Helpers/runtime/shortcut unchanged; config/memory preserved by fixed write scope, not inspected.\nCandidate c5ef5599 plus uncommitted fixes, distinct from retained main '+pin+'. Fresh permitted native run: 128/128, zero failed/skipped/incomplete/timeouts; 1,854 pins unchanged before install. Qt13 Debug -O0 -g0, assertions. Excludes later Atlas-only edits. 31 of 159 registrations excluded; FoundationWithoutMicrophone omits 16 microphone functions. NumPy benchmark absent, not passed/skipped. Prior interrupted run: 83 passes, case 84 unknown, not aggregated. Earlier 122/128 failures/setup recoveries retained.\nInstalled: truthful submission status and finite saved-now qualification, two request/four acknowledgement forms. Earlier focused gates: 6/6 in 38.11s and separate 2/2 in 29.89s, 2/2 in 28.54s. No blanket honesty guarantee; wider wording, fiction/prior ambiguity and rapid-message retention remain.\nUpdater: 80 synthetic controls (1 success, 79 expected HOLDs), 20,137 checks. Separate UI loads 2/2; nonfatal staged-font warnings. No physical audio or normal runtime proof.\nEarlier synthetic r2: 12 turns; exact acceptance/restart PASS, replies PARTIAL. One receipt/no pending proposal; old/new rows retained, no restored chat. Preserved stop before continuation; no acceptance retry. Save claim 2.572s early and pronoun/typo/introduction/guess defects retained as historical findings. Three canonical facts embedded; newest reply tails pending, no complete-background guarantee. No new model run or training.\nRNL earlier 182 Python cases plus 35 helpers/separate audits, no model promotion. Earlier publication receipt: 5 November. Next: safe rapid-message work.</pre>')+
  foot();

function studyCards(){
  return studies.map(s=>'<article class="study searchable" id="'+s.id+'-current" data-search="'+e([s.id,s.tag,s.name,s.summary,s.detail,s.next].join(' '))+'"><div class="study-meta"><span>'+e(s.id.toUpperCase())+'</span>'+pill(s.tag,s.tone)+'</div><h3>'+e(s.name)+'</h3><p class="study-summary">'+e(s.summary)+'</p>'+detail(s.detail)+'<p class="study-limit">'+e(s.limit)+'</p><p class="next-line"><strong>Next</strong> '+e(s.next)+'</p></article>').join('');
}
let research=head('RNL research',true)+
  '<section class="research-hero" id="research-overview"><p class="eyebrow"><span class="gold-diamond"></span> THE RESEARCH NOTEBOOK</p><h1>RNL, <em>with context.</em></h1><p class="hero-lead">Promising ideas.<br>Evidence before integration.</p><p class="hero-description">A research track exploring recurrent neural memory and exact reuse. Positive results, failed approaches and open questions belong in the same record.</p>'+
  '<div class="hero-actions">'+link('#latest-findings','Read the latest findings ↓','button primary')+link('index.html','Back to Revia →','text-link')+'</div></section>'+
  '<div class="research-stages"><div><span class="stage-index">01</span><strong>Experimental</strong><p>Local improvements in progress</p></div><div><span class="stage-index">02</span><strong>Measured</strong><p>Named synthetic studies</p></div><div><span class="stage-index">03</span><strong>Candidate for Revia</strong><p>None</p></div><div><span class="stage-index">04</span><strong>Integrated</strong><p>None</p></div></div>'+
  section('rnl-validation','7 NOVEMBER 2026 / LOCAL REGRESSION','182/182 Python cases passed.','This verifies the tested research software. It does not establish better language-model quality or a promoted RNL model.',
    '<div class="evidence-grid"><article class="evidence-panel"><h3>Independent scopes</h3><p>165 unit/preparation cases and 17 trace-audit cases passed. Separately: three audits, 512 arithmetic checks, 1,440 graph matches, LEARN026 with 11 controls, and a LEARN027 baseline failure plus 23 patched controls. Do not sum these overlapping checks into a single total.</p></article><article class="evidence-panel"><h3>Repairs and limits</h3><p>Two bounded-read helper defects were fixed in four candidate Python/helper-test files. The 35 helper cases passed; one real-training test was deliberately excluded. The regression closeout found all 1,143 original artifacts unchanged. The history-export patch is isolated from normal Revia.</p>'+detail('No weights were trained, no actual-user data captured and no model generation ran in this regression. FIT021 remains incomplete; FIT022/FIT023 acceptance gates remain false. The old sealed test was not opened. Detailed receipts and source hashes remain local, not embedded here.')+'</article></div>')+
  '<div class="comparison-notice" id="comparison-status"><div>'+pill('Earlier study closeout','neutral')+'<h2>RNL tied a lookup table,<br>then fell short of a good store.</h2></div><div><p>COMPARE-001 finished: RNL and exact lookup answered 32/32, Qwen 0.8B 17/32. STATE-017 then trained RNL against a compact keyed store on harder synthetic memory: on development data it stayed 7–32 points below at every memory size, and a learned name matcher in front of a store had at most about 2.5 points of room.</p><p>These are development results for the designs tried, not proof that RNL cannot work. The sealed test set was never opened. Candidate for Revia: none.</p>'+
  detail('COMPARE-001: all 46 outputs sealed before grading; the original 34 LLM files were reused unchanged. Its first stop was traced to a disk census inside the log-reading thread. STATE-017: 63 trained models, three seeds each, CPU only, about 6 CPU-hours per run; rules frozen before run 2. No upload is active.')+'</div></div>'+
  search('Find a study, result or limitation')+
  section('latest-findings','01 / LATEST LOCAL STUDIES','What the newer studies found.','Positive, negative and stopping results are shown together. Detailed methods, frozen artifacts, checkpoints and traces for STATE-011–019 remain local and unpublished.', '<div class="study-grid">'+studyCards()+'</div>')+
  section('published-findings','02 / EARLIER WORK','What the public source supports.','The pinned public research branch contains methods and results through STATE-010. Atlas summaries of later studies do not make their full evidence public.',
    '<div class="public-findings">'+publicFindings.map(x=>'<article class="finding searchable" data-search="'+e([x.name,x.summary,x.detail].join(' '))+'"><h3>'+e(x.name)+'</h3><p>'+e(x.summary)+'</p>'+detail(x.detail)+'</article>').join('')+'</div><p class="section-end">'+link(repo+'/tree/'+researchPin+'/research/rnl','Pinned public research source ↗')+'</p>')+
  section('research-handoff','03 / REPRODUCIBILITY &amp; NEXT STEPS','Know which evidence you have.','A readable summary is a starting point. Reproducing the latest studies also needs their exact artifacts.', '<div class="boundary-grid"><article><p class="eyebrow">PUBLIC</p><h3>Code through STATE-010</h3><p>Use the pinned branch revision and its method, lock and config references. A moving branch head is not the same source identity.</p>'+
    link(repo+'/tree/'+researchPin+'/research/rnl','aac6f161 · research/rnl ↗')+'</article><article><p class="eyebrow">LOCAL · UNPUBLISHED</p><h3>Studies 011–019 and the comparison</h3><p>Source deltas, per-example predictions, traces, frozen snapshots, the 63 STATE-017 checkpoints and the full comparison seal are held locally. This website does not supply them.</p></article></div>'+
    '<details data-technical><summary>What a reproducible, reviewed handoff needs</summary><ul><li>Exact method/config/source/test deltas and all positive and negative results.</li><li>Fixture generators, seeds, evaluation definitions and per-example predictions/traces.</li><li>Three frozen delta snapshots, tensor inventory, original hashes and lineage.</li><li>The comparison fixture/parser, 32 retained requests/responses and completed 46-output seal.</li><li>An inventory mapping symbolic artifact IDs to sanitized filenames and export hashes.</li></ul><p>Raw local logs/manifests can contain private paths and process metadata. The exact export inventory and destination need review before any upload. A summary alone does not reproduce these studies.</p></details>'+
    '<div class="next-research"><span class="gold-diamond"></span><div><h3>Improve incrementally; keep the evidence bounded.</h3><p>The earlier STATE-017 closeout remains valid for those designs. Current regression checks and scoped improvements do not erase its negative findings. No new training, sealed-test evaluation, normal-app integration or publication is claimed by this checkpoint.</p></div></div>')+
  foot();

const record={
  schema_version:4,
  milestone:'Submission and bounded save-claim fixes installed; fresh permitted 128/128 and independent postcheck passed; not launched',
  recorded_utc:current.as_of,
  refresh_mode:current.refresh_mode,
  repository:'MahouSenseii/R.E.V.I.A',
  current,
  source:{main_commit:pin,installed_app_updated:false,release_qualified:false},
  product_delivery:'Merged source on GitHub main; installed app unchanged; no application deployment',
  integration:{main_commit:pin,installed_app_updated:false,runtime_release_qualified:false},
  product_delivery_scope:'The source/integration/product_delivery fields retain the earlier main checkpoint only. Current normal-update status is current.validation.normal_update; website deployment is tracked separately.',
  counts,
  counts_scope:'Historical 153-registration controlled checkpoint ONLY. Retained main CI is current.ci; new local checks are current.validation. These scopes are not additive; discovery is not a pass count.',
  changes,
  research_findings:studies,
  open_work:issues,
  in_review:inReview,
  ruled_out:ruledOut,
  earlier_records:{commit:earlierRecords,url:earlier,scope:'The full dated records and the complete 2026-11-03 milestone snapshot were published up to this commit and remain in the repository history. They are not repeated in this file.'}
};
write('index.html',index);
write('research.html',research);
// The current block is written on one line; the rest is indented for reading.
write('milestone.json','{\n'+Object.entries(record).map(([key,value])=>'  '+JSON.stringify(key)+': '+
  (key==='current'?JSON.stringify(value):JSON.stringify(value,null,2).replace(/\n/g,'\n  '))).join(',\n')+'\n}\n');
for(const [from,to] of [['styles.css','atlas.css'],['records.css','public.css'],['interactions.js','atlas.js']])
  fs.copyFileSync(path.join(path.dirname(fileURLToPath(import.meta.url)),from),path.join(root,to));
console.log('Built two static pages and the milestone record.');
