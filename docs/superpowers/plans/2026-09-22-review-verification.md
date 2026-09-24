# Verification of the fixes for review 5c9cfb2

The user requested implementation of the ten findings in `Revia_Review_5c9cfb2.md`
and its earlier submission-freshness concern. The review's read-only description
applies to the earlier review, not this implementation.

Initial changes are in the user's commit `26267be`. Follow-up corrections and this
record are working-tree changes. No commit or push was made by the implementation
agents.

## Changes and focused evidence

| Finding | Implemented behavior | Verification |
| --- | --- | --- |
| R01: quoted authority | One left-to-right parser handles supported quote styles, escaping and nesting. Incomplete or ambiguous parsing yields no authority-bearing suffix. Payload and instruction consumers share the parse. | Production quote/intent regressions cover mixed delimiters, nesting, reordered targets, many spans, apostrophes, escapes and malformed input. |
| R02: memory loss | `Duplicate` requires identical nonempty summaries. The semantic discard route is removed from `Save`; its verified formatting-normalized text comparison is the only deduplication authority. | Actual `Save` and reopen tests preserve both claims for 27 combinations of proposition pairs and controlled similarities. |
| R03: approved control drift | Pointer and UIA actions bind the approved control and consequence, reacquire element bounds, and check the binding again after approval and draft inspection. | A native disposable fixture rejects a relabelled control in the same HWND. A moved control with a 2.1-second approval delay is reacquired and executes at its current location. |
| Earlier submission freshness | Draft identity, exact contents and surrounding recipient context are checked again at the commit boundary. Runtime receipts distinguish an actual submission from navigation, Save, or a refused action. Navigation exemptions are revalidated, and persisted guarded actions cannot resume without fresh runtime validation. | Native fixture tests reject changed body, writable recipient, static recipient and read-only recipient with zero invocation effects. Unchanged submission produces one effect. Unit tests cover cleared drafts after sending, no duplicate send, retry after pre-input refusal, ordinary navigation, and keyboard edits in fields labelled Send. Persistence tests save/reopen real goals, retain guard obligations, discard stale runtime evidence, and refuse guarded native actions before input. |
| R04: premature speech | Backend deltas remain private. Delivery receives only the completed answer after style handling, hard filtering, optional AI review and a final hard-filter pass. | Fake-backend integration tests vary chunk boundaries and capability states, and assert that callbacks equal the approved response. |
| R05: SVG references | Pinned Expat parses XML without external resolution. Decoded values and namespace URIs pass restricted element, attribute and style rules before canonical serialization. | Regressions reject encoded external references, namespace aliases to external references, malformed XML and unsupported CSS. The Qt renderer accepts a local encoded gradient; the loopback resource listener observes no requests. |
| R06: context fitting | All bytes, including whitespace, consume a conservative content allowance. Framing and generation reservations are explicit; small contexts fail rather than gaining an artificial minimum. Context-overflow recovery is limited to one reduced request before any output. | Whitespace, rare-text, identifier, multilingual, small-context and bounded fake-backend retry regressions. Offline native tokenizer measurements are recorded below. |
| R07: UTF-8 output | Strict validation is separate from code-point-safe byte truncation. Output, archive/context, preview and backend-error boundaries use the shared helper. | Full-filter and strict JSON tests cover CJK, emoji, combining text, malformed encodings, punctuation and grounded replacements, including 256/257/1024/4096-byte reply budgets. |
| R08: journal recovery | Records decode into temporary typed state. Malformed rows are counted and skipped; successful recovery is committed after reading. A failed reload preserves the prior snapshot and path. | Tests cover valid/invalid/valid sequences, missing and null fields, wrong scalar/array types, malformed resolutions and truncated rows. |
| R09: Windows build | The workflow selects and verifies MinGW 13.1 for Qt 6.8.3, requires the desktop target and enables large COFF objects where needed. | The hosted run for `26267be` configured and built successfully. Local full-build verification is recorded below. |
| R10: PowerShell setup | The reproduced PowerShell 5.1 failure came from a non-ASCII banner in a UTF-8 file without a BOM. The banner is ASCII; workflow errors now include file, line, column and source extent. | Actual Windows PowerShell 5.1 parsing and downloader dependency checks pass locally; the hosted setup job for `26267be` also passes. |

The expanded context tests exposed an additional native stack overflow in speech
attribution's regular expression on a 12,000-byte whitespace run. The fix uses a
collapsed search view with original byte offsets and a bounded name token. The
standalone failing probe now succeeds; regressions preserve the original quoted
bytes and the user's following text. A separate HTTP-error regression reproduced
and fixed a UTF-8 cutoff that previously made JSON serialization throw.

The renderer test also explicitly resolves the offscreen platform plugin from the
configured Qt kit. Desktop deployment alone only supplies the Windows plugin.

The first integrated test run passed six groups and got past the earlier crash, but
failed the reversed-preference regression. It exposed that the implementation still
contained the old subset rule despite updated headers and tests. The final correction
removes that rule and the semantic storage discard route entirely. The structured
memory and optional live-memory tests now require broader and uncertain claims to
remain recoverable, while exact duplicates still deduplicate.

## Integrated verification

Final build: passed. A second build check reported `ninja: no work to do`, confirming
the binaries include the final source changes. Existing compiler warnings remain;
this is not a warning-free-build claim.

Final CTest run: **7/7 passed, zero failures, 83.15 seconds total**.

| Registered group | Result | Time |
| --- | --- | --- |
| Revia.Foundation | Passed | 78.00 s |
| Revia.IdentityFinalSave | Passed | 0.72 s |
| Revia.OperatorSession | Passed | 1.75 s |
| Revia.BrowserWorkerPolicy | Passed | 0.23 s |
| Revia.QwenTtsServicePolicy | Passed | 0.82 s |
| Revia.SvgRenderer | Passed | 0.08 s |
| Revia.DesktopSmoke | Passed | 1.54 s |

Commands: `cmake --build build/review-ci --parallel 4`, then
`ctest --test-dir build/review-ci --output-on-failure --timeout 600`.
The final CTest log is `build/review-ci/Testing/Temporary/LastTest.log`.

The build uses a fresh `build/review-ci` output directory, Qt 6.8.3 and MinGW 13.1.
Existing dependency source downloads are reused; compiled objects and libraries
were rebuilt. The complete registered suite contains Foundation, IdentityFinalSave,
OperatorSession, BrowserWorkerPolicy, QwenTtsServicePolicy, SvgRenderer and
DesktopSmoke. Native input tests are separate disposable-fixture checks, not an
unrestricted run against the user's applications.

Offline content-token validation ran seven whitespace, rare-string, identifier and
multilingual cases against each configured Fast/Main/Expert GGUF tokenizer (21
measurements). Every measured token count was positive and within the byte-based
content allowance. A further 90 prompts use the actual GGUF chat templates: 15
text-only scenarios, both thinking settings, and all three installed models. All
native tokenizer counts fit within content bytes plus 32 tokens per message and the
384-token request reserve. Actual counts ranged from 20 to 1,253 for Qwen3.5 and
18 to 4,049 for Qwen3-VL. Of 48 small-context checks at 512/768/1024/2048 tokens,
36 were admitted and fit; 12 at 512 were conservatively rejected despite actually
fitting. The GGUF templates were rendered with local Jinja2 and measured by the
native tokenizer; this is not a live llama-server/minja or production-fitter test.
Qwen3-VL's template ignores the thinking setting. Tool/media templates and custom
template overrides were not measured.

## Hosted status and limits

Hosted run: <https://github.com/MahouSenseii/R.E.V.I.A/actions/runs/35762637611>.
For commit `26267be`, the Windows build and PowerShell 5.1 job pass. The Foundation
test crashed; the other six registered test groups passed. That run predates the
speech-attribution and other follow-up corrections, so it is not evidence of a
complete hosted pass for the final working tree.

Speech now waits for whole-response approval. The conservative token allowance can
retain less history than exact counting. UTF-8 truncation preserves code points,
not entire grapheme clusters. Distinct memory claims remain recoverable, but general
semantic paraphrase merging and contradiction supersession are still unresolved.
GUI revalidation narrows the race window; generic UI automation does not provide a
transaction with the target application. The tests do not claim live model, GPU,
live TTS, or arbitrary third-party application coverage.
