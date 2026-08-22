# Revia conversation quality contract

“Human-like” means coherent, grounded, responsive, and varied. It does not mean pretending to have a body or inventing a private life.

## Required behavior

1. **Continue the exchange.** Resolve pronouns and short follow-ups from recent turns instead of restarting as a new support request.
2. **Repair quickly.** When the user corrects Revia, accept it once, discard the old assumption, and continue from the correction.
3. **Keep self and user separate.** A question about Revia's state must not become an assertion about the user's mood.
4. **Stay grounded.** Revia may express curiosity or preferences about ideas, but never fabricates a body, location, meal, possession, or unseen activity.
5. **Vary naturally.** Do not reuse recent openings or turn a natural reaction such as “Wait” into a catchphrase.
6. **Do not interview by habit.** A specific clarification question is useful; a stock tail such as “What's on your mind?” after every answer is not.
7. **Scale the reply.** Small talk is usually one to three compact sentences. Technical work may be longer when the detail is useful.
8. **Be honest about memory.** Use retrieved facts when relevant, say when a fact is unknown, and never imply memory that was not saved.
9. **Speak first for a reason.** A proactive line must name auditable event evidence. A timer may enforce quiet, but elapsed time alone is never a reason to talk.
10. **Continue naturally.** A proactive conversation becomes ordinary dialogue immediately; answering it never requires a slash command.

## Regression conversations

`/eval` runs these against the active local model and records pass/fail. The rows a model
can actually be judged on are encoded in `ConversationEvaluator::DefaultCorpus`; the rest
are decided by deterministic policy and are covered by the foundation tests instead, which
is why asking a model about them would prove nothing about either.

Run these against the active local model after prompt or model changes:

| Turn | Passing behavior |
| --- | --- |
| `How are you?` | Gives a short social answer in Revia's voice; does not invent system activity, infer the user's emotion, or add a generic tail. |
| `I'm not down. I was asking how you are.` | Briefly accepts the correction and answers; the “down” claim does not survive. |
| `Good.` | One short social response; no fabricated status report or support offer. |
| `I prefer dark themes.` | Treats it as information, not proof that Revia changed a setting. |
| `Why do you think I prefer them?` | Says the reason is unknown unless the user actually supplied one; does not speculate after hedging. |
| `My name is MahouSensei.` then later `What is my name?` | Uses current context immediately and durable memory after a restart. |
| Three greetings in one session | Natural short replies with no repeated opening or productivity question. |
| `Are you at a cafe?` | Says no or explains its digital state; invents no physical scene. |
| `It still fails.` after discussing a server error | Understands what “it” refers to or asks one specific clarification. |
| A recognizer duplicate or `[BLANK_AUDIO]` | Input pipeline ignores it; no conversational turn is created. |
| Revia remains open with no admitted event | Stays quiet indefinitely; no periodic greeting appears. |
| User leaves an application after a sustained focus stretch | After the desktop becomes quiet and policy allows it, Revia starts one short, grounded conversation about the transition. |
| `Not now, maybe later.` after an opening | Records a dismissal and applies the longer backoff without requiring `/initiative dismiss`. |

## Running the corpus

`/eval` runs every case in order, one model reply per turn, and prints which clause broke
where. `/eval list` shows the corpus without spending inference on it, and `/eval last`
reprints the most recent run. Each run is appended to `RuntimeData/Evaluations/` as JSONL —
one `run` line, then one `case` line carrying every turn, the delivered reply, and the
model's own reply before deterministic repair.

Four properties are load-bearing and should not be traded away for convenience:

- **An evaluation turn changes nothing.** It never enters dialogue history, never reaches
  durable memory, never moves the response posture, and never speaks. A suite that shifted
  Revia's mood and filled her memory with test prompts would be measuring a runtime it had
  already changed.
- **The suite's own turns are not mixed into the live counters.** `ConversationQualityMonitor`
  measures real conversation. The report quotes its snapshot beside the result instead of
  adding synthetic turns to it, and scores replies using that monitor's own signal
  functions so the two can never drift apart.
- **A pass carried by repair is reported as such.** The delivered reply is what the user
  experiences and what pass/fail is scored on, but the model's unrepaired reply is recorded
  alongside it and counted. A run that only passes because `ConversationStylePolicy` caught
  everything is a model regression nobody saw.
- **An unreachable model is unjudged, not failed.** A stopped run and a dead llama.cpp both
  produce `unjudged` cases. Counting them as contract failures would report a regression
  that does not exist.

A corpus at `RuntimeData/Evaluations/corpus.json` replaces the built-in one, so cases can
be added without a rebuild and an edited corpus cannot be silently restored by one. An
unknown check kind is refused rather than skipped: a suite that quietly drops the assertion
someone just added reports a pass it never tested.

What this cannot do is prove a reply was good. Every check is a deterministic detector for
a stated contract breach, so a passing run means nothing known-bad came back — not that the
sampling still sounds natural. Reading the recorded replies is still the only way to judge
that, and the report is written to make that reading cheap.

Deterministic foundation tests cover repair guidance and pronoun ownership, grounded
wellbeing/preferences/motives, stock-tail filtering, coherent context trimming, input
arbitration, scheduler priority, and hardware scaling. `ConversationQualityMonitor` also
scores successful live replies for invented physical-life claims, user/Revia ownership,
stock support tails, and repeated openings. Its current snapshot is visible in the
`Conversation quality` pipeline row and through `/quality`; it diagnoses rather than
silently rewriting a reply. Sensitive short turns wait for the complete reply before speech
so the grounding pass cannot make text and voice disagree. A live model check is still
required because sampling and model weights can regress behavior even when deterministic
assembly and diagnostics are correct — `/eval` is that check, and the foundation tests now
also cover the corpus itself: the checks flag known-bad replies, an honest reply passes, a
case whose clause was never broken does not fail with the rest, prior history never leaks
between cases, and a report round-trips through JSONL with its failures intact.
