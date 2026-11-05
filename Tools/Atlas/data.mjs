export const repo='https://github.com/MahouSenseii/R.E.V.I.A';
export const pin='4a2cb2ba26d58eac6218d7778e2491a4e05b3b1e';
export const researchPin='aac6f161fd080db400317670e4b3a78973dbb105';
export const current={
  as_of:'2026-11-05',
  source_commit:pin,
  source_pr:37,
  state:'Paused at a safe stopping point',
  refresh_mode:'Manual, source-pinned snapshot; no live monitoring',
  ci:{run:42062451066,jobs_passed:6,passed:155,failed:0,optional_skips:2},
  installed_app:'Not updated or freshly audited by this work',
  research:{
    public_source_commit:researchPin,
    public_source_through:'STATE-010',
    local_studies:'STATE-011–016',
    proposed_study:'STATE-017',
    comparison_complete:false,
    graded:false,
    saved_responses:32,
    integration:'None',
    candidate:'None',
    state:'Paused; no continuation or upload active'
  },
  pages:{
    state:'published',
    artifact:'Validated and uploaded',
    deployment:'Verified live on GitHub Pages',
    verified_publication:{
      run:41961109918,
      source_commit:'ac755b28d635676e398623841b7c4c49eee5416c',
      url:'https://mahousenseii.github.io/R.E.V.I.A/',
      verified_utc:'2026-11-05T03:23:23.246Z'
    },
    previous_failure:{
      run:41893563372,
      recorded_date:'2026-11-04',
      deployment:'Both attempts stalled and were cancelled at the ten-minute timeout',
      cause:'Underlying historical Pages backend cause unverified',
      preceding_successful_run:41880016023
    },
    redesign:'Published from reviewed PR35; installed Revia app unchanged'
  }
};

export const changes=[
  {pr:37,title:'Keep casual check-ins and stated names grounded.',
    summary:'“What are you up to?” no longer invents a body or offscreen life, and “my name is X, not Y” no longer invents a mix-up nobody had.',
    detail:'Merged change to conversationStylePolicy.cpp, with conversationGuidanceTests.cpp regressions built from actual replies. Turn guidance plus sentence-level repair; streaming is kept because a repair never withdraws an already released sentence. Measured through the conversation runtime with the SHA-pinned Qwen3.5-4B on three fixed prompts, four passes each: 5/12 before, 12/12 after on CPU, 11/12 on the RTX 5070, where the miss was reply length. Repair still does much of the work, the gates match phrases, and the installed app is unchanged.',
    next:'Rebuild and requalify the installed app; widen the fixed-prompt check to more history-shaped turns.'},
  {pr:34,title:'Keep corrections grounded in what was actually said.',
    summary:'Quoted corrections no longer create a messenger story in the host response rules.',
    detail:'The merged change adjusts conversationStylePolicy.cpp and speechAttributionTests.cpp. This host-code fix does not establish that a real model will never invent history.',
    next:'Check the remaining model-generated false-history case in a separately reviewed real-model trial.'},
  {pr:33,title:'Accept an explicit name self-correction.',
    summary:'A clear correction can update the speaker name without being treated as rejected naming.',
    detail:'Source fix merged. The exact latest main CI includes the speech-attribution regression coverage; the installed application has not been requalified.',
    next:'Confirm correction and recall during a real session.'},
  {pr:32,title:'Keep ordinary recurrence from creating correction friction.',
    summary:'Repeated ordinary references should not be treated as repeated corrections.',
    detail:'The original merge encountered a retained CI failure; later main CI succeeded. Preserve the earlier failure rather than counting every historical attempt as a pass.',
    next:'Keep ordinary conversation and correction behavior separate in runtime checks.'},
  {pr:31,title:'Keep reassurance from becoming hostility evidence.',
    summary:'Reassuring language should not manufacture a hostile interaction.',
    detail:'Conversation host policy and its focused regression coverage are merged. This is a bounded source correction.',
    next:'Check reassurance with the configured model and real conversation context.'},
  {pr:30,title:'Retain a later valid name correction.',
    summary:'An earlier rejected name no longer hides a later valid correction.',
    detail:'Merged identity parsing change. A passing fixture is evidence for the tested cases, not for every possible spoken or written name.',
    next:'Include mixed rejection and correction examples in runtime qualification.'},
  {pr:29,title:'Ignore a nickname the speaker rejected.',
    summary:'A rejected nickname should not become the speaker’s accepted identity.',
    detail:'Merged speaker-resolution change. Real voice attribution remains a separate qualification.',
    next:'Check name resolution and consented voice identity together.'},
  {pr:28,title:'Separate your server from Revia’s runtime.',
    summary:'A question about your own server should not be answered as Revia’s service status.',
    detail:'Merged ownership/context clarification. It does not change deployment or install a model server.',
    next:'Keep user-owned infrastructure questions in their stated context.'}
];

export const issues=[
  {id:'report',area:'Revia',title:'A useful delegated report is still unverified.',
    summary:'An earlier real-model worker listed files, then stopped without an accepted report. Chat remained available.',
    status:'Open · real-model result missing',
    detail:'The cause of the early stop remains unresolved. Clearer receipts and private stopping diagnostics are merged; they have not demonstrated successful model completion.',
    next:'Review the saved evidence, verify the exact runtime package, and prepare one bounded known-path report check. Do not replay an old run automatically.'},
  {id:'history',area:'Revia',title:'Model-generated invented history remains open.',
    summary:'A scoped real-model check now covers two common cases. General grounding is still not established.',
    status:'Open · narrowed by a scoped real-model check',
    detail:'PR34 fixed a quoted-correction path. PR37 was then measured through the conversation runtime with the pinned 4B: casual check-ins went from 0/4 to 4/4 and first-time name statements from 1/4 to 4/4 on CPU; 11/12 cases passed on GPU. The model still produces invented details that host repair removes. This is not proof of general truthfulness or of improvement in the installed app.',
    next:'Add more history-shaped turns to the fixed real-model check, then requalify the installed app.'},
  {id:'comparison',area:'RNL',title:'The direct comparison stopped before RNL ran.',
    summary:'32 Qwen 0.8B replies were saved. Supervision cleanup failed; RNL and exact lookup did not run.',
    status:'Incomplete · no grade or ranking',
    detail:'COMPARE-001 stopped at “Owned monitor did not join”. The native processes exited. The 34 preserved files are not the planned 46-file protocol seal. The 0.8B pilot cannot rank the usual 4B model.',
    next:'Review preservation and cleanup first. A separately reviewed continuation could reuse the saved replies and run only the remaining cells.'},
  {id:'runtime',area:'Revia',title:'Code checks do not qualify the installed app.',
    summary:'Restart memory, physical desktop, real voice/media and release checks remain separately scoped.',
    status:'Open · runtime qualification',
    detail:'The latest source CI passed 155 tests. The two optional video checks were skipped. Historic long-path save limits, setup interruptions and timing failures remain in the dated records.',
    next:'Choose one controlled runtime workflow after the source and installed revision are reconciled.'}
];

export const studies=[
  {id:'state-016',tag:'Positive synthetic result',tone:'positive',name:'Separate addresses recovered the retained errors.',
    summary:'Fixed separate slots answered all 1,008 questions correctly, while the original and sham controls each answered 970.',
    detail:'STATE-016: 1,008/1,008 versus 970/1,008; 38 rescues and no new errors. Three frozen seeds, four supplied keys, eight values. No training updates. This isolates a useful memory-address intervention on this task; it does not establish general routing, language understanding or equal-capacity training.',
    limit:'Local study · detailed source and traces unpublished',next:'A larger scaling study, STATE-017, is proposed only.'},
  {id:'state-015',tag:'Negative result',tone:'negative',name:'The alternative reader made recall worse.',
    summary:'Changing only the reader recovered three answers but broke ten others.',
    detail:'STATE-015: 963/1,008 correct versus the original 970/1,008. The fixed reader used the same frozen saved state; three seeds, zero training updates. This negative result limits the proposed reader change and does not erase the earlier memory finding.',
    limit:'Local study · detailed source and traces unpublished',next:'Keep this result alongside the positive separate-slot result.'},
  {id:'state-014',tag:'Diagnostic finding',tone:'neutral',name:'Errors appeared during later unrelated writes.',
    summary:'Older facts were correct just after their updates; some became wrong after other keys were written.',
    detail:'STATE-014: all older targets were correct at both update checkpoints. By the final checkpoint, 10/504 older answers were wrong after four unrelated writes and 32/504 after sixteen. This identifies an interval, not the cause of the failure.',
    limit:'Local trace audit · no training or quality improvement',next:'Test causes without treating the interval as a proven explanation.'},
  {id:'state-013',tag:'Diagnostic finding',tone:'neutral',name:'Older facts accounted for most retained errors.',
    summary:'At eight and sixteen unrelated writes, 32 of 38 wrong answers concerned the older requested fact.',
    detail:'STATE-013 re-read saved errors. Some predictions repeated the queried key’s prior value. No retained wrong answer matched the partner fact or final unrelated write. This is saved-error analysis, not a new model run.',
    limit:'Local analysis · task-specific error pattern',next:'Use the pattern to guide bounded interventions.'},
  {id:'state-012',tag:'Robustness limit',tone:'neutral',name:'More unrelated writes exposed a weaker margin.',
    summary:'Frozen learned memory scored 998/1,008 after four writes and 970/1,008 after eight or sixteen.',
    detail:'STATE-012 reused matched families over three seeds without training. All sixteen-write screens passed, but the weaker seeds sat exactly on the complete-group threshold. One seed remained perfect.',
    limit:'Local inference-only audit · three seeds',next:'Preserve both the high recall and the narrow reliability margin.'},
  {id:'state-011',tag:'Positive synthetic result',tone:'positive',name:'A different learned-memory recipe recalled more facts.',
    summary:'Learned memory answered 4,009/4,032 correctly; the fresh GRU answered 1,035. Exact lookup solved the task.',
    detail:'STATE-011: 985/1,008 complete four-question groups versus zero for GRU; exact lookup 100%. Four supplied keys/eight values. Unequal whole recipes: 153 versus 4,968 parameters and 32 state floats. Learned optimization took 88.65s versus 57.70s for GRU, so no speed advantage was established.',
    limit:'Local trained study · unequal recipes, known keys',next:'A synthetic memory finding remains separate from any Revia or LLM benefit.'}
];

export const publicFindings=[
  {id:'prefix-004',name:'Existing prompt-prefix reuse was observed.',
    summary:'Cache-on reused 399 of 438 prompt tokens and both modes gave the expected 26-token answer.',
    detail:'One fixed-order Qwen3.5-0.8B pair: 4.666s versus 51.408s to first content, with each primer costing about 50.5s. This verifies an existing llama.cpp mechanism; it is not a reliable speedup estimate, a usual 4B result or a new RNL cache.'},
  {id:'event-cost',name:'Exact reuse helped the synthetic repeat workload.',
    summary:'Repeated-event cases met the declared timing rule; zero-repeat controls had no benefit.',
    detail:'RNL-003: eight repeated-event conditions across four matrix sizes, median paired uncached/cached ratios 1.213–3.994. Retained buffers increased; no RAM reduction or real Revia savings were established.'},
  {id:'state-010',name:'The earlier neural readers missed reliability targets.',
    summary:'Some individual recall improved, but STATE-006–010 did not demonstrate reliable keyed memory.',
    detail:'STATE-010 scored 27.88%, 29.56% and 27.93% for three reader forms, versus 12.5% references. Every trial had 0/168 complete four-case groups. Earlier training and query-blind controls remain in the full records.'},
  {id:'cache-contract',name:'The cache must preserve the full computation contract.',
    summary:'Keying by selected expert and every input bit preserved 280 tested steps. A shortcut did not.',
    detail:'RNL-002 retained routing, activation and recurrent updates every time. The first supplied toy cache changed behavior, so skipped work could not count as an equivalent speed improvement.'}
];
