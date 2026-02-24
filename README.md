# mswiki

`mswiki` is a lightweight, SQLite-backed markdown wiki server implemented as a single C++ binary.

## Features

- Markdown page storage in SQLite (`pages.markdown`)
- Wiki-style links with backlink index (`[[Page Name]]` and `[[slug|label]]`)
- Tufte-inspired page styling with right-margin sidenotes and margin figures
- Footnotes via `[^id]` references and `[^id]: definition` blocks
- Minimal web UI for browse/edit/create
- Image uploads stored in SQLite BLOBs
- Rendered page images are scaled inline as margin figures and open full-size on click
- Uploaded images can be deleted from the page image list
- Inline cat emoji favicon (`🐱`) with built-in stylesheet
- No client-side JavaScript required for core flows

## Build

`cmake` path:

```sh
cmake -S . -B build
cmake --build build
```

Direct compile path:

```sh
c++ -std=c++11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fno-exceptions -fno-rtti src/main.cpp -lsqlite3 -o mswiki
```

## Run

```sh
./mswiki --listen 0.0.0.0 --port 8080 --db ./mswiki.db
```

Open [http://localhost:8080](http://localhost:8080).

## Runtime arguments

- `--listen <addr>`: IPv4 listen address (default `0.0.0.0`)
- `--port <port>`: port (default `8080`)
- `--db <path>`: sqlite database path (default `./mswiki.db`)
- `--max-body-bytes <bytes>`: maximum accepted request body bytes (default `10485760`)
- `--self-test`: run the in-process unit/integration self-test suite

## Test Suite

Run tests with CTest:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Or run directly:

```sh
./mswiki --self-test
```

## Container

Build:

```sh
docker build -t mswiki:local .
```

Run with a Docker named volume (recommended):

```sh
docker volume create mswiki-data
docker run --rm -p 8080:8080 -v mswiki-data:/data mswiki:local
```

Build specifically for amd64 (recommended for deployment to x86_64 servers):

```sh
docker buildx build --platform linux/amd64 -t mswiki:amd64 --load .
```

Run the amd64 image locally:

```sh
docker volume create mswiki-data
docker run --rm --platform linux/amd64 -p 8080:8080 -v mswiki-data:/data mswiki:amd64
```

Build and run the fuzzing image:

```sh
docker buildx build --platform linux/amd64 --target fuzz -t mswiki:fuzz --load .
docker run --rm --platform linux/amd64 mswiki:fuzz
```

The default fuzz target is HTTP request fuzzing and loads
`fuzz/http_request.dict`.
The default run also enforces `-timeout=5` seconds per input to avoid
pathological slow-unit stalls.

The image includes starter seeds in `/corpus/http_request`,
`/corpus/markdown`, and `/corpus/multipart`.

Run HTTP fuzzing with baked-in corpus:

```sh
docker run --rm --platform linux/amd64 mswiki:fuzz
```

Run the markdown renderer fuzz target:

```sh
docker run --rm --platform linux/amd64 \
  --entrypoint /src/fuzz-build/mswiki_markdown_fuzz \
  mswiki:fuzz \
  -dict=/src/fuzz/markdown.dict -max_total_time=3600 /corpus/markdown
```

Run the multipart parser fuzz target:

```sh
docker run --rm --platform linux/amd64 \
  --entrypoint /src/fuzz-build/mswiki_multipart_fuzz \
  mswiki:fuzz \
  -dict=/src/fuzz/multipart.dict -max_total_time=3600 /corpus/multipart
```

If you want corpus persistence across runs, mount a host corpus directory:

```sh
mkdir -p fuzz-corpus
cp -R fuzz/seeds/* fuzz-corpus/
docker run --rm --platform linux/amd64 -v "$PWD/fuzz-corpus:/corpus" mswiki:fuzz
```

If the container exits immediately, inspect logs:

```sh
docker logs <container-id>
```

## Data layout (SQLite)

- `pages`: page slug/title/markdown/timestamps
- `page_links`: extracted wiki links for backlinks
- `images`: image metadata and binary data

This schema keeps markdown and related metadata directly readable via SQLite tooling.
