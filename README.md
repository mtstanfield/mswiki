# mswiki

`mswiki` is a lightweight, SQLite-backed markdown wiki server implemented as a single C++ binary.

## Features

- Markdown page storage in SQLite (`pages.markdown`)
- Wiki-style links with backlink index (`[[Page Name]]` and `[[slug|label]]`)
- Minimal web UI for browse/edit/create
- Image uploads stored in SQLite BLOBs
- Optional runtime branding from assets directory:
  - `/data/assets/style.css`
  - `/data/assets/logo.svg|logo.png|logo.jpg|logo.jpeg|logo.gif`
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
- `--assets <dir>`: optional assets directory
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

Run with a single volume mount for data and custom assets:

```sh
docker run --rm -p 8080:8080 -v "$PWD/data:/data" mswiki:local
```

## Data layout (SQLite)

- `pages`: page slug/title/markdown/timestamps
- `page_links`: extracted wiki links for backlinks
- `images`: image metadata and binary data

This schema keeps markdown and related metadata directly readable via SQLite tooling.
