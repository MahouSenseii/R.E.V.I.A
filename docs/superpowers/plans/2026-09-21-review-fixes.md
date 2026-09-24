# Review fixes for 5c9cfb2

The user requested implementation of the findings in `Revia_Review_5c9cfb2.md`.
Its read-only mode describes the earlier review, not this implementation.

Work is divided by production boundary so independent fixes can be verified together:

- [x] R01/R06: unify quote parsing and authority consumers; conservatively bound context,
  including whitespace, UTF-8, small windows, and bounded backend overflow recovery.
  Regressions: quotedPayloadTests, operateIntentTests, contextFittingTests.
- [x] R02/R08: preserve uncertain memory candidates and recover typed journal records
  independently. Regressions: classifier and actual Save, selfAssessmentTests.
- [x] R03/submission freshness: bind approval to the control and its consequence,
  reacquire current bounds, and recheck draft content at the submission boundary.
  Regressions: target binding, content gate, task progression, native fixture if available.
- [x] R04/R07: deliver only completed, hard-filtered output; preserve UTF-8 at output,
  event preview, and archive byte boundaries. Regressions: fake backend delivery callbacks,
  capability toggles, strict JSON encoding of no-space CJK/emoji/combining text.
- [x] R05: validate parsed XML, decoded values and namespaces against a restricted SVG
  vocabulary, then serialize it. Regressions: encoded references, namespace aliases,
  malformed XML, CSS escapes, and local references.
- [x] R09/R10: reproduce PowerShell 5.1 errors, fix identified sources, expose all parser
  diagnostics; require Qt desktop, pin MinGW 13.1, enable large COFF objects and perform
  a clean Windows build plus CTest. Hosted CI status must be reported separately.

For each domain, confirm the original failure before changing production behavior,
run focused tests, review the integrated diff, then build all required targets and run
the registered test suite. Do not treat model-free tests as live-model, GPU, network,
or unrestricted native-desktop verification. No commits or publishing are required.

Implementation and local verification completed on 2026-09-22: the final Windows
build passes and all seven registered CTest groups pass. See
[the verification record](2026-09-22-review-verification.md) for focused native tests,
tokenizer measurements, and the separate hosted CI status. The hosted Windows build
and PowerShell job passed for the user's earlier `26267be` commit; the final follow-up
changes have not been pushed or run on hosted CI.
