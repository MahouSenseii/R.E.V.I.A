import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {repo,pin,researchPin,current,changes,issues,studies,publicFindings} from './data.mjs';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../../docs/atlas');
const publication=current.pages.verified_publication;
const short=pin.slice(0,8),ci=current.ci,two=n=>String(n).padStart(2,'0');
const read=name=>fs.readFileSync(path.join(root,name),'utf8');
const write=(name,text)=>fs.writeFileSync(path.join(root,name),text.replaceAll('\r\n','\n'),'utf8');
const e=s=>String(s).replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;').replaceAll('"','&quot;');
const link=(href,label,cls='')=>'<a class="'+cls+'" href="'+e(href)+'">'+label+'</a>';
const pill=(text,tone='neutral')=>'<span class="pill '+tone+'">'+e(text)+'</span>';
const detail=text=>'<details data-technical><summary>Detail &amp; evidence scope</summary><p>'+e(text)+'</p></details>';
const marker='<!-- BEGIN RETAINED RECORD -->',endMarker='<!-- END RETAINED RECORD -->';
function retained(name){
  const html=read(name);
  if(html.includes(marker))return html.split(marker)[1].split(endMarker)[0];
  const main=html.slice(html.indexOf('>',html.indexOf('<main'))+1,html.lastIndexOf('</main>'));
  return main.replace(/<header>[\s\S]*?<\/header>/,'').replace(/<footer>[\s\S]*?<\/footer>/,'');
}
const oldIndex=retained('index.html'),oldResearch=retained('research.html');
const original=JSON.parse(read('milestone.json'));
const history=original.history??original;

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
    '<div class="nav-rule"></div>'+link(research?'#research-records':'#dated-records','<span class="nav-index">↳</span>Dated records')+
    '</nav><div class="sidebar-bottom"><span class="pause-dot"></span><strong>Work is paused</strong><p>A safe stopping point.<br>Snapshot: 05 November 2026</p>'+
    link(repo,'Source on GitHub ↗')+link('https://mahousenseii.github.io/Portfolio/','Quentin’s portfolio ↗')+'</div></aside>';
}
function head(title,research=false){
  return '<!doctype html>\n<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'+
    '<meta name="description" content="A clear view of Revia and RNL: current source, measured findings, open questions and the next steps."><meta name="theme-color" content="#fbf8f1">'+
    '<title>'+e(title)+' · Revia Atlas</title><link rel="stylesheet" href="atlas.css"><link rel="stylesheet" href="public.css"><script src="atlas.js" defer></script>'+
    '<noscript><style>.mobile-bar .nav-toggle,.search-tools,.search-status,#copy-handoff{display:none}@media(max-width:800px){.sidebar{position:static;width:auto;max-width:none;transform:none;visibility:visible}.sidebar-bottom,.sidebar-top,.nav-caption{display:none}.sidebar nav{grid-template-columns:1fr 1fr}}</style></noscript></head><body>'+
    '<a class="skip-link" href="#main-content">Skip to content</a><div class="mobile-bar">'+link('index.html','REVIA <span>ATLAS</span>')+
    '<button class="nav-toggle" type="button" aria-expanded="false" aria-controls="site-nav">Menu <span aria-hidden="true">☰</span></button></div>'+
    '<button class="nav-backdrop" type="button" aria-label="Close navigation" hidden></button>'+nav(research)+
    '<main id="main-content"><div class="topline"><span>QUENTIN F. DAVIS <i>/</i> PROJECT NOTES</span><span class="snapshot">SOURCE-PINNED · 05 NOV 2026</span></div>'+
    '<div class="review-note"><span class="review-indicator" aria-hidden="true"></span> Published Atlas snapshot <span class="review-note-tail">· Website live; installed-app status remains separate.</span></div>';
}
function section(id,kicker,title,intro,body){
  return '<section class="chapter" id="'+id+'" aria-labelledby="'+id+'-title"><div class="section-heading"><div><p class="eyebrow">'+kicker+'</p><h2 id="'+id+'-title">'+title+'</h2></div></div>'+
    (intro?'<p class="section-intro">'+intro+'</p>':'')+body+'</section>';
}
function search(label){
  return '<div class="search-tools"><label class="search-label" for="site-search"><span aria-hidden="true">⌕</span><input type="search" id="site-search" placeholder="'+label+'" autocomplete="off"></label><button type="button" id="clear-search">Clear</button><label class="sr-only" for="site-search">'+label+'</label>'+
    '<button type="button" id="technical-toggle" aria-expanded="false">Show technical detail</button></div><p class="search-status" id="search-status" role="status" aria-live="polite"></p><p id="search-empty" class="empty-state" hidden>No matching records. Try a shorter phrase or clear the search.</p>';
}
function archive(id,html,when){
  return '<section class="chapter records" id="'+id+'"><details class="record-drawer"><summary><span><span class="eyebrow">THE FULL RECORD</span><strong>Dated records</strong><span class="record-description">Original findings, methods, failures and evidence.</span></span><span class="record-plus" aria-hidden="true">+</span></summary>'+
    '<div class="archive-notice"><strong>Historical snapshot · '+when+'</strong><p>These records preserve their original wording and evidence limits. Their “current”, “next” and “unrun” statements describe that earlier snapshot. The dated summary above is the current starting point.</p></div>'+
    '<div class="legacy">'+marker+html+endMarker+'</div></details></section>';
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
  for(const c of history.capabilities){if(!groups.has(c.group))groups.set(c.group,[]);groups.get(c.group).push(c);}
  return '<div class="capability-groups">'+[...groups.entries()].map(([group,caps])=>'<details class="capability-group" data-search-group><summary><span>'+e(group)+'</span><span class="group-count">'+caps.length+' areas <i>+</i></span></summary><div>'+
    caps.map(c=>{
      const blocked=c.verification.state==='blocked';
      const summary=c.id==='CAP-014'?'Delegation and continued chat were observed, but a useful accepted real-model report is still unverified.':c.verification.summary;
      return '<article class="capability searchable" data-search="'+e([c.name,c.purpose,c.implementation.summary,summary,c.next_step].join(' '))+'"><div><h3>'+e(c.name)+'</h3><p>'+e(c.purpose)+'</p></div><div>'+pill(blocked?'Runtime blocked':c.implementation.state==='partial'?'Partial implementation':'Present in source',blocked?'negative':'neutral')+
        '<p>'+e(summary)+'</p>'+detail(c.implementation.summary+' Next: '+c.next_step)+'</div></article>';
    }).join('')+'</div></details>').join('')+'</div>';
}
function issueRows(){
  return '<div class="issue-list">'+issues.map((x,i)=>'<article class="issue searchable" data-search="'+e([x.area,x.title,x.summary,x.detail,x.next].join(' '))+'"><span class="issue-number">0'+(i+1)+'</span><div><p class="small-label">'+e(x.area)+' <span>· '+e(x.status)+'</span></p><h3>'+e(x.title)+'</h3><p>'+e(x.summary)+'</p>'+detail(x.detail)+'<p class="next-line"><strong>Next</strong> '+e(x.next)+'</p></div></article>').join('')+'</div>';
}
const sourceDetails='<dl class="evidence-dl"><div><dt>Source revision</dt><dd>'+link(repo+'/commit/'+pin,short)+' · merged PR #'+current.source_pr+'</dd></div><div><dt>Application check</dt><dd>'+link(repo+'/actions/runs/'+ci.run,'CI run '+ci.run)+' · six successful jobs</dd></div><div><dt>Test outcomes</dt><dd>'+ci.passed+' passed · '+ci.failed+' failed · '+ci.optional_skips+' optional video skips</dd></div><div><dt>Installed app</dt><dd>Not updated or freshly audited here</dd></div><div><dt>RNL integration</dt><dd>None · no candidate selected</dd></div></dl>';
let index=head('Project overview')+
  '<section class="hero" id="overview" aria-labelledby="page-title"><div class="hero-copy"><p class="eyebrow"><span class="gold-diamond"></span> A LOCAL-FIRST COMPANION, IN PROGRESS</p><h1 id="page-title">Revia,<br><em>in focus.</em></h1><p class="hero-lead">What changed. What we learned.<br>What still needs to work.</p><p class="hero-description">One readable place for Revia’s engineering progress and RNL’s research — with the findings, limits and next steps kept together.</p><div class="hero-actions">'+link('#revia','Explore the progress <span aria-hidden="true">↓</span>','button primary')+link('research.html','Read the research ↗','text-link')+'</div></div>'+
  '<div class="hero-aside"><div class="atlas-orbit" aria-hidden="true"><span class="orbit-small">REACTIVE · EMOTIONAL · VIRTUAL</span><span class="orbit-r">R</span><span class="orbit-star">✧</span><span class="orbit-bottom">INTERACTIVE ASSISTANT</span></div><p>SOURCE CHANGES. RESEARCH FINDINGS.<br>A CLEAR RECORD OF BOTH.</p></div></section>'+
  '<div class="status-strip"><div><span class="stat-value">'+ci.passed+'</span><span class="stat-label">Tests passed</span></div><div><span class="stat-value">'+two(ci.optional_skips)+'</span><span class="stat-label">Optional video skips</span></div><div><span class="stat-value">'+two(ci.jobs_passed)+'</span><span class="stat-label">CI jobs passed</span></div><div class="stat-context"><span class="small-label">LATEST VERIFIED SOURCE</span>'+link(repo+'/commit/'+pin,short+' ↗')+'<span>PR #'+current.source_pr+' · 05 November</span></div></div>'+
  '<div class="workstream-grid"><article class="workstream"><p class="eyebrow">01 / ENGINEERING</p><div class="workstream-heading"><h2>Revia</h2>'+pill('Source verified','positive')+'</div><p class="workstream-lead">Small fixes. Clearer behavior.</p><p>Identity and conversation corrections are merged. Two invented-history cases now have a scoped real-model check; general grounding, useful delegated reports and installed-app qualification remain open.</p>'+link('#revia','See what changed <span>→</span>','card-link')+'</article>'+
  '<article class="workstream research-stream"><p class="eyebrow">02 / RESEARCH</p><div class="workstream-heading"><h2>RNL</h2>'+pill('Research paused')+'</div><p class="workstream-lead">A useful finding. An unfinished comparison.</p><p>Separate memory slots solved the small synthetic test. The LLM comparison stopped after saving 32 replies; no model ranking exists.</p>'+link('research.html','Explore the findings <span>→</span>','card-link')+'</article></div>'+
  '<div class="reading-note"><span aria-hidden="true">↳</span><p><strong>How to read this Atlas</strong> “Merged” describes source code. “Measured” applies to a named test. Neither means the installed application is qualified. This is a manually checked snapshot.</p></div>'+
  search('Find a change, capability or open issue')+
  section('revia','01 / REVIA PROGRESS','The latest changes, plainly.','Eight merged fixes bring the overview up to PR #'+current.source_pr+'. Each record keeps the source change separate from what still needs checking.',changeRows()+
    '<p class="section-end">'+link('#dated-records','Earlier repairs, tooling and retained failures ↓')+'</p>')+
  section('systems','02 / CAPABILITIES','What Revia is built to do.','The source contains 20 capability areas. These describe the implementation; live use and release readiness have their own evidence.',capabilityRows())+
  section('verification','03 / EVIDENCE &amp; CHECKS','A pass needs a scope.','The pinned application-source CI is green. Product qualification and website publication are separate.', '<div class="evidence-grid"><article class="evidence-panel"><h3>Application source</h3>'+sourceDetails+'</article>'+
    '<article class="evidence-panel deployment-panel"><p class="eyebrow">WEBSITE PUBLICATION</p><h3>Atlas is live<br>on GitHub Pages.</h3><p>Verified publication checkpoint: 5 November 2026. The reviewed Atlas from PR #35 was published successfully, and all six public files matched the reviewed source.</p>'+link(publication.url,'Open live Atlas ↗')+
    '<p class="muted">Publication evidence: '+link(repo+'/actions/runs/'+publication.run,'verified Pages run')+'. Publishing this website does not update the installed Revia app.</p>'+
    '<details data-technical><summary>Earlier deployment failure (4 November 2026)</summary><p>The earlier Pages run had two deployment attempts that stalled and were cancelled at the ten-minute timeout. Its underlying backend cause remains unverified. The successful 5 November publication supersedes that failure as the current website status.</p>'+
    link(repo+'/actions/runs/'+current.pages.previous_failure.run,'Earlier cancelled run ↗')+'<p class="muted">The '+link(repo+'/actions/runs/'+current.pages.previous_failure.preceding_successful_run,'preceding historical run')+' also succeeded.</p></details></article></div>'+
    '<details data-technical><summary>Metric definitions and historical counts</summary><p>The '+ci.passed+' pass / '+ci.optional_skips+' skip result belongs to the source-pinned CI run above. It is not a new local test run, a capability score or a performance measurement. The older 153-registration controlled checkpoint remains dated in the records and JSON history. Test discovery is not test completion.</p></details>')+
  section('open-work','04 / OPEN WORK','The unfinished work stays visible.','The workstreams are paused at a safe stopping point. These are the checks that would make the next step meaningful.',issueRows())+
  section('handoff','05 / PICK UP THE THREAD','A clear start for the next AI.','Begin with the verified state, then follow the evidence before running anything.', '<div class="handoff-panel"><div><p class="eyebrow">START HERE</p><h3>Understand first.<br>Continue deliberately.</h3><p>Read the handoff, check the current GitHub head against the pinned revision, and inspect the relevant source. Keep the paused research and saved responses intact.</p>'+
    link(repo+'/blob/'+pin+'/README.md','Pinned repository README ↗','text-link')+'</div><ol class="handoff-steps"><li><span>01</span><div><strong>Revia source</strong><p>PR #'+current.source_pr+', revision '+short+'. Latest CI: '+ci.passed+' passed, two optional skips. Installed state unverified.</p></div></li><li><span>02</span><div><strong>Research boundary</strong><p>Public reproducible source is pinned through STATE-010. Studies 011–016 are held locally; 017 is proposed.</p></div></li><li><span>03</span><div><strong>Next useful decision</strong><p>Choose one bounded validation. Review the exact evidence and authorization before resuming experiments or publishing local material.</p></div></li></ol></div>'+
    '<div class="handoff-links">'+link('milestone.json','Read the structured snapshot ↓')+link('research.html#research-handoff','RNL handoff &amp; evidence boundary →')+
    '<button type="button" id="copy-handoff">Copy handoff summary</button><span id="copy-status" role="status"></span></div>'+
    '<pre id="handoff-text" class="handoff-text">REVIA ATLAS — 2026-11-05\nSource: '+pin+'\nPR #'+current.source_pr+' merged. CI '+ci.run+': '+ci.passed+' passed, '+ci.failed+' failed, '+ci.optional_skips+' optional video skips; '+ci.jobs_passed+' jobs passed.\nInstalled app not updated or freshly audited. PR37 has a scoped real-model check for check-ins and first-time names; general invented history and a useful delegated report remain open.\nRNL paused. Public source '+researchPin+' through STATE-010. STATE-011–016 local; STATE-017 proposed. Candidate and integration: None.\nCOMPARE-001 retained 32 Qwen 0.8B replies; RNL/lookup unrun; no accuracy grade or valid comparison.\nAtlas is live at https://mahousenseii.github.io/R.E.V.I.A/. Verified publication checkpoint: Pages run 41961109918 (5 November 2026). Earlier run 41893563372 timed out; its failure remains historical evidence.\nNext: read source and handoffs; preserve saved artifacts; review exact scope before execution or publication.</pre>')+
  archive('dated-records',oldIndex,'03 November 2026')+foot();

function studyCards(){
  return studies.map(s=>'<article class="study searchable" id="'+s.id+'-current" data-search="'+e([s.id,s.tag,s.name,s.summary,s.detail,s.next].join(' '))+'"><div class="study-meta"><span>'+e(s.id.toUpperCase())+'</span>'+pill(s.tag,s.tone)+'</div><h3>'+e(s.name)+'</h3><p class="study-summary">'+e(s.summary)+'</p>'+detail(s.detail)+'<p class="study-limit">'+e(s.limit)+'</p><p class="next-line"><strong>Next</strong> '+e(s.next)+'</p></article>').join('');
}
let research=head('RNL research',true)+
  '<section class="research-hero" id="research-overview"><p class="eyebrow"><span class="gold-diamond"></span> THE RESEARCH NOTEBOOK</p><h1>RNL, <em>with context.</em></h1><p class="hero-lead">Promising ideas.<br>Evidence before integration.</p><p class="hero-description">A research track exploring recurrent neural memory and exact reuse. Positive results, failed approaches and open questions belong in the same record.</p>'+
  '<div class="hero-actions">'+link('#latest-findings','Read the latest findings ↓','button primary')+link('index.html','Back to Revia →','text-link')+'</div></section>'+
  '<div class="research-stages"><div><span class="stage-index">01</span><strong>Experimental</strong><p>STATE-017 scaling proposed</p></div><div><span class="stage-index">02</span><strong>Measured</strong><p>Named synthetic studies</p></div><div><span class="stage-index">03</span><strong>Candidate for Revia</strong><p>None</p></div><div><span class="stage-index">04</span><strong>Integrated</strong><p>None</p></div></div>'+
  '<div class="comparison-notice" id="comparison-status"><div>'+pill('Incomplete comparison','negative')+'<h2>32 replies saved.<br>No comparison result.</h2></div><div><p>Qwen 0.8B completed its question set. The owned monitoring process did not join during shutdown, so RNL and exact lookup never ran. No accuracy was graded.</p><p>The saved replies can inform a separately reviewed continuation. They cannot rank RNL, the pilot model or Revia’s usual 4B model.</p>'+
  detail('COMPARE-001: 32 saved responses; 34 preserved files; planned 46-file seal incomplete. Native processes exited and key removal was checked. No rerun, lifecycle fixture, continuation or upload is active.')+'</div></div>'+
  search('Find a study, result or limitation')+
  section('latest-findings','01 / LATEST LOCAL STUDIES','What the newer studies found.','The strongest positive result and the negative result are shown together. Detailed methods, frozen artifacts and traces for STATE-011–016 remain local and unpublished.', '<div class="study-grid">'+studyCards()+'</div>')+
  section('published-findings','02 / EARLIER WORK','What the public source supports.','The pinned public research branch contains methods and results through STATE-010. Atlas summaries of later studies do not make their full evidence public.',
    '<div class="public-findings">'+publicFindings.map(x=>'<article class="finding searchable" data-search="'+e([x.name,x.summary,x.detail].join(' '))+'"><h3>'+e(x.name)+'</h3><p>'+e(x.summary)+'</p>'+detail(x.detail)+link('#'+x.id,'Original method, counts &amp; limits ↓')+'</article>').join('')+'</div><p class="section-end">'+link(repo+'/tree/'+researchPin+'/research/rnl','Pinned public research source ↗')+'</p>')+
  section('research-handoff','03 / REPRODUCIBILITY &amp; NEXT STEPS','Know which evidence you have.','A readable summary is a starting point. Reproducing the latest studies also needs their exact artifacts.', '<div class="boundary-grid"><article><p class="eyebrow">PUBLIC</p><h3>Code through STATE-010</h3><p>Use the pinned branch revision and its method, lock and config references. A moving branch head is not the same source identity.</p>'+
    link(repo+'/tree/'+researchPin+'/research/rnl','aac6f161 · research/rnl ↗')+'</article><article><p class="eyebrow">LOCAL · UNPUBLISHED</p><h3>Studies 011–016 and saved replies</h3><p>Source deltas, per-example predictions, traces, three frozen delta snapshots and the partial comparison seal are still held locally. This website does not supply them.</p></article></div>'+
    '<details data-technical><summary>What a reproducible, reviewed handoff needs</summary><ul><li>Exact method/config/source/test deltas and all positive and negative results.</li><li>Fixture generators, seeds, evaluation definitions and per-example predictions/traces.</li><li>Three frozen delta snapshots, tensor inventory, original hashes and lineage.</li><li>The comparison fixture/parser, 32 retained requests/responses, incomplete report and partial seal.</li><li>An inventory mapping symbolic artifact IDs to sanitized filenames and export hashes.</li></ul><p>Raw local logs/manifests can contain private paths and process metadata. The exact export inventory and destination need review before any upload. A summary alone does not reproduce these studies.</p></details>'+
    '<div class="next-research"><span class="gold-diamond"></span><div><h3>The next study is still a proposal.</h3><p>STATE-017 would ask whether the separate-slot result scales. No execution, training, application integration or publication is implied. Review saved artifacts and cleanup before considering a new bounded run.</p></div></div>')+
  archive('research-records',oldResearch,'earlier summary through STATE-014')+foot();

const record={
  schema_version:3,
  milestone:'Revia source through PR'+current.source_pr+'; RNL paused; direct comparison incomplete',
  recorded_utc:'2026-11-05',
  refresh_mode:current.refresh_mode,
  repository:'MahouSenseii/R.E.V.I.A',
  current,
  source:{main_commit:pin,installed_app_updated:false,release_qualified:false},
  product_delivery:'Merged source on GitHub main; installed app unchanged; no application deployment',
  integration:{main_commit:pin,installed_app_updated:false,runtime_release_qualified:false},
  counts:history.counts,
  counts_scope:'Historical 153-registration controlled checkpoint ONLY. Current source CI is current.ci; discovery is not a pass count.',
  changes,
  research_findings:studies,
  open_work:issues,
  history_scope:'The complete previous milestone snapshot is preserved below. Its current/next/unrun fields describe its own date, not the 2026-11-05 snapshot.',
  history
};
write('index.html',index);
write('research.html',research);
write('milestone.json',JSON.stringify(record,null,2)+'\n');
for(const [from,to] of [['styles.css','atlas.css'],['records.css','public.css'],['interactions.js','atlas.js']])
  fs.copyFileSync(path.join(path.dirname(fileURLToPath(import.meta.url)),from),path.join(root,to));
console.log('Built two static pages; preserved dated HTML and full milestone history.');
