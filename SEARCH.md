# Search Feature Plan (Pages Only)

## Scope

- Add full-site search for wiki pages only.
- Searchable content:
  - page `title`
  - page `slug`
  - page `markdown`
- Out of scope:
  - PDF text extraction/search
  - image text search
  - OCR

Rationale: keep implementation aligned with app simplicity, robustness, and low operational complexity.

## Product Decisions (Approved)

1. Query semantics: simple search mode (no advanced query syntax exposed).
2. Minimum query length: 3 characters.
3. Result limit: 20.
4. Ranking: include title and slug boosts (plus markdown body match).
5. Query errors/validation feedback: render inline on search page.
6. Search UI placement: always visible in header on all pages.

## High-Level Design

### 1) Storage and Indexing

Add an SQLite FTS5 index for pages:

- Virtual table: `pages_fts`
- Indexed fields:
  - `slug`
  - `title`
  - `markdown`

`pages` remains the source of truth; `pages_fts` is derived.

### 2) Migration and Backfill

At DB open/migration time:

- Create `pages_fts` if missing.
- Backfill/rebuild from existing `pages` rows.

This ensures search works immediately for existing data.

### 3) Write-Path Consistency

Keep FTS and canonical data in sync in existing transactions:

- On page create/update (`DbUpsertPage`): upsert corresponding `pages_fts` row.
- On page delete (`DbDeletePage`): delete corresponding `pages_fts` row.

Goal: avoid index drift and preserve deterministic behavior.

### 4) Search Endpoint

Add route:

- `GET /search?q=<query>`

Behavior:

- Missing/empty query: show search page with guidance.
- Query length `< 3`: show inline validation message.
- Valid query: run prepared FTS statement, return top 20 results.
- Any FTS parse/exec error: render friendly inline error on search page (no crash, no raw SQL errors).

### 5) Ranking and Results

Use BM25 ranking with weighted fields:

- highest weight: `title`
- medium weight: `slug`
- lower weight: `markdown`

Results display:

- page title (linked)
- slug
- short snippet around matches
- result count
- explicit no-results message

### 6) Header UI

Add search form to global header layout (all pages):

- method: `GET`
- action: `/search`
- input name: `q`

No new JS required.

## Hardening and Correctness Constraints

- Prepared statements only; no SQL string concatenation from user input.
- Strict bounds:
  - minimum length: 3
  - query max length cap (to be defined in code constants)
  - result limit: 20
  - bounded snippet length
- Escape all user-influenced output before HTML rendering.
- Maintain C++11 and existing project style constraints.
- Keep behavior deterministic and testable.

## Test Plan

Extend self-tests with search-specific coverage:

1. Route behavior:
   - `GET /search` with empty query returns search page + prompt.
   - `< 3` char query returns inline validation message.
   - valid query returns 200 and expected result links.

2. Index lifecycle:
   - page create appears in search.
   - page update changes search hits/snippet content.
   - page delete removes search hit.

3. Ranking sanity:
   - title/slug hits rank ahead of body-only hit in representative fixture.

4. Guardrails:
   - malformed/unexpected query input yields friendly inline error.
   - result list is capped at 20.

5. Regression safety:
   - existing routes/render tests remain green.

All validation runs must be sequential when reporting final counts:

- `cmake --build build -j4 && ctest --test-dir build --output-on-failure && ./build/mswiki --self-test`

## Documentation Updates (After Implementation)

Update `README.md` to include:

- Search feature scope (pages only; no PDF/image text search)
- Query rules (minimum length, limit behavior)
- User-visible behavior for empty/short/no-result/error states

## Risks and Mitigations

1. FTS drift risk
- Mitigation: update index in same transactions as page writes/deletes.

2. Noisy/too-broad results
- Mitigation: min query length 3 + limit 20 + ranking weights.

3. Query edge cases causing SQL errors
- Mitigation: strict validation, prepared statements, graceful UI error handling.

4. Output safety
- Mitigation: HTML escaping for all rendered query/result text.
