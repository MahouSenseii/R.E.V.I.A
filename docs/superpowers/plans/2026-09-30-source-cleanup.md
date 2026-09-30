# Source Cleanup Implementation Plan

> **For agentic workers:** Use Superpowers review and verification for each cleanup task. Preserve the uncommitted audit fixes already in this checkout.

**Goal:** Remove verified leftovers and make source ownership, signatures, and comments easy to read.

**Architecture:** Keep existing classes, type names, field order, defaults, and behavior. Move shared definitions into their existing domain folders and split mixed implementation files along existing class boundaries.

**Tech Stack:** C++20, Qt 6, CMake, Python, clang-format.

**Spec:** The user's cleanup instructions in this conversation: single-purpose folders and classes; function signatures on one or two lines where practical; short comments only where context is needed.

## Constraints

- Preserve saved data, optional working features, and prior audit fixes.
- Delete files only after checking code, build, scripts, configuration, and documentation references.
- Keep function bodies readable; compact parameter lists rather than compressing logic.
- Preserve comments explaining locking, resource lifetime, persistence, and permission boundaries.
- Keep changes uncommitted for review in the current checkout. A complete pre-cleanup snapshot is in `build/cleanup-20260930/baseline`.

## Tasks

- [x] Replace `Public/Library` with domain-owned settings and transport headers; update direct consumers and verify each new header compiles independently.
- [x] Split `identityTypes.cpp` into development and relationship implementations; move context eviction out of retained log counts and session resource methods into `sessionResources.cpp`. Compare moved definitions with the baseline.
- [x] Remove the stale IDE workspace backup, orphaned requirements manifest, unused `memoryType`, and unimplemented backend enum entries. Prevent the backup from returning.
- [x] Compact C++ signatures with whitespace-only edits; add a formatter configuration and concise contributor rules. Check token equivalence and representative one/two-line results.
- [x] Shorten verbose ownership/history comments without dropping useful contracts. Extend the existing architecture map and README source navigation.
- [x] Build all targets, run the registered tests, inspect diagnostics, and obtain an independent cleanup review.

## Review Focus

- Every former Library type retains its original name, fields, order, and defaults.
- Each header includes its own dependencies, without broad compatibility umbrellas.
- Split helpers retain their existing behavior and all consumers remain discoverable.
- Formatting preserves strings, comments, macros, and executable tokens.
- Removals affect verified leftovers only; runtime data and prior fixes remain intact.
