# Cppcheck Zero-Warning Remediation Plan

## Goal

Reduce current `cppcheck` output to zero warnings while preserving behavior,
maintaining readability, and staying within project coding constraints.

## Current Warning Set (Baseline)

- `src/main.cpp`:
  - `knownConditionTrueFalse` warnings in `HtmlAppendContentPretty` (multiple lines)
  - `constVariablePointer` (`pos` can be `const char*`)
  - `variableScope` (`queryExecutionFailed` scope)
- `src/sections/markdown_rendering.inc`:
  - `knownConditionTrueFalse` (`idLen == 0U` always false)

## Work Plan

1. Refactor `HtmlAppendContentPretty` for static-analyzer clarity
- Keep behavior and output formatting equivalent.
- Replace intertwined branching with explicit staged state evaluation:
  - per-line parse: `isTagLine`, `isClosingTag`, raw-block markers
  - indentation adjustment phase (pre-emit / post-emit)
  - block-state transitions (`inTextarea`, `inScript`)
- Eliminate branch structures that produce analyzer-inferred constant outcomes.

2. Address direct small warnings
- `DisableLazyLoadingForFirstImage`: make search pointer `const char*`.
- Search page builder: narrow `queryExecutionFailed` variable scope.
- `IsDocumentLinkTarget`: remove redundant `idLen == 0U` condition already implied
  by earlier length guard.

3. Comments and readability
- Add/refresh concise function comments where control-flow was simplified.
- Ensure comments describe intent and constraints, not line-by-line mechanics.

4. Validation
- Build/test gates:
  - `cmake --build build -j4`
  - `ctest --test-dir build --output-on-failure`
  - `./build/mswiki --self-test`
- Static analysis gate:
  - `cppcheck --enable=warning,style,performance,portability --std=c++11 --inline-suppr src/main.cpp`
- Acceptance criteria:
  - zero warnings from the baseline set
  - no test regressions
  - HTML output remains readable and stable

## Fallback

- If a residual warning is a proven false-positive after refactor, isolate
  condition evaluation into a small helper function so analyzer invariants are
  explicit.
- Suppressions are last resort and should be avoided unless technically
  unavoidable.
