# syntax=docker/dockerfile:1

FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build -DMSWIKI_STATIC=ON
RUN cmake --build build --config Release -j"$(nproc)"

FROM gcr.io/distroless/static-debian12:nonroot
WORKDIR /app
COPY --from=build /src/build/mswiki /app/mswiki
VOLUME ["/data"]
EXPOSE 8080
ENTRYPOINT ["/app/mswiki", "--listen", "0.0.0.0", "--port", "8080", "--db", "/data/mswiki.db", "--assets", "/data/assets"]
