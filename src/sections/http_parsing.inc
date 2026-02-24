/*
 * HTTP request parsing helpers.
 * This section depends on HttpRequest/Header layout, buffer limits,
 * and shared helpers (CopyString, LowerString, UrlDecodeStrict, IsSafeSlug)
 * declared in main.cpp.
 */

/*
 * Lookup a normalized HTTP header value by lowercase key.
 */
const char* FindHeader(const HttpRequest* request, const char* key) {
  for (size_t i = 0; i < request->headerCount; i++) {
    if (std::strcmp(request->headers[i].key, key) == 0) {
      return request->headers[i].value;
    }
  }
  return nullptr;
}

/*
 * Parse HTTP request line into method, target, version, path, and
 * query.
 */
bool ParseRequestLine(const char* line, HttpRequest* request) {
  int matched = std::sscanf(line, "%7s %1023s %15s", request->method,
                            request->target, request->version);
  if (matched != 3) {
    return false;
  }
  if (std::strcmp(request->version, "HTTP/1.1") != 0 &&
      std::strcmp(request->version, "HTTP/1.0") != 0) {
    return false;
  }
  if (request->target[0] != '/') {
    return false;
  }

  const char* question = std::strchr(request->target, '?');
  if (question == nullptr) {
    if (!CopyString(request->path, sizeof(request->path), request->target)) {
      return false;
    }
    request->query[0] = '\0';
  } else {
    const size_t pathLen = static_cast<size_t>(question - request->target);
    if (pathLen + 1U > sizeof(request->path)) {
      return false;
    }
    (void)std::memcpy(request->path, request->target, pathLen);
    request->path[pathLen] = '\0';

    if (!CopyString(request->query, sizeof(request->query), question + 1)) {
      return false;
    }
  }

  return true;
}

/*
 * Decode and validate a route slug/path component without slugify
 * fallback.
 */
bool DecodeRouteSlugStrict(const char* encoded, char* slug, size_t slugSize) {
  size_t decodedLen = 0U;
  if (!UrlDecodeStrict(slug, slugSize, encoded, std::strlen(encoded),
                       &decodedLen)) {
    return false;
  }
  if (decodedLen == 0U) {
    return false;
  }
  if (slug[0] == '\0') {
    return false;
  }
  LowerString(slug);
  return IsSafeSlug(slug);
}

/*
 * Parse a complete HTTP request buffer into HttpRequest fields.
 */
bool ParseHttpRequest(unsigned char* raw,
                      size_t rawLen,
                      HttpRequest* request,
                      const unsigned char** bodyStart,
                      size_t* bodyOffset) {
  request->headerCount = 0;

  size_t headerEnd = 0;
  bool found = false;
  for (size_t i = 0; i + 3U < rawLen; i++) {
    if (raw[i] == '\r' && raw[i + 1U] == '\n' && raw[i + 2U] == '\r' &&
        raw[i + 3U] == '\n') {
      headerEnd = i;
      found = true;
      break;
    }
  }
  if (!found) {
    return false;
  }
  for (size_t i = 0; i < headerEnd; i++) {
    if (raw[i] == '\0') {
      return false;
    }
  }

  raw[headerEnd] = '\0';
  char* cursor = reinterpret_cast<char*>(raw);
  char* lineEnd = std::strstr(cursor, "\r\n");
  if (lineEnd == nullptr) {
    return false;
  }
  *lineEnd = '\0';
  if (!ParseRequestLine(cursor, request)) {
    return false;
  }

  cursor = lineEnd + 2;
  while (*cursor != '\0') {
    lineEnd = std::strstr(cursor, "\r\n");
    if (lineEnd != nullptr) {
      *lineEnd = '\0';
    }

    char* colon = std::strchr(cursor, ':');
    if (colon == nullptr || colon == cursor) {
      return false;
    }
    for (const char* it = cursor; it < colon; ++it) {
      if (!IsHeaderTokenChar(static_cast<unsigned char>(*it))) {
        return false;
      }
    }
    *colon = '\0';
    if (request->headerCount >= MAX_HEADERS) {
      return false;
    }
    if (!CopyString(request->headers[request->headerCount].key,
                    sizeof(request->headers[request->headerCount].key),
                    cursor)) {
      return false;
    }
    LowerString(request->headers[request->headerCount].key);

    if (!CopyString(request->headers[request->headerCount].value,
                    sizeof(request->headers[request->headerCount].value),
                    colon + 1)) {
      return false;
    }
    TrimInPlace(request->headers[request->headerCount].value);
    request->headerCount++;

    if (lineEnd == nullptr) {
      break;
    }
    cursor = lineEnd + 2;
  }

  if (std::strcmp(request->version, "HTTP/1.1") == 0) {
    const char* host = FindHeader(request, "host");
    if (host == nullptr || host[0] == '\0') {
      return false;
    }
  }

  *bodyOffset = headerEnd + 4U;
  if (*bodyOffset > rawLen) {
    return false;
  }
  request->body = raw + *bodyOffset;
  request->bodyLen = rawLen - *bodyOffset;
  *bodyStart = request->body;
  return true;
}

/*
 * Read one HTTP request from a socket with strict framing checks.
 */
bool ReadRequestFromSocket(int clientFd,
                           size_t maxBodyBytes,
                           unsigned char* out,
                           size_t* outLen,
                           bool* tooLarge) {
  *outLen = 0;
  *tooLarge = false;

  size_t headerEnd = 0;
  bool headerFound = false;
  while (!headerFound) {
    if (*outLen >= MAX_REQUEST_BYTES) {
      return false;
    }
    const ssize_t n =
        recv(clientFd, out + *outLen, MAX_REQUEST_BYTES - *outLen, 0);
    if (n <= 0) {
      return false;
    }
    *outLen += static_cast<size_t>(n);

    for (size_t i = 0; i + 3U < *outLen; i++) {
      if (out[i] == '\r' && out[i + 1U] == '\n' && out[i + 2U] == '\r' &&
          out[i + 3U] == '\n') {
        headerEnd = i;
        headerFound = true;
        break;
      }
    }

    if (!headerFound && *outLen > 64U * 1024U) {
      return false;
    }
  }

  HttpRequest parsed;
  unsigned char headerCopy[64U * 1024U + 4U];
  const size_t headerBytes = headerEnd + 4U;
  if (headerBytes > sizeof(headerCopy)) {
    return false;
  }
  (void)std::memcpy(headerCopy, out, headerBytes);
  const unsigned char* bodyStart = nullptr;
  size_t bodyOffset = 0U;
  if (!ParseHttpRequest(headerCopy, headerBytes, &parsed, &bodyStart,
                        &bodyOffset)) {
    return false;
  }

  size_t contentLength = 0U;
  bool hasContentLength = false;
  for (size_t i = 0; i < parsed.headerCount; i++) {
    if (std::strcmp(parsed.headers[i].key, "transfer-encoding") == 0) {
      return false;
    }
    if (std::strcmp(parsed.headers[i].key, "content-length") == 0) {
      size_t candidate = 0U;
      if (!ParseUnsignedSizeStrict(parsed.headers[i].value, &candidate)) {
        return false;
      }
      if (!hasContentLength) {
        contentLength = candidate;
        hasContentLength = true;
      } else if (contentLength != candidate) {
        return false;
      }
    }
  }

  if (std::strcmp(parsed.method, "POST") == 0 && !hasContentLength) {
    return false;
  }

  if (hasContentLength && contentLength > maxBodyBytes) {
    *tooLarge = true;
    return false;
  }

  const size_t needed = headerEnd + 4U + contentLength;
  while (*outLen < needed) {
    if (*outLen >= MAX_REQUEST_BYTES) {
      return false;
    }
    const ssize_t n =
        recv(clientFd, out + *outLen, MAX_REQUEST_BYTES - *outLen, 0);
    if (n <= 0) {
      return false;
    }
    *outLen += static_cast<size_t>(n);
  }

  if (hasContentLength && *outLen > needed) {
    *outLen = needed;
  }

  return true;
}
