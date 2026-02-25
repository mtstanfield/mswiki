# mswiki

`mswiki` is a lightweight, SQLite-backed markdown wiki server implemented as a single C++11 binary.

## Current Scope

- Single-process, single-threaded HTTP server
- SQLite data store (pages, backlinks, images, documents)
- Local/home-wiki scale with a minimal dependency/runtime footprint
- Reverse-proxy-first deployment model for external exposure and auth

## Features

- Page CRUD by slug (`/page/<slug>`, `/edit/<slug>`, `/save/<slug>`, `/delete/<slug>`)
- Wiki links with backlink index:
  - `[[Target Page]]`
  - `[[target-page|Label]]`
- Markdown rendering with support for:
  - Headings (`#`..`######`)
  - Inline emphasis (`*italic*`, `**bold**`, `***bold+italic***`)
  - Inline code (`` `code` ``)
  - Fenced code blocks (triple backticks)
  - Blockquotes (`> `)
  - Lists (`- item` or `* item`)
  - One-level nested sublists (`-- child item` under current list item)
  - Inline links and images
  - Footnotes (`[^id]` + `[^id]: text`)
- Tufte-inspired view styling:
  - Right-margin sidenotes for footnote references
  - Right-margin figures for markdown images
- Binary media in SQLite BLOBs:
  - Images (`/image/<id>`)
  - PDF documents (`/document/<id>`)
- Edit-page media/document controls:
  - Upload image / upload document
  - Insert markdown snippet buttons
  - Delete image / delete document actions with confirmation
- View-page sections:
  - Backlinks
  - Images
  - Documents
- Built-in self-test mode (`--self-test`)
- Distroless nonroot runtime image (Docker)

## Security / Robustness Notes

- Markdown URL scheme filtering blocks unsafe schemes in rendered links/images.
- Footnote identifiers are allowlisted before use in HTML attributes.
- Upload handlers verify target page existence before inserting blobs.
- The server is intentionally single-threaded; run behind a reverse proxy for production exposure.
- CSRF/auth/session controls are intentionally out of app scope.

## Build

```sh
cmake -S . -B build
cmake --build build -j
```

### CMake Options

- `MSWIKI_STATIC` (default `OFF`): static link mode for `mswiki`
- `MSWIKI_FUZZING` (default `OFF`): build libFuzzer targets

## Run

```sh
./build/mswiki --listen 0.0.0.0 --port 8080 --db ./mswiki.db
```

Open [http://localhost:8080](http://localhost:8080).

### Runtime Arguments

- `--listen <addr>`: IPv4 listen address (default `0.0.0.0`)
- `--port <port>`: listen port (default `8080`)
- `--db <path>`: SQLite DB path (default `./mswiki.db`)
- `--max-body-bytes <bytes>`: max accepted request body size (default `10485760`)
- `--self-test`: run in-process self-test suite and exit

## Test

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Or directly:

```sh
./build/mswiki --self-test
```

## Docker

### App image (local architecture)

```sh
docker build -t mswiki:local .
docker volume create mswiki-data
docker run --rm -p 8080:8080 -v mswiki-data:/data mswiki:local
```

### App image (amd64 target)

```sh
docker buildx build --platform linux/amd64 -t mswiki:amd64 --load .
docker volume create mswiki-data
docker run --rm --platform linux/amd64 -p 8080:8080 -v mswiki-data:/data mswiki:amd64
```

Runtime image details:

- Base: `gcr.io/distroless/static-debian12:nonroot`
- User: `65532:65532`
- DB/data path in container: `/data/mswiki.db`

### Fuzz image

```sh
docker buildx build --platform linux/amd64 --target fuzz -t mswiki:fuzz --load .
```

HTTP request fuzz target (default entrypoint):

```sh
docker run --rm --platform linux/amd64 mswiki:fuzz
```

Markdown fuzz target:

```sh
docker run --rm --platform linux/amd64 \
  --entrypoint /src/fuzz-build/mswiki_markdown_fuzz \
  mswiki:fuzz \
  -dict=/src/fuzz/markdown.dict -timeout=5 -max_total_time=3600 /corpus/markdown
```

Multipart fuzz target:

```sh
docker run --rm --platform linux/amd64 \
  --entrypoint /src/fuzz-build/mswiki_multipart_fuzz \
  mswiki:fuzz \
  -dict=/src/fuzz/multipart.dict -timeout=5 -max_len=65536 -max_total_time=3600 /corpus/multipart
```

## Data Model (SQLite)

- `pages`
  - `slug`, `title`, `markdown`, `created_at`, `updated_at`
- `page_links`
  - `from_slug`, `to_slug` (backlink index)
- `images`
  - `id`, `page_slug`, `filename`, `mime_type`, `data`, `created_at`
- `documents`
  - `id`, `page_slug`, `filename`, `mime_type`, `data`, `created_at`

## UI Notes

- Editor toolbar and insert buttons use client-side JavaScript for in-place text insertion.
- Core page rendering and data flows are server-side.
- The favicon is an inline cat emoji SVG (`🐱`).
