# syntax=docker/dockerfile:1

FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    clang \
    cmake \
    cppcheck \
    libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build -DMSWIKI_STATIC=ON -DCMAKE_CXX_COMPILER=clang++ -DMSWIKI_WARNINGS_AS_ERRORS=ON -DMSWIKI_STRICT_CLANG=ON
RUN cmake --build build --config Release -j"$(nproc)"
RUN ctest --test-dir build --output-on-failure
RUN ./build/mswiki --self-test
RUN cppcheck --enable=warning,style,performance,portability --std=c++11 --inline-suppr src/main.cpp
RUN mkdir -p /runtime/data && chown -R 65532:65532 /runtime/data

FROM debian:bookworm-slim AS fuzz
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    clang-14 \
    cmake \
    llvm-14 \
    libclang-rt-14-dev \
    libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B fuzz-build -DMSWIKI_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++-14
RUN cmake --build fuzz-build --target mswiki_http_fuzz mswiki_markdown_fuzz mswiki_multipart_fuzz -j"$(nproc)"
RUN mkdir -p /corpus/http_request /corpus/markdown /corpus/multipart \
    && cp -f /src/fuzz/seeds/http_request/*.bin /corpus/http_request/ \
    && cp -f /src/fuzz/seeds/markdown/* /corpus/markdown/ \
    && cp -f /src/fuzz/seeds/multipart/*.bin /corpus/multipart/
ENV ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:symbolize=1
ENV ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-14
ENV UBSAN_OPTIONS=print_stacktrace=1
ENTRYPOINT ["/src/fuzz-build/mswiki_http_fuzz", "-dict=/src/fuzz/http_request.dict", "-timeout=5", "-max_total_time=60", "/corpus/http_request"]

FROM gcr.io/distroless/static-debian12:nonroot
WORKDIR /app
USER 65532:65532
COPY --from=build --chown=65532:65532 /src/build/mswiki /app/mswiki
COPY --from=build --chown=65532:65532 /runtime/data /data
VOLUME ["/data"]
EXPOSE 8080
ENTRYPOINT ["/app/mswiki", "--listen", "0.0.0.0", "--port", "8080", "--db", "/data/mswiki.db"]
