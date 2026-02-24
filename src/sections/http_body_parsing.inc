/*
 * HTTP body parsing helpers for form-urlencoded and multipart payloads.
 * This section depends on HttpRequest/MultipartImage types and shared
 * utility helpers (FindHeader, CopyString, TrimInPlace, UrlDecodeStrict)
 * provided by main.cpp.
 */

/*
 * Locate the first occurrence of a byte subsequence.
 */
const unsigned char* FindBytes(const unsigned char* haystack,
                               size_t haystackLen,
                               const unsigned char* needle,
                               size_t needleLen) {
  if (needleLen == 0U || haystackLen < needleLen) {
    return nullptr;
  }
  for (size_t i = 0; i + needleLen <= haystackLen; i++) {
    if (std::memcmp(haystack + i, needle, needleLen) == 0) {
      return haystack + i;
    }
  }
  return nullptr;
}

/*
 * Parse multipart/form-data payload and extract first `image` part.
 */
void ParseMultipartImage(const HttpRequest* request, MultipartImage* image) {
  image->valid = false;

  const char* contentType = FindHeader(request, "content-type");
  if (contentType == nullptr) {
    return;
  }

  const char* boundaryPos = std::strstr(contentType, "boundary=");
  if (boundaryPos == nullptr) {
    return;
  }
  boundaryPos += 9;
  while (*boundaryPos == ' ' || *boundaryPos == '\t') {
    boundaryPos++;
  }

  const char* boundaryEnd = boundaryPos;
  if (*boundaryPos == '"') {
    boundaryPos++;
    boundaryEnd = boundaryPos;
    while (*boundaryEnd != '\0' && *boundaryEnd != '"') {
      boundaryEnd++;
    }
    if (*boundaryEnd != '"') {
      return;
    }
  } else {
    while (*boundaryEnd != '\0' && *boundaryEnd != ';' && *boundaryEnd != ' ' &&
           *boundaryEnd != '\t') {
      boundaryEnd++;
    }
  }

  char boundary[128];
  const size_t boundaryLen = static_cast<size_t>(boundaryEnd - boundaryPos);
  if (boundaryLen == 0U || boundaryLen + 1U > sizeof(boundary)) {
    return;
  }
  (void)std::memcpy(boundary, boundaryPos, boundaryLen);
  boundary[boundaryLen] = '\0';

  char delimiter[160];
  if (std::snprintf(delimiter, sizeof(delimiter), "--%s", boundary) < 0) {
    return;
  }

  const unsigned char* body = request->body;
  const size_t bodyLen = request->bodyLen;
  const unsigned char* search = body;
  size_t remaining = bodyLen;

  while (remaining > 0U) {
    const unsigned char* partStart = FindBytes(
        search, remaining, reinterpret_cast<const unsigned char*>(delimiter),
        std::strlen(delimiter));
    if (partStart == nullptr) {
      return;
    }

    const size_t consumed = static_cast<size_t>(partStart - search);
    search += consumed;
    remaining -= consumed;

    if (remaining < std::strlen(delimiter) + 2U) {
      return;
    }
    search += std::strlen(delimiter);
    remaining -= std::strlen(delimiter);

    if (remaining >= 2U && search[0] == '-' && search[1] == '-') {
      return;
    }

    if (remaining < 2U || search[0] != '\r' || search[1] != '\n') {
      return;
    }
    search += 2U;
    remaining -= 2U;

    const unsigned char* headerEnd =
        FindBytes(search, remaining,
                  reinterpret_cast<const unsigned char*>("\r\n\r\n"), 4U);
    if (headerEnd == nullptr) {
      return;
    }

    size_t headerLen = static_cast<size_t>(headerEnd - search);
    if (headerLen >= 1024U) {
      return;
    }

    char headerText[1024];
    (void)std::memcpy(headerText, search, headerLen);
    headerText[headerLen] = '\0';

    search = headerEnd + 4U;
    remaining = bodyLen - static_cast<size_t>(search - body);

    char disposition[512];
    disposition[0] = '\0';
    char partMime[MAX_MIME];
    partMime[0] = '\0';

    char* line = headerText;
    while (line[0] != '\0') {
      char* next = std::strstr(line, "\r\n");
      if (next != nullptr) {
        *next = '\0';
      }

      if (strncasecmp(line, "Content-Disposition:", 20) == 0) {
        (void)CopyString(disposition, sizeof(disposition), line + 20);
        TrimInPlace(disposition);
      } else if (strncasecmp(line, "Content-Type:", 13) == 0) {
        (void)CopyString(partMime, sizeof(partMime), line + 13);
        TrimInPlace(partMime);
      }

      if (next == nullptr) {
        break;
      }
      line = next + 2;
    }

    const bool isImageField =
        std::strstr(disposition, "name=\"image\"") != nullptr;

    const unsigned char* dataEnd = FindBytes(
        search, remaining, reinterpret_cast<const unsigned char*>("\r\n"), 2U);
    if (dataEnd == nullptr) {
      return;
    }

    bool foundPartEnd = false;
    while (!foundPartEnd) {
      const unsigned char* candidate =
          FindBytes(dataEnd, bodyLen - static_cast<size_t>(dataEnd - body),
                    reinterpret_cast<const unsigned char*>("\r\n"), 2U);
      if (candidate == nullptr) {
        return;
      }

      const size_t afterCrLfOffset = static_cast<size_t>(candidate - body) + 2U;
      if (afterCrLfOffset + std::strlen(delimiter) <= bodyLen &&
          std::memcmp(body + afterCrLfOffset, delimiter,
                      std::strlen(delimiter)) == 0) {
        dataEnd = candidate;
        foundPartEnd = true;
        continue;
      }
      dataEnd = candidate + 2U;
    }

    const size_t partDataLen = static_cast<size_t>(dataEnd - search);

    if (isImageField) {
      char filename[MAX_FILENAME];
      filename[0] = '\0';
      const char* fn = std::strstr(disposition, "filename=\"");
      if (fn != nullptr) {
        fn += 10;
        const char* end = std::strchr(fn, '"');
        if (end != nullptr) {
          const size_t len = static_cast<size_t>(end - fn);
          if (len + 1U < sizeof(filename)) {
            (void)std::memcpy(filename, fn, len);
            filename[len] = '\0';
          }
        }
      }
      if (filename[0] == '\0') {
        (void)CopyString(filename, sizeof(filename), "upload.bin");
      }

      (void)CopyString(image->filename, sizeof(image->filename), filename);
      if (partMime[0] == '\0') {
        (void)CopyString(image->mimeType, sizeof(image->mimeType),
                         "application/octet-stream");
      } else {
        (void)CopyString(image->mimeType, sizeof(image->mimeType), partMime);
      }
      image->data = search;
      image->dataLen = partDataLen;
      image->valid = (partDataLen > 0U);
      return;
    }

    const size_t nextStartOffset = static_cast<size_t>(dataEnd - body) + 2U;
    if (nextStartOffset >= bodyLen) {
      return;
    }
    search = body + nextStartOffset;
    remaining = bodyLen - nextStartOffset;
  }
}

/*
 * Parse one URL-encoded key-value field from a request body.
 */
bool ParseFormField(const HttpRequest* request,
                    const char* key,
                    char* out,
                    size_t outSize) {
  if (out == nullptr || outSize == 0U) {
    return false;
  }
  out[0] = '\0';
  if (request->bodyLen == 0U || request->body == nullptr) {
    return false;
  }

  char needle[64];
  if (std::snprintf(needle, sizeof(needle), "%s=", key) < 0) {
    return false;
  }

  const char* body = reinterpret_cast<const char*>(request->body);
  size_t pos = 0;
  while (pos < request->bodyLen) {
    size_t end = pos;
    while (end < request->bodyLen && body[end] != '&') {
      end++;
    }

    const size_t tokenLen = end - pos;
    if (tokenLen >= std::strlen(needle) &&
        std::strncmp(body + pos, needle, std::strlen(needle)) == 0) {
      const char* encodedValue = body + pos + std::strlen(needle);
      const size_t encodedLen = tokenLen - std::strlen(needle);
      return UrlDecodeStrict(out, outSize, encodedValue, encodedLen, nullptr);
    }

    if (end >= request->bodyLen) {
      break;
    }
    pos = end + 1U;
  }

  return false;
}
