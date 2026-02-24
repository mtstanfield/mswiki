#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#define main mswiki_main
#include "main.cpp"
#undef main

namespace {

void DrainSocket(int fd) {
  unsigned char buffer[4096];
  while (true) {
    const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) {
      break;
    }
  }
}

void ExerciseSendResponse(const HttpResponse* response) {
  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return;
  }

  std::thread drainer([&]() { DrainSocket(fds[1]); });

  (void)SendResponse(fds[0], response);
  (void)shutdown(fds[0], SHUT_WR);
  drainer.join();

  (void)close(fds[0]);
  (void)close(fds[1]);
}

void ExerciseParsedRequest(sqlite3* db,
                           RequestArena* arena,
                           unsigned char* raw,
                           size_t rawLen) {
  HttpRequest request;
  const unsigned char* bodyStart = nullptr;
  size_t bodyOffset = 0U;
  if (!ParseHttpRequest(raw, rawLen, &request, &bodyStart, &bodyOffset)) {
    return;
  }

  // Exercise body decoders independently of route acceptance.
  char title[MAX_TITLE];
  (void)ParseFormField(&request, "title", title, sizeof(title));
  MultipartImage image;
  ParseMultipartImage(&request, &image);

  HttpResponse response;
  HandleRequest(db, arena, &request, &response);
  if (response.body != nullptr && response.bodyLen > 0U) {
    volatile unsigned char sink = response.body[0];
    (void)sink;
  }
  ExerciseSendResponse(&response);
}

void ExerciseSocketReadPath(sqlite3* db,
                            RequestArena* arena,
                            const uint8_t* data,
                            size_t size) {
  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return;
  }

  size_t written = 0U;
  while (written < size) {
    const size_t stepSeed = static_cast<size_t>(data[written % size]);
    size_t chunk = 1U + (stepSeed % 64U);
    if (chunk > size - written) {
      chunk = size - written;
    }
    const ssize_t n = send(fds[0], data + written, chunk, MSG_NOSIGNAL);
    if (n <= 0) {
      break;
    }
    written += static_cast<size_t>(n);
  }
  (void)shutdown(fds[0], SHUT_WR);

  static unsigned char socketBuffer[MAX_REQUEST_BYTES];
  size_t rawLen = 0U;
  bool tooLarge = false;
  if (ReadRequestFromSocket(fds[1], MAX_BODY_BYTES, socketBuffer, &rawLen,
                            &tooLarge)) {
    ExerciseParsedRequest(db, arena, socketBuffer, rawLen);
  }

  (void)close(fds[0]);
  (void)close(fds[1]);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (data == nullptr || size < 3U || size > MAX_REQUEST_BYTES) {
    return 0;
  }

  sqlite3* db = nullptr;
  char err[256];
  if (!DbOpen(&db, ":memory:", err, sizeof(err))) {
    return 0;
  }

  const size_t requestCount = 1U + static_cast<size_t>(data[0] % 4U);
  size_t offset = 1U;
  RequestArena arena;
  for (size_t i = 0U; i < requestCount && offset + 2U <= size; i++) {
    size_t chunkLen = static_cast<size_t>(data[offset]) |
                      (static_cast<size_t>(data[offset + 1U]) << 8U);
    offset += 2U;
    if (chunkLen > MAX_REQUEST_BYTES) {
      chunkLen = MAX_REQUEST_BYTES;
    }
    if (offset + chunkLen > size) {
      chunkLen = size - offset;
    }
    if (chunkLen > MAX_REQUEST_BYTES) {
      chunkLen = MAX_REQUEST_BYTES;
    }
    if (chunkLen == 0U) {
      continue;
    }

    std::vector<unsigned char> input(chunkLen + 1U);
    (void)std::memcpy(input.data(), data + offset, chunkLen);
    input[chunkLen] = '\0';

    ExerciseParsedRequest(db, &arena, input.data(), chunkLen);
    ExerciseSocketReadPath(db, &arena, data + offset, chunkLen);
    offset += chunkLen;
  }

  (void)sqlite3_close(db);

  return 0;
}
