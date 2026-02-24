#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#define main mswiki_main
#include "main.cpp"
#undef main

namespace {

void AppendString(std::vector<unsigned char>* out, const std::string& text) {
  out->insert(out->end(), text.begin(), text.end());
}

char BoundaryChar(unsigned char v) {
  static const char kChars[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  return kChars[v % (sizeof(kChars) - 1U)];
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0U) {
    return 0;
  }

  const unsigned char flags = data[0];
  size_t cursor = 1U;

  char boundary[33];
  const size_t boundaryLen = 8U + static_cast<size_t>(flags % 16U);
  for (size_t i = 0U; i < boundaryLen; i++) {
    const unsigned char seed = (cursor < size) ? data[cursor++] : 0U;
    boundary[i] = BoundaryChar(seed);
  }
  boundary[boundaryLen] = '\0';

  const bool quotedBoundary = (flags & 0x01U) != 0U;
  const bool includeClosingBoundary = (flags & 0x02U) != 0U;
  const bool wrongFieldName = (flags & 0x04U) != 0U;
  const bool includeMime = (flags & 0x08U) != 0U;
  const bool includeFilename = (flags & 0x10U) != 0U;

  const char* fieldName = wrongFieldName ? "file" : "image";
  const char* filename = includeFilename ? "upload.png" : "";

  std::vector<unsigned char> body;
  AppendString(&body, "--");
  AppendString(&body, std::string(boundary));
  AppendString(&body, "\r\nContent-Disposition: form-data; name=\"");
  AppendString(&body, std::string(fieldName));
  AppendString(&body, "\"");
  if (includeFilename) {
    AppendString(&body, "; filename=\"");
    AppendString(&body, std::string(filename));
    AppendString(&body, "\"");
  }
  AppendString(&body, "\r\n");
  if (includeMime) {
    AppendString(&body, "Content-Type: image/png\r\n");
  }
  AppendString(&body, "\r\n");

  const size_t payloadCap = 4096U;
  size_t payloadLen = (cursor < size) ? (size - cursor) : 0U;
  if (payloadLen > payloadCap) {
    payloadLen = payloadCap;
  }
  body.insert(body.end(), data + cursor, data + cursor + payloadLen);
  AppendString(&body, "\r\n");

  if (includeClosingBoundary) {
    AppendString(&body, "--");
    AppendString(&body, std::string(boundary));
    AppendString(&body, "--\r\n");
  }

  HttpRequest request;
  (void)std::memset(&request, 0, sizeof(request));
  (void)CopyString(request.method, sizeof(request.method), "POST");
  (void)CopyString(request.version, sizeof(request.version), "HTTP/1.1");
  (void)CopyString(request.target, sizeof(request.target), "/images/upload/home");
  (void)CopyString(request.path, sizeof(request.path), "/images/upload/home");
  request.query[0] = '\0';
  request.headerCount = 1U;
  (void)CopyString(request.headers[0].key, sizeof(request.headers[0].key),
                   "content-type");

  char contentType[MAX_HEADER_VALUE];
  if (quotedBoundary) {
    (void)std::snprintf(contentType, sizeof(contentType),
                        "multipart/form-data; boundary=\"%s\"", boundary);
  } else {
    (void)std::snprintf(contentType, sizeof(contentType),
                        "multipart/form-data; boundary=%s", boundary);
  }
  (void)CopyString(request.headers[0].value, sizeof(request.headers[0].value),
                   contentType);

  request.body = body.empty() ? nullptr : body.data();
  request.bodyLen = body.size();

  MultipartImage image;
  ParseMultipartImage(&request, &image);

  sqlite3* db = nullptr;
  char err[256];
  if (DbOpen(&db, ":memory:", err, sizeof(err))) {
    RequestArena arena;
    HttpResponse response;
    HandleRequest(db, &arena, &request, &response);
    if (response.body != nullptr && response.bodyLen > 0U) {
      volatile unsigned char sink = response.body[0];
      (void)sink;
    }
    (void)sqlite3_close(db);
  }

  if (image.valid && image.data != nullptr && image.dataLen > 0U) {
    volatile unsigned char sink = image.data[0];
    (void)sink;
  }

  return 0;
}
