#include <arpa/inet.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

static const int DEFAULT_PORT = 8080;
static const int LISTEN_BACKLOG = 16;
static const int CLIENT_SOCKET_TIMEOUT_SEC = 15;
static const size_t MAX_HEADERS = 40;
static const size_t MAX_HEADER_KEY = 64;
static const size_t MAX_HEADER_VALUE = 512;
static const size_t MAX_METHOD = 8;
static const size_t MAX_PATH = 512;
static const size_t MAX_TARGET = 1024;
static const size_t MAX_SLUG = 256;
static const size_t MAX_TITLE = 256;
static const size_t MAX_TIMESTAMP = 32;
static const size_t MAX_MIME = 128;
static const size_t MAX_FILENAME = 256;
static const size_t MAX_BODY_BYTES = 10U * 1024U * 1024U;
static const size_t MAX_REQUEST_BYTES = MAX_BODY_BYTES + 64U * 1024U;
static const size_t MAX_RESPONSE_BYTES = 1U * 1024U * 1024U;
static const size_t MAX_REQUEST_ARENA_BYTES = 4U * MAX_RESPONSE_BYTES;
static const size_t MAX_BINARY_RESPONSE_BYTES = 10U * 1024U * 1024U;
static const size_t MAX_PAGE_MARKDOWN = 512U * 1024U;
static const size_t MAX_FOOTNOTES = 64U;
static const size_t MAX_FOOTNOTE_ID = 32U;
static const size_t MAX_FOOTNOTE_TEXT = 1024U;
static const size_t MAX_SEARCH_QUERY = 256U;
static const size_t MAX_SEARCH_TOKEN = 32U;
static const size_t MAX_SEARCH_TOKENS = 16U;
static const size_t MAX_SEARCH_SNIPPET = 240U;
static const int MAX_SEARCH_RESULTS = 20;

typedef struct {
  char listenHost[64];
  int port;
  char dbPath[512];
  size_t maxBodyBytes;
  bool runSelfTest;
} Config;

typedef struct {
  char key[MAX_HEADER_KEY];
  char value[MAX_HEADER_VALUE];
} Header;

typedef struct {
  char method[MAX_METHOD];
  char version[16];
  char target[MAX_TARGET];
  char path[MAX_PATH];
  char query[MAX_PATH];
  Header headers[MAX_HEADERS];
  size_t headerCount;
  const unsigned char* body;
  size_t bodyLen;
} HttpRequest;

typedef struct {
  int status;
  char contentType[MAX_MIME];
  const unsigned char* body;
  size_t bodyLen;
  char location[512];
  bool hasLocation;
} HttpResponse;

typedef struct {
  char slug[MAX_SLUG];
  char title[MAX_TITLE];
  char markdown[MAX_PAGE_MARKDOWN];
  char createdAt[MAX_TIMESTAMP];
  char updatedAt[MAX_TIMESTAMP];
} PageRecord;

typedef struct {
  char* data;
  size_t length;
  size_t capacity;
} TextBuffer;

typedef struct {
  unsigned char* storage;
  size_t capacity;
  size_t used;
} RequestArena;

typedef struct {
  char filename[MAX_FILENAME];
  char mimeType[MAX_MIME];
  const unsigned char* data;
  size_t dataLen;
  bool valid;
} MultipartImage;

static volatile sig_atomic_t gKeepRunning = 1;
static unsigned char gRequestBuffer[MAX_REQUEST_BYTES];
static unsigned char gResponseBuffer[MAX_RESPONSE_BYTES];
static unsigned char gBinaryBuffer[MAX_BINARY_RESPONSE_BYTES];
static unsigned char gRequestArenaStorage[MAX_REQUEST_ARENA_BYTES];

bool DbOpen(sqlite3** db, const char* dbPath, char* err, size_t errSize);
bool DbGetPage(sqlite3* db,
               const char* slug,
               PageRecord* page,
               bool* found,
               char* err,
               size_t errSize);
bool DbUpsertPage(sqlite3* db,
                  const char* slug,
                  const char* title,
                  const char* markdown,
                  char* err,
                  size_t errSize);
bool DbDeletePage(sqlite3* db, const char* slug, char* err, size_t errSize);
void ExtractWikiLinks(const char* markdown,
                      char links[][MAX_SLUG],
                      size_t* linkCount,
                      size_t maxLinks);
bool DbListPagesHtml(sqlite3* db, TextBuffer* html, char* err, size_t errSize);
bool DbSearchPagesHtml(sqlite3* db,
                       const char* ftsQuery,
                       const char tokens[][MAX_SEARCH_TOKEN],
                       size_t tokenCount,
                       TextBuffer* html,
                       int* resultCount,
                       char* err,
                       size_t errSize);
bool RenderMarkdownToHtml(const char* markdown, TextBuffer* html);
bool DbAppendBacklinksHtml(sqlite3* db,
                           const char* slug,
                           TextBuffer* html,
                           char* err,
                           size_t errSize);
bool DbAppendImagesHtml(sqlite3* db,
                        const char* slug,
                        TextBuffer* html,
                        bool includeInsertButtons,
                        char* err,
                        size_t errSize);
bool DbAppendDocumentsHtml(sqlite3* db,
                           const char* slug,
                           TextBuffer* html,
                           bool includeInsertButtons,
                           char* err,
                           size_t errSize);
bool DbInsertImage(sqlite3* db,
                   const char* pageSlug,
                   const char* filename,
                   const char* mimeType,
                   const unsigned char* data,
                   size_t dataLen,
                   int* imageId,
                   char* err,
                   size_t errSize);
bool DbInsertDocument(sqlite3* db,
                      const char* pageSlug,
                      const char* filename,
                      const char* mimeType,
                      const unsigned char* data,
                      size_t dataLen,
                      int* documentId,
                      char* err,
                      size_t errSize);
bool DbGetImage(sqlite3* db,
                int id,
                char* mimeType,
                size_t mimeTypeSize,
                const unsigned char** data,
                int* dataLen,
                sqlite3_stmt** stmtOut,
                bool* found,
                char* err,
                size_t errSize);
bool DbGetDocument(sqlite3* db,
                   int id,
                   char* mimeType,
                   size_t mimeTypeSize,
                   const unsigned char** data,
                   int* dataLen,
                   sqlite3_stmt** stmtOut,
                   bool* found,
                   char* err,
                   size_t errSize);
bool DbDeleteImage(sqlite3* db,
                   int id,
                   const char* pageSlug,
                   char* err,
                   size_t errSize);
bool DbDeleteDocument(sqlite3* db,
                      int id,
                      const char* pageSlug,
                      char* err,
                      size_t errSize);
bool ParseFormField(const HttpRequest* request,
                    const char* key,
                    char* out,
                    size_t outSize);
void ParseMultipartImage(const HttpRequest* request, MultipartImage* image);
void ParseMultipartDocument(const HttpRequest* request, MultipartImage* document);
bool ParseRequestLine(const char* line, HttpRequest* request);
bool ParseHttpRequest(unsigned char* raw,
                      size_t rawLen,
                      HttpRequest* request,
                      const unsigned char** bodyStart,
                      size_t* bodyOffset);
bool DecodeRouteSlugStrict(const char* encoded, char* slug, size_t slugSize);
bool ReadRequestFromSocket(int clientFd,
                           size_t maxBodyBytes,
                           unsigned char* out,
                           size_t* outLen,
                           bool* tooLarge);
const unsigned char* FindBytes(const unsigned char* haystack,
                               size_t haystackLen,
                               const unsigned char* needle,
                               size_t needleLen);
const char* DetectImageMimeFromData(const unsigned char* data, size_t len);
const char* DetectDocumentMimeFromData(const unsigned char* data, size_t len);
void HandleRequest(sqlite3* db,
                   RequestArena* arena,
                   const HttpRequest* request,
                   HttpResponse* response);

/*
 * Handle termination signals by requesting a clean server shutdown.
 */
void SignalHandler(int) {
  gKeepRunning = 0;
}

/*
 * Initialize or reset a request arena over caller-provided storage.
 */
void ArenaReset(RequestArena* arena, unsigned char* storage, size_t capacity) {
  arena->storage = storage;
  arena->capacity = capacity;
  arena->used = 0U;
}

/*
 * Allocate one TextBuffer slice from the request arena.
 */
bool ArenaAllocTextBuffer(RequestArena* arena,
                          TextBuffer* buffer,
                          size_t capacity) {
  if (arena == nullptr || buffer == nullptr || arena->storage == nullptr) {
    return false;
  }
  if (capacity == 0U || capacity > arena->capacity - arena->used) {
    return false;
  }
  buffer->data = reinterpret_cast<char*>(arena->storage + arena->used);
  buffer->capacity = capacity;
  buffer->length = 0U;
  buffer->data[0] = '\0';
  arena->used += capacity;
  return true;
}

/*
 * Reset a TextBuffer to an empty C string.
 */
void BufferReset(TextBuffer* buffer) {
  buffer->length = 0;
  if (buffer->capacity > 0U && buffer->data != nullptr) {
    buffer->data[0] = '\0';
  }
}

/*
 * Append raw bytes to a TextBuffer while preserving null termination.
 */
bool BufferAppendBytes(TextBuffer* buffer, const char* text, size_t len) {
  if (buffer->length + len + 1U > buffer->capacity) {
    return false;
  }
  (void)std::memcpy(buffer->data + buffer->length, text, len);
  buffer->length += len;
  buffer->data[buffer->length] = '\0';
  return true;
}

/*
 * Append a null-terminated string to a TextBuffer.
 */
bool BufferAppend(TextBuffer* buffer, const char* text) {
  return BufferAppendBytes(buffer, text, std::strlen(text));
}

/*
 * Append one character to a TextBuffer.
 */
bool BufferAppendChar(TextBuffer* buffer, char ch) {
  if (buffer->length + 2U > buffer->capacity) {
    return false;
  }
  buffer->data[buffer->length] = ch;
  buffer->length += 1U;
  buffer->data[buffer->length] = '\0';
  return true;
}

/*
 * Append formatted text to a TextBuffer via `vsnprintf`.
 */
bool BufferAppendFormat(TextBuffer* buffer, const char* format, ...) {
  va_list args;
  va_start(args, format);
  const size_t remaining = buffer->capacity - buffer->length;
  const int count =
      std::vsnprintf(buffer->data + buffer->length, remaining, format, args);
  va_end(args);
  if (count < 0) {
    return false;
  }
  if (static_cast<size_t>(count) >= remaining) {
    return false;
  }
  buffer->length += static_cast<size_t>(count);
  return true;
}

/*
 * Append HTML-escaped text into a TextBuffer.
 */
bool BufferAppendEscaped(TextBuffer* buffer, const char* text, size_t len) {
  for (size_t i = 0; i < len; i++) {
    const char ch = text[i];
    if (ch == '&') {
      if (!BufferAppend(buffer, "&amp;")) {
        return false;
      }
    } else if (ch == '<') {
      if (!BufferAppend(buffer, "&lt;")) {
        return false;
      }
    } else if (ch == '>') {
      if (!BufferAppend(buffer, "&gt;")) {
        return false;
      }
    } else if (ch == '"') {
      if (!BufferAppend(buffer, "&quot;")) {
        return false;
      }
    } else if (ch == '\'') {
      if (!BufferAppend(buffer, "&#39;")) {
        return false;
      }
    } else {
      if (!BufferAppendChar(buffer, ch)) {
        return false;
      }
    }
  }
  return true;
}

/*
 * Convert one ASCII uppercase letter to lowercase.
 */
int ToLowerAscii(int ch) {
  if (ch >= 'A' && ch <= 'Z') {
    return ch - 'A' + 'a';
  }
  return ch;
}

/*
 * Determine whether a character is a hexadecimal digit.
 */
bool IsHexDigitAscii(int ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
         (ch >= 'A' && ch <= 'F');
}

/*
 * Convert a hexadecimal digit character to its numeric value.
 */
int HexDigitValue(int ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  return ch - 'A' + 10;
}

/*
 * Validate HTTP header-name token character per RFC tchar set.
 */
bool IsHeaderTokenChar(int ch) {
  if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
      (ch >= '0' && ch <= '9')) {
    return true;
  }
  switch (ch) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

/*
 * Parse a base-10 unsigned integer with strict full-string validation.
 */
bool ParseUnsignedSizeStrict(const char* text, size_t* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  size_t value = 0U;
  size_t i = 0U;
  while (text[i] != '\0') {
    if (text[i] < '0' || text[i] > '9') {
      return false;
    }
    const size_t digit = static_cast<size_t>(text[i] - '0');
    if (value > (SIZE_MAX - digit) / 10U) {
      return false;
    }
    value = value * 10U + digit;
    i++;
  }
  *out = value;
  return true;
}

/*
 * Parse a strictly positive signed integer without suffix garbage.
 */
bool ParsePositiveIntStrict(const char* text, int* out) {
  size_t value = 0U;
  if (!ParseUnsignedSizeStrict(text, &value)) {
    return false;
  }
  if (value == 0U || value > static_cast<size_t>(INT32_MAX)) {
    return false;
  }
  *out = static_cast<int>(value);
  return true;
}

/*
 * Lowercase a mutable ASCII string in place.
 */
void LowerString(char* text) {
  const size_t len = std::strlen(text);
  for (size_t i = 0; i < len; i++) {
    text[i] = static_cast<char>(ToLowerAscii(text[i]));
  }
}

/*
 * Remove leading and trailing ASCII whitespace in place.
 */
void TrimInPlace(char* text) {
  size_t start = 0;
  size_t len = std::strlen(text);
  while (start < len && (text[start] == ' ' || text[start] == '\t' ||
                         text[start] == '\r' || text[start] == '\n')) {
    start++;
  }

  size_t end = len;
  while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                         text[end - 1] == '\r' || text[end - 1] == '\n')) {
    end--;
  }

  if (start > 0) {
    (void)std::memmove(text, text + start, end - start);
  }
  text[end - start] = '\0';
}

/*
 * Safely copy one C string into a fixed-size destination.
 */
bool CopyString(char* dst, size_t dstSize, const char* src) {
  if (dstSize == 0U) {
    return false;
  }
  const size_t len = std::strlen(src);
  if (len + 1U > dstSize) {
    return false;
  }
  (void)std::memcpy(dst, src, len + 1U);
  return true;
}

/*
 * Decode percent-encoded URL text and '+' spaces.
 */
size_t UrlDecode(char* out, size_t outSize, const char* in, size_t inLen) {
  if (outSize == 0U) {
    return 0U;
  }
  size_t outLen = 0;
  for (size_t i = 0; i < inLen; i++) {
    if (outLen + 1U >= outSize) {
      break;
    }
    if (in[i] == '%' && i + 2U < inLen && IsHexDigitAscii(in[i + 1U]) &&
        IsHexDigitAscii(in[i + 2U])) {
      const int hi = HexDigitValue(in[i + 1U]);
      const int lo = HexDigitValue(in[i + 2U]);
      const unsigned int decoded = static_cast<unsigned int>((hi << 4) | lo);
      if (decoded == 0U) {
        if (outLen + 3U >= outSize) {
          break;
        }
        out[outLen++] = '%';
        out[outLen++] = in[i + 1U];
        out[outLen++] = in[i + 2U];
        i += 2U;
      } else {
        out[outLen++] = static_cast<char>(decoded);
        i += 2U;
      }
    } else if (in[i] == '+') {
      out[outLen++] = ' ';
    } else {
      out[outLen++] = in[i];
    }
  }
  out[outLen] = '\0';
  return outLen;
}

/*
 * Decode percent-encoded URL text with strict overflow signaling.
 */
bool UrlDecodeStrict(char* out,
                     size_t outSize,
                     const char* in,
                     size_t inLen,
                     size_t* outLen) {
  if (outSize == 0U) {
    if (outLen != nullptr) {
      *outLen = 0U;
    }
    return false;
  }

  size_t decodedLen = 0U;
  for (size_t i = 0; i < inLen; i++) {
    if (decodedLen + 1U >= outSize) {
      out[decodedLen] = '\0';
      if (outLen != nullptr) {
        *outLen = decodedLen;
      }
      return false;
    }

    if (in[i] == '%' && i + 2U < inLen && IsHexDigitAscii(in[i + 1U]) &&
        IsHexDigitAscii(in[i + 2U])) {
      const int hi = HexDigitValue(in[i + 1U]);
      const int lo = HexDigitValue(in[i + 2U]);
      const unsigned int decoded = static_cast<unsigned int>((hi << 4) | lo);
      if (decoded == 0U) {
        out[decodedLen] = '\0';
        if (outLen != nullptr) {
          *outLen = decodedLen;
        }
        return false;
      }
      out[decodedLen++] = static_cast<char>(decoded);
      i += 2U;
      continue;
    }

    out[decodedLen++] = (in[i] == '+') ? ' ' : in[i];
  }

  out[decodedLen] = '\0';
  if (outLen != nullptr) {
    *outLen = decodedLen;
  }
  return true;
}

/*
 * Encode a string for URL-safe usage while preserving common safe
 * characters.
 */
bool UrlEncode(const char* in, char* out, size_t outSize) {
  static const char HEX[] = "0123456789ABCDEF";
  size_t outLen = 0;
  const size_t inLen = std::strlen(in);
  for (size_t i = 0; i < inLen; i++) {
    const unsigned char ch = static_cast<unsigned char>(in[i]);
    const bool safe = ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                       (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                       ch == '.' || ch == '~' || ch == '/');
    if (safe) {
      if (outLen + 2U > outSize) {
        return false;
      }
      out[outLen++] = static_cast<char>(ch);
    } else {
      if (outLen + 4U > outSize) {
        return false;
      }
      out[outLen++] = '%';
      out[outLen++] = HEX[(ch >> 4U) & 0x0F];
      out[outLen++] = HEX[ch & 0x0F];
    }
  }
  if (outLen >= outSize) {
    return false;
  }
  out[outLen] = '\0';
  return true;
}

/*
 * Escape markdown-special characters for link/image label text.
 */
bool AppendMarkdownLabelEscaped(TextBuffer* buffer,
                                const char* text,
                                size_t len) {
  for (size_t i = 0U; i < len; i++) {
    const char ch = text[i];
    if (ch == '\r' || ch == '\n') {
      if (!BufferAppendChar(buffer, ' ')) {
        return false;
      }
      continue;
    }
    const bool needsEscape =
        (ch == '\\' || ch == '[' || ch == ']' || ch == '(' || ch == ')' ||
         ch == '`');
    if (needsEscape) {
      if (!BufferAppendChar(buffer, '\\')) {
        return false;
      }
    }
    if (!BufferAppendChar(buffer, ch)) {
      return false;
    }
  }
  return true;
}

/*
 * Build `![label](/image/<id>)` markdown snippet for editor insertion.
 */
bool BuildImageMarkdownSnippet(const char* filename,
                               int imageId,
                               char* out,
                               size_t outSize) {
  TextBuffer snippet;
  snippet.data = out;
  snippet.length = 0U;
  snippet.capacity = outSize;
  if (outSize == 0U) {
    return false;
  }
  out[0] = '\0';
  if (!BufferAppend(&snippet, "![")) {
    return false;
  }
  if (!AppendMarkdownLabelEscaped(&snippet, filename, std::strlen(filename))) {
    return false;
  }
  if (!BufferAppendFormat(&snippet, "](/image/%d)", imageId)) {
    return false;
  }
  return true;
}

/*
 * Build `[label](/document/<id>)` markdown snippet for editor insertion.
 */
bool BuildDocumentMarkdownSnippet(const char* filename,
                                  int documentId,
                                  char* out,
                                  size_t outSize) {
  TextBuffer snippet;
  snippet.data = out;
  snippet.length = 0U;
  snippet.capacity = outSize;
  if (outSize == 0U) {
    return false;
  }
  out[0] = '\0';
  if (!BufferAppend(&snippet, "[")) {
    return false;
  }
  if (!AppendMarkdownLabelEscaped(&snippet, filename, std::strlen(filename))) {
    return false;
  }
  if (!BufferAppendFormat(&snippet, "](/document/%d)", documentId)) {
    return false;
  }
  return true;
}

/*
 * Convert free-form title text into a normalized page slug.
 */
bool Slugify(const char* input, char* slug, size_t slugSize) {
  size_t slugLen = 0;
  bool prevSep = false;
  const size_t inLen = std::strlen(input);

  for (size_t i = 0; i < inLen; i++) {
    const char ch = input[i];
    const bool alphaNum =
        ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9'));
    const bool sep = (ch == '-' || ch == '_' || ch == '/' || ch == ' ');

    if (alphaNum) {
      if (slugLen + 2U > slugSize) {
        return false;
      }
      slug[slugLen++] = static_cast<char>(ToLowerAscii(ch));
      prevSep = false;
    } else if (sep) {
      if (!prevSep) {
        if (slugLen + 2U > slugSize) {
          return false;
        }
        slug[slugLen++] = (ch == ' ') ? '-' : ch;
        prevSep = true;
      }
    }
  }

  while (slugLen > 0U &&
         (slug[slugLen - 1U] == '-' || slug[slugLen - 1U] == '_' ||
          slug[slugLen - 1U] == '/')) {
    slugLen--;
  }

  size_t start = 0;
  while (start < slugLen &&
         (slug[start] == '-' || slug[start] == '_' || slug[start] == '/')) {
    start++;
  }
  if (start > 0U) {
    (void)std::memmove(slug, slug + start, slugLen - start);
    slugLen -= start;
  }

  if (slugLen == 0U) {
    if (slugSize < 9U) {
      return false;
    }
    (void)std::memcpy(slug, "untitled", 9U);
    return true;
  }

  slug[slugLen] = '\0';
  return true;
}

/*
 * Validate a slug against allowed wiki path character rules.
 */
bool IsSafeSlug(const char* slug) {
  if (slug[0] == '\0') {
    return false;
  }
  if (std::strstr(slug, "..") != nullptr) {
    return false;
  }
  const size_t len = std::strlen(slug);
  for (size_t i = 0; i < len; i++) {
    const char ch = slug[i];
    const bool ok =
        ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '/');
    if (!ok) {
      return false;
    }
  }
  return true;
}

/*
 * Format current UTC time as an ISO-8601 timestamp.
 */
void NowIso8601(char* out, size_t outSize) {
  const std::time_t now = std::time(nullptr);
  struct tm tmUtc;
#if defined(__APPLE__) || defined(__linux__)
  (void)gmtime_r(&now, &tmUtc);
#else
  (void)now;
  (void)std::memset(&tmUtc, 0, sizeof(tmUtc));
#endif
  (void)std::strftime(out, outSize, "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
}

/*
 * Map HTTP status code to a reason phrase string.
 */
const char* StatusText(int status) {
  if (status == 200) {
    return "OK";
  }
  if (status == 302) {
    return "Found";
  }
  if (status == 400) {
    return "Bad Request";
  }
  if (status == 404) {
    return "Not Found";
  }
  if (status == 405) {
    return "Method Not Allowed";
  }
  if (status == 413) {
    return "Payload Too Large";
  }
  if (status == 500) {
    return "Internal Server Error";
  }
  return "Unknown";
}

/*
 * Populate an HttpResponse with status, content type, and body.
 */
void SetResponse(HttpResponse* response,
                 int status,
                 const char* contentType,
                 const unsigned char* body,
                 size_t bodyLen) {
  response->status = status;
  (void)CopyString(response->contentType, sizeof(response->contentType),
                   contentType);
  response->body = body;
  response->bodyLen = bodyLen;
  response->location[0] = '\0';
  response->hasLocation = false;
}

/*
 * Copy a textual body into stable storage before assigning response.
 * This function is intentionally byte-preserving and does not transform
 * the response body.
 */
bool SetResponseCopy(HttpResponse* response,
                     int status,
                     const char* contentType,
                     const char* body,
                     size_t bodyLen) {
  if (bodyLen > sizeof(gResponseBuffer)) {
    return false;
  }
  if (bodyLen > 0U) {
    (void)std::memcpy(gResponseBuffer, body, bodyLen);
  }
  SetResponse(response, status, contentType, gResponseBuffer, bodyLen);
  return true;
}

/*
 * Build a 302 redirect response with a `Location` header value.
 */
void SetRedirect(HttpResponse* response, const char* location) {
  static const char BODY[] = "redirect";
  SetResponse(response, 302, "text/plain; charset=utf-8",
              reinterpret_cast<const unsigned char*>(BODY), sizeof(BODY) - 1U);
  (void)CopyString(response->location, sizeof(response->location), location);
  response->hasLocation = true;
}

typedef enum {
  ARG_PARSE_OK = 0,
  ARG_PARSE_HELP = 1,
  ARG_PARSE_ERROR = 2
} ArgParseResult;

ArgParseResult ParseArgs(int argc, char** argv, Config* config);

typedef struct {
  int passed;
  int failed;
} SelfTestState;

/*
 * Populate response as plain-text error with bounded body copy.
 */
void SetError(HttpResponse* response, int status, const char* message) {
  if (message == nullptr) {
    SetResponse(response, 500, "text/plain; charset=utf-8",
                reinterpret_cast<const unsigned char*>("internal error"), 14U);
    return;
  }
  const size_t len = std::strlen(message);
  if (len >= sizeof(gResponseBuffer)) {
    SetResponse(response, 500, "text/plain; charset=utf-8",
                reinterpret_cast<const unsigned char*>("internal error"), 14U);
    return;
  }
  (void)std::memcpy(gResponseBuffer, message, len);
  SetResponse(response, status, "text/plain; charset=utf-8", gResponseBuffer,
              len);
}

#include "sections/http_parsing.inc"

#include "sections/http_body_parsing.inc"

#include "sections/markdown_rendering.inc"

#include "sections/wiki_link_extraction.inc"

#include "sections/db_open.inc"

#include "sections/db_pages.inc"

#include "sections/db_images.inc"

#include "sections/db_documents.inc"

/*
 * Identify image MIME from binary signatures (magic bytes).
 */
const char* DetectImageMimeFromData(const unsigned char* data, size_t len) {
  if (data == nullptr || len < 4U) {
    return nullptr;
  }
  if (data[0] == 0xFFU && data[1] == 0xD8U && data[2] == 0xFFU) {
    return "image/jpeg";
  }
  if (len >= 8U && data[0] == 0x89U && data[1] == 0x50U && data[2] == 0x4EU &&
      data[3] == 0x47U && data[4] == 0x0DU && data[5] == 0x0AU &&
      data[6] == 0x1AU && data[7] == 0x0AU) {
    return "image/png";
  }
  if (len >= 6U && ((std::memcmp(data, "GIF87a", 6U) == 0) ||
                    (std::memcmp(data, "GIF89a", 6U) == 0))) {
    return "image/gif";
  }
  if (len >= 12U && std::memcmp(data, "RIFF", 4U) == 0 &&
      std::memcmp(data + 8U, "WEBP", 4U) == 0) {
    return "image/webp";
  }
  return nullptr;
}

/*
 * Identify document MIME from binary signatures (currently PDF only).
 */
const char* DetectDocumentMimeFromData(const unsigned char* data, size_t len) {
  if (data == nullptr || len < 5U) {
    return nullptr;
  }
  if (std::memcmp(data, "%PDF-", 5U) == 0) {
    return "application/pdf";
  }
  return nullptr;
}

/*
 * Lightweight HTML output writer with indentation support.
 */
struct HtmlWriter {
  TextBuffer* out;
  int indentLevel;
};

/*
 * Append indentation spaces for the current HTML nesting level.
 */
bool HtmlWriteIndent(HtmlWriter* writer) {
  for (int i = 0; i < writer->indentLevel; i++) {
    if (!BufferAppend(writer->out, "  ")) {
      return false;
    }
  }
  return true;
}

/*
 * Append one complete HTML line at current indentation.
 */
bool HtmlLine(HtmlWriter* writer, const char* line) {
  if (!HtmlWriteIndent(writer)) {
    return false;
  }
  if (!BufferAppend(writer->out, line)) {
    return false;
  }
  return BufferAppend(writer->out, "\n");
}

/*
 * Append opening HTML tag line and increase nesting indentation.
 */
bool HtmlOpen(HtmlWriter* writer, const char* line) {
  if (!HtmlLine(writer, line)) {
    return false;
  }
  writer->indentLevel += 1;
  return true;
}

/*
 * Decrease nesting indentation and append closing HTML tag line.
 */
bool HtmlClose(HtmlWriter* writer, const char* line) {
  if (writer->indentLevel > 0) {
    writer->indentLevel -= 1;
  }
  return HtmlLine(writer, line);
}

/*
 * Return true when a byte-range line contains the given token substring.
 */
bool LineContainsToken(const char* line, size_t lineLen, const char* token) {
  const size_t tokenLen = std::strlen(token);
  if (tokenLen == 0U || tokenLen > lineLen) {
    return false;
  }
  for (size_t i = 0U; i + tokenLen <= lineLen; i++) {
    if (std::memcmp(line + i, token, tokenLen) == 0) {
      return true;
    }
  }
  return false;
}

/*
 * Append content HTML lines under current indentation without mutating
 * raw text blocks (for example textarea contents).
 */
bool HtmlAppendContentPretty(HtmlWriter* writer, const char* content) {
  const size_t len = std::strlen(content);
  size_t start = 0U;
  int contentIndent = 0;
  bool inTextarea = false;
  bool inScript = false;
  while (start < len) {
    size_t end = start;
    while (end < len && content[end] != '\n') {
      end++;
    }
    const char* line = content + start;
    size_t lineLen = end - start;
    size_t left = 0U;
    while (left < lineLen && (line[left] == ' ' || line[left] == '\t')) {
      left++;
    }
    const char* stripped = line + left;
    const bool hasStrippedContent = (left < lineLen);
    const size_t strippedLen = hasStrippedContent ? (lineLen - left) : 0U;
    const bool isTagLine = hasStrippedContent && stripped[0] == '<';
    const bool closesTextarea = inTextarea && isTagLine &&
                                LineContainsToken(stripped, strippedLen,
                                                  "</textarea>");
    const bool closesScript =
        inScript && isTagLine &&
        LineContainsToken(stripped, strippedLen, "</script>");
    const bool closesRawBlock = closesTextarea || closesScript;
    const bool inRawBlock = inTextarea || inScript;

    if (closesRawBlock && contentIndent > 0) {
      contentIndent -= 1;
    }

    if (!inRawBlock && isTagLine && strippedLen > 1U && stripped[1] == '/' &&
        contentIndent > 0) {
      contentIndent -= 1;
    }

    const bool shouldIndent = (!inRawBlock && isTagLine) ||
                              (inScript && !closesScript) || closesRawBlock;
    if (shouldIndent) {
      for (int i = 0; i < writer->indentLevel + contentIndent; i++) {
        if (!BufferAppend(writer->out, "  ")) {
          return false;
        }
      }
    }
    if ((!inRawBlock && isTagLine) || closesRawBlock) {
      line = stripped;
      lineLen = strippedLen;
    }
    if (lineLen > 0U && !BufferAppendBytes(writer->out, line, lineLen)) {
      return false;
    }
    if (!BufferAppend(writer->out, "\n")) {
      return false;
    }

    if (!inRawBlock && isTagLine) {
      const bool isClosing = (lineLen > 1U && line[1] == '/');
      const bool hasInlineClose = LineContainsToken(line, lineLen, "</");
      const bool selfClosed =
          (lineLen >= 2U && line[lineLen - 2U] == '/' && line[lineLen - 1U] == '>');
      const bool isVoid = LineContainsToken(line, lineLen, "<br") ||
                          LineContainsToken(line, lineLen, "<hr") ||
                          LineContainsToken(line, lineLen, "<img") ||
                          LineContainsToken(line, lineLen, "<input") ||
                          LineContainsToken(line, lineLen, "<meta") ||
                          LineContainsToken(line, lineLen, "<link");
      if (!isClosing && !hasInlineClose && !selfClosed && !isVoid) {
        contentIndent += 1;
      }
    }

    if (!inScript && LineContainsToken(line, lineLen, "<textarea") &&
        !LineContainsToken(line, lineLen, "</textarea>")) {
      inTextarea = true;
    }
    if (inTextarea && LineContainsToken(line, lineLen, "</textarea>")) {
      inTextarea = false;
    }
    if (!inTextarea && LineContainsToken(line, lineLen, "<script") &&
        !LineContainsToken(line, lineLen, "</script>")) {
      inScript = true;
    }
    if (inScript && LineContainsToken(line, lineLen, "</script>")) {
      inScript = false;
    }
    if (end == len) {
      break;
    }
    start = end + 1U;
  }
  return true;
}

/*
 * Remove the first `loading="lazy"` marker from rendered markdown HTML
 * so the first in-content figure can be prioritized for LCP.
 *
 * Input: mutable HTML buffer produced by RenderMarkdownToHtml.
 * Output: true on success (including when no lazy marker exists).
 */
bool DisableLazyLoadingForFirstImage(TextBuffer* html) {
  if (html == nullptr || html->data == nullptr) {
    return false;
  }
  static const char TOKEN[] = " loading=\"lazy\"";
  const char* pos = std::strstr(html->data, TOKEN);
  if (pos == nullptr) {
    return true;
  }
  const size_t tokenLen = sizeof(TOKEN) - 1U;
  const size_t index = static_cast<size_t>(pos - html->data);
  if (index + tokenLen > html->length) {
    return false;
  }
  const size_t tailOffset = index + tokenLen;
  const size_t tailLen = html->length - tailOffset;
  (void)std::memmove(html->data + index, html->data + tailOffset, tailLen + 1U);
  html->length -= tokenLen;
  return true;
}

/*
 * Build the common HTML page shell with brand, header search, and nav links
 * wrapped around page content.
 */
bool BuildPageLayout(const char* title,
                     const char* content,
                     const char* headerLinkHref,
                     const char* headerLinkLabel,
                     TextBuffer* out) {
  HtmlWriter writer;
  writer.out = out;
  writer.indentLevel = 0;

  if (!HtmlLine(&writer, "<!doctype html>")) {
    return false;
  }
  if (!HtmlOpen(&writer, "<html lang=\"en\">")) {
    return false;
  }
  if (!HtmlOpen(&writer, "<head>")) {
    return false;
  }
  if (!HtmlLine(&writer, "<meta charset=\"utf-8\">")) {
    return false;
  }
  if (!HtmlLine(&writer,
                "<meta name=\"viewport\" "
                "content=\"width=device-width,initial-scale=1\">")) {
    return false;
  }
  if (!HtmlLine(
          &writer,
          "<meta name=\"description\" content=\"mswiki is a lightweight "
          "markdown wiki for personal and small-team knowledge bases.\">")) {
    return false;
  }
  if (!HtmlWriteIndent(&writer)) {
    return false;
  }
  if (!BufferAppend(out, "<title>")) {
    return false;
  }
  if (!BufferAppendEscaped(out, title, std::strlen(title))) {
    return false;
  }
  if (!BufferAppend(out, " - mswiki</title>\n")) {
    return false;
  }
  if (!HtmlLine(&writer,
                "<link rel=\"icon\" "
                "href=\"data:image/svg+xml,%3Csvg%20xmlns='http://www.w3.org/"
                "2000/svg'%20viewBox='0%200%20"
                "100%20100'%3E%3Ctext%20y='.9em'%20font-size='90'%3E%F0"
                "%9F%90%B1%3C/text%3E%3C/svg%3E\">")) {
    return false;
  }
  if (!HtmlLine(&writer,
                "<link rel=\"preload\" href=\"/style.css\" as=\"style\">")) {
    return false;
  }
  if (!HtmlLine(&writer, "<link rel=\"stylesheet\" href=\"/style.css\">")) {
    return false;
  }
  if (!HtmlClose(&writer, "</head>")) {
    return false;
  }
  if (!HtmlOpen(&writer, "<body>")) {
    return false;
  }
  if (!HtmlOpen(&writer, "<header>")) {
    return false;
  }
  if (!HtmlOpen(&writer, "<div class=\"header-inner\">")) {
    return false;
  }
  if (!HtmlWriteIndent(&writer)) {
    return false;
  }
  if (!BufferAppend(out, "<a class=\"brand\" href=\"/page/home\">")) {
    return false;
  }
  if (!BufferAppend(out,
                    "<strong>mswiki</strong></a>\n")) {
    return false;
  }
  if (!HtmlWriteIndent(&writer)) {
    return false;
  }
  if (!BufferAppend(out,
                    "<form class=\"header-search\" method=\"get\" "
                    "action=\"/search\">")) {
    return false;
  }
  if (!BufferAppend(
          out,
          "<input type=\"search\" name=\"q\" minlength=\"3\" maxlength=\"256\" "
          "placeholder=\"Search pages\" aria-label=\"Search pages\">")) {
    return false;
  }
  if (!BufferAppend(out, "<button type=\"submit\">Search</button></form>\n")) {
    return false;
  }
  if (!HtmlOpen(&writer, "<nav>")) {
    return false;
  }
  if (!HtmlLine(&writer, "<a href=\"/page/home\">Home</a>")) {
    return false;
  }
  if (!HtmlLine(&writer, "<a href=\"/pages\">All Pages</a>")) {
    return false;
  }
  if (headerLinkHref != nullptr && headerLinkLabel != nullptr) {
    if (!HtmlWriteIndent(&writer)) {
      return false;
    }
    if (!BufferAppend(out, "<a href=\"")) {
      return false;
    }
    if (!BufferAppend(out, headerLinkHref)) {
      return false;
    }
    if (!BufferAppend(out, "\">")) {
      return false;
    }
    if (!BufferAppendEscaped(out, headerLinkLabel, std::strlen(headerLinkLabel))) {
      return false;
    }
    if (!BufferAppend(out, "</a>\n")) {
      return false;
    }
  }
  if (!HtmlClose(&writer, "</nav>")) {
    return false;
  }
  if (!HtmlClose(&writer, "</div>")) {
    return false;
  }
  if (!HtmlClose(&writer, "</header>")) {
    return false;
  }
  if (!HtmlOpen(&writer, "<main>")) {
    return false;
  }
  if (!HtmlAppendContentPretty(&writer, content)) {
    return false;
  }
  if (!HtmlClose(&writer, "</main>")) {
    return false;
  }
  if (!HtmlClose(&writer, "</body>")) {
    return false;
  }
  if (!HtmlClose(&writer, "</html>")) {
    return false;
  }

  return true;
}

/*
 * Populate stylesheet buffer with built-in Tufte-inspired theme CSS.
 */
void BuildDefaultCss(TextBuffer* css) {
  (void)BufferAppend(
      css,
      ":root{--bg:#f8f5ee;--surface:#fffdf9;--text:#1e1c1a;--muted:#6b6259;"
      "--line:#ded5c7;--accent:#1f5d78;--accent-hover:#17465a;--paper:#fffcf7;}"
      "*{box-sizing:border-box;}"
      "body{margin:0;padding:0;background:linear-gradient(180deg,#fdfaf4 "
      "0%,#f4ede2 100%);"
      "color:var(--text);font-family:\"ETBembo\",\"Iowan Old Style\","
      "\"Palatino Linotype\",Palatino,serif;line-height:1.62;}"
      "header{border-bottom:1px solid var(--line);background:var(--paper);}"
      ".header-inner{max-width:980px;margin:0 auto;padding:12px "
      "16px;display:flex;"
      "flex-wrap:wrap;align-items:center;justify-content:space-between;gap:"
      "12px;}"
      ".brand{display:flex;align-items:center;gap:10px;text-decoration:none;"
      "color:var(--text);}"
      ".brand img{max-height:36px;width:auto;}"
      "nav a{margin-right:12px;color:var(--accent);text-decoration:none;"
      "font-size:.95rem;}"
      "nav a:hover{text-decoration:underline;}"
      ".header-search{display:flex;align-items:center;gap:8px;margin:0;}"
      ".header-search input[type=\"search\"]{width:220px;max-width:46vw;"
      "min-height:34px;padding:6px 10px;border:1px solid var(--line);"
      "border-radius:6px;background:#fff;font-size:.92rem;}"
      ".header-search button{min-height:34px;padding:0 12px;border-radius:6px;}"
      "main{max-width:1120px;margin:0 auto;padding:30px 24px 56px;}"
      "article{position:relative;background:var(--paper);border:1px solid "
      "var(--line);border-radius:6px;padding:36px 42% 36px 56px;}"
      ".content-body{max-width:66ch;}"
      "h1,h2,h3,h4{font-weight:500;line-height:1.25;letter-spacing:-.01em;}"
      "p,li{font-size:1.08rem;}"
      ".content-body p{white-space:pre-wrap;}"
      ".content-body li{white-space:normal;}"
      "a{color:var(--accent);}"
      "textarea,input[type=\"text\"]{width:100%;border:1px solid "
      "var(--line);border-radius:4px;padding:10px;font:inherit;background:#fff;"
      "}"
      "textarea{min-height:340px;font-family:\"SF "
      "Mono\",Menlo,Consolas,monospace;}"
      "button{border:0;border-radius:8px;background:var(--accent);color:#fff;"
      "padding:10px 14px;font:inherit;cursor:pointer;}"
      "button:hover{background:var(--accent-hover);}"
      ".form-actions{display:flex;flex-wrap:wrap;gap:10px;align-items:center;"
      "margin:10px 0;}"
      ".action-button,.button-link{display:inline-flex;align-items:center;"
      "justify-content:center;min-height:36px;padding:0 14px;border-radius:8px;"
      "border:1px solid transparent;font:inherit;line-height:1.1;"
      "text-decoration:none;}"
      ".action-button{background:var(--accent);color:#fff;cursor:pointer;}"
      ".action-button:hover{background:var(--accent-hover);}"
      ".button-secondary{background:#fff;color:var(--text);border-color:var("
      "--line);}"
      ".button-secondary:hover{color:var(--accent);border-color:var(--accent);}"
      ".editor-toolbar{display:flex;flex-wrap:wrap;gap:6px;margin:8px 0 10px;}"
      ".md-tool{display:inline-flex;align-items:center;justify-content:center;"
      "min-width:92px;height:34px;padding:0 10px;border:1px solid var(--line);"
      "border-radius:6px;background:#fff;color:var(--text);font-family:\"SF "
      "Mono\",Menlo,Consolas,monospace;font-size:.8rem;line-height:1;"
      "font-weight:600;white-space:nowrap;}"
      ".md-tool:hover{background:#fff;color:var(--accent);border-color:var("
      "--accent);}"
      ".md-tool:focus{outline:2px solid var(--accent);outline-offset:1px;}"
      ".md-insert-image{margin-left:8px;}"
      ".delete-button{background:#8a2f2f;border-color:#8a2f2f;color:#fff;}"
      ".delete-button:hover{background:#6f2525;border-color:#6f2525;}"
      ".inline-delete{display:inline-block;margin:0;}"
      ".meta{color:var(--muted);font-size:.9rem;}"
      ".notice{padding:10px 12px;border-left:4px solid "
      "var(--accent);background:#eef7fb;}"
      ".panel{margin-top:16px;padding-top:12px;border-top:1px solid "
      "var(--line);}"
      "ul.page-list,ul.backlinks,ul.images,ul.documents,ul.search-results"
      "{padding-left:18px;}"
      ".search-page-form{display:flex;flex-wrap:wrap;gap:8px;align-items:center;"
      "margin-bottom:12px;}"
      ".search-page-form input[type=\"search\"]{max-width:560px;}"
      ".content-body li>ul{margin-top:.18rem;margin-bottom:.18rem;}"
      ".content-body ul ul{margin:.12rem 0 .16rem .9rem;padding-left:.95rem;}"
      "pre{background:#f7f3ea;color:var(--text);padding:12px;border-radius:8px;"
      "border:1px solid var(--line);overflow-x:auto;}"
      "code{font-family:\"SF "
      "Mono\",Menlo,Consolas,monospace;background:#eef2ff;padding:0 "
      "4px;border-radius:4px;color:#111827;}"
      "pre code{background:transparent;color:inherit;padding:0;}"
      ".pdf-tag{display:inline-block;margin-left:6px;padding:0 4px;border:1px "
      "solid var(--line);border-radius:4px;font-size:.72rem;color:var(--muted);"
      "background:#f5efe4;vertical-align:baseline;}"
      ".margin-figure,.sidenote{float:right;clear:right;width:32%;margin-right:"
      "-38%;"
      "font-size:.9rem;line-height:1.45;color:var(--muted);}"
      ".margin-figure{margin-top:.25rem;margin-bottom:1.1rem;}"
      ".margin-figure img{width:100%;height:auto;display:block;border:1px "
      "solid var(--line);"
      "box-shadow:0 2px 12px rgba(0,0,0,.08);}"
      ".margin-figure figcaption{margin-top:.3rem;}"
      ".footnote-ref{font-size:.8rem;vertical-align:super;}"
      ".sidenote{display:block;margin-top:.4rem;margin-bottom:.8rem;padding-"
      "left:.8rem;"
      "border-left:2px solid var(--line);}"
      ".sidenote-number{font-weight:600;margin-right:.35rem;}"
      "@media(max-width:1020px){article{padding:24px;} "
      ".content-body{max-width:none;}"
      ".margin-figure,.sidenote{float:none;width:100%;margin:12px "
      "0;padding-left:0;"
      "border-left:0;} .margin-figure img{max-width:100%;}}"
      "@media(max-width:700px){.header-inner{align-items:flex-start;}nav{"
      "display:flex;flex-wrap:wrap;gap:8px;}nav a{margin-right:0;}}");
}

/*
 * Render the "all pages" listing document.
 */
bool BuildPagesHtml(sqlite3* db,
                    RequestArena* arena,
                    TextBuffer* page,
                    char* err,
                    size_t errSize) {
  TextBuffer content;
  if (!ArenaAllocTextBuffer(arena, &content, MAX_RESPONSE_BYTES)) {
    (void)std::snprintf(err, errSize, "response arena exhausted");
    return false;
  }

  if (!BufferAppend(&content,
                    "<article>\n"
                    "<h1>All Pages</h1>\n"
                    "<p class=\"meta\">To create a page, edit the URL to "
                    "<code>/edit/your-page-slug</code> (for example "
                    "<code>/edit/project-notes</code>), then save.</p>\n")) {
    return false;
  }

  TextBuffer items;
  if (!ArenaAllocTextBuffer(arena, &items, MAX_RESPONSE_BYTES)) {
    (void)std::snprintf(err, errSize, "response arena exhausted");
    return false;
  }
  if (!DbListPagesHtml(db, &items, err, errSize)) {
    return false;
  }

  if (items.length == 0U) {
    if (!BufferAppend(&content,
                      "<p class=\"notice\">No pages exist yet. Start with <a "
                      "href=\"/edit/home\">Home</a>.</p>\n")) {
      return false;
    }
  } else {
    if (!BufferAppend(&content, "<ul class=\"page-list\">\n")) {
      return false;
    }
    if (!BufferAppend(&content, items.data)) {
      return false;
    }
    if (!BufferAppend(&content, "</ul>\n")) {
      return false;
    }
  }

  if (!BufferAppend(&content, "</article>\n")) {
    return false;
  }

  BufferReset(page);
  return BuildPageLayout("All Pages", content.data, nullptr, nullptr, page);
}

/*
 * Parse one decoded query-string field from request URI query text.
 */
typedef enum {
  QUERY_FIELD_MISSING = 0,
  QUERY_FIELD_OK = 1,
  QUERY_FIELD_INVALID = 2
} QueryFieldStatus;

QueryFieldStatus ParseQueryField(const HttpRequest* request,
                                 const char* key,
                                 char* out,
                                 size_t outSize) {
  if (out == nullptr || outSize == 0U) {
    return QUERY_FIELD_INVALID;
  }
  out[0] = '\0';
  if (request == nullptr || key == nullptr || key[0] == '\0') {
    return QUERY_FIELD_INVALID;
  }
  const size_t queryLen = std::strlen(request->query);
  if (queryLen == 0U) {
    return QUERY_FIELD_MISSING;
  }

  char needle[64];
  if (std::snprintf(needle, sizeof(needle), "%s=", key) < 0) {
    return QUERY_FIELD_INVALID;
  }

  size_t pos = 0U;
  while (pos < queryLen) {
    size_t end = pos;
    while (end < queryLen && request->query[end] != '&') {
      end++;
    }
    const size_t tokenLen = end - pos;
    if (tokenLen >= std::strlen(needle) &&
        std::strncmp(request->query + pos, needle, std::strlen(needle)) == 0) {
      const char* encodedValue = request->query + pos + std::strlen(needle);
      const size_t encodedLen = tokenLen - std::strlen(needle);
      if (!UrlDecodeStrict(out, outSize, encodedValue, encodedLen, nullptr)) {
        return QUERY_FIELD_INVALID;
      }
      return QUERY_FIELD_OK;
    }
    if (end >= queryLen) {
      break;
    }
    pos = end + 1U;
  }
  return QUERY_FIELD_MISSING;
}

/*
 * Tokenize a simple search query into normalized alphanumeric terms.
 */
size_t ExtractSearchTokens(const char* query,
                           char tokens[][MAX_SEARCH_TOKEN],
                           size_t maxTokens) {
  size_t count = 0U;
  char current[MAX_SEARCH_TOKEN];
  size_t currentLen = 0U;
  const size_t len = std::strlen(query);
  for (size_t i = 0U; i < len; i++) {
    const int ch = static_cast<unsigned char>(query[i]);
    const bool isAlphaNum =
        ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9'));
    if (isAlphaNum) {
      if (currentLen + 1U < sizeof(current)) {
        current[currentLen++] = static_cast<char>(ToLowerAscii(ch));
      }
    } else if (currentLen > 0U) {
      current[currentLen] = '\0';
      if (currentLen >= 3U && count < maxTokens) {
        (void)CopyString(tokens[count], MAX_SEARCH_TOKEN, current);
        count += 1U;
      }
      currentLen = 0U;
    }
  }
  if (currentLen > 0U) {
    current[currentLen] = '\0';
    if (currentLen >= 3U && count < maxTokens) {
      (void)CopyString(tokens[count], MAX_SEARCH_TOKEN, current);
      count += 1U;
    }
  }
  return count;
}

/*
 * Build a safe FTS MATCH expression from normalized search tokens.
 */
bool BuildSimpleSearchExpression(const char tokens[][MAX_SEARCH_TOKEN],
                                 size_t tokenCount,
                                 char* out,
                                 size_t outSize) {
  TextBuffer expr;
  expr.data = out;
  expr.length = 0U;
  expr.capacity = outSize;
  if (outSize == 0U || tokenCount == 0U) {
    return false;
  }
  out[0] = '\0';
  for (size_t i = 0U; i < tokenCount; i++) {
    if (i > 0U) {
      if (!BufferAppendChar(&expr, ' ')) {
        return false;
      }
    }
    if (!BufferAppendChar(&expr, '"')) {
      return false;
    }
    if (!BufferAppend(&expr, tokens[i])) {
      return false;
    }
    if (!BufferAppendChar(&expr, '"')) {
      return false;
    }
  }
  return true;
}

/*
 * Render the search page with inline validation feedback and optional results.
 * Backend query failures are surfaced as user-friendly inline notices.
 */
bool BuildSearchHtml(sqlite3* db,
                     RequestArena* arena,
                     const char* query,
                     const char* ftsQuery,
                     const char tokens[][MAX_SEARCH_TOKEN],
                     size_t tokenCount,
                     const char* feedback,
                     TextBuffer* page,
                     char* err,
                     size_t errSize) {
  TextBuffer content;
  if (!ArenaAllocTextBuffer(arena, &content, MAX_RESPONSE_BYTES)) {
    (void)std::snprintf(err, errSize, "response arena exhausted");
    return false;
  }

  if (!BufferAppend(&content, "<article>\n<h1>Search</h1>\n"
                              "<form class=\"search-page-form\" method=\"get\" "
                              "action=\"/search\">\n"
                              "<input type=\"search\" name=\"q\" minlength=\"3\" "
                              "maxlength=\"256\" placeholder=\"Search pages\" "
                              "value=\"")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, query, std::strlen(query))) {
    return false;
  }
  if (!BufferAppend(&content,
                    "\">\n<button type=\"submit\" class=\"action-button\">"
                    "Search</button>\n</form>\n")) {
    return false;
  }

  if (feedback != nullptr && feedback[0] != '\0') {
    if (!BufferAppend(&content, "<p class=\"notice\">")) {
      return false;
    }
    if (!BufferAppendEscaped(&content, feedback, std::strlen(feedback))) {
      return false;
    }
    if (!BufferAppend(&content, "</p>\n")) {
      return false;
    }
  } else if (ftsQuery != nullptr && tokenCount > 0U) {
    int resultCount = 0;
    const bool queryExecutionFailed =
        !DbSearchPagesHtml(db, ftsQuery, tokens, tokenCount, &content,
                           &resultCount, err, errSize);
    if (queryExecutionFailed) {
      if (!BufferAppend(
              &content,
              "<p class=\"notice\">Search is temporarily unavailable. Please "
              "try again.</p>\n")) {
        return false;
      }
    } else {
      if (resultCount == 0) {
        if (!BufferAppend(
                &content,
                "<p class=\"meta\">No pages matched this search query.</p>\n")) {
          return false;
        }
      } else {
        if (!BufferAppendFormat(&content,
                                "<p class=\"meta\">Showing %d result%s.</p>\n",
                                resultCount, (resultCount == 1) ? "" : "s")) {
          return false;
        }
      }
    }
  } else if (query[0] == '\0') {
    if (!BufferAppend(&content,
                      "<p class=\"meta\">Enter at least 3 characters to search "
                      "page titles, slugs, and markdown.</p>\n")) {
      return false;
    }
  }

  if (!BufferAppend(&content, "</article>\n")) {
    return false;
  }
  BufferReset(page);
  return BuildPageLayout("Search", content.data, nullptr, nullptr, page);
}

/*
 * Append markdown toolbar buttons for editor convenience actions.
 */
bool AppendEditorToolbarHtml(TextBuffer* content) {
  return BufferAppend(
      content,
      "<div id=\"markdown-toolbar\" class=\"editor-toolbar\" role=\"toolbar\" "
      "aria-label=\"Markdown formatting\">\n"
      "<button type=\"button\" class=\"md-tool\" data-md-action=\"h1\" "
      "aria-label=\"Heading 1\">H1 Heading</button>\n"
      "<button type=\"button\" class=\"md-tool\" data-md-action=\"h2\" "
      "aria-label=\"Heading 2\">H2 Heading</button>\n"
      "<button type=\"button\" class=\"md-tool\" data-md-action=\"bold\" "
      "aria-label=\"Bold\">Bold</button>\n"
      "<button type=\"button\" class=\"md-tool\" data-md-action=\"italic\" "
      "aria-label=\"Italic\">Italic</button>\n"
      "<button type=\"button\" class=\"md-tool\" data-md-action=\"link\" "
      "aria-label=\"Link\">Link</button>\n"
      "<button type=\"button\" class=\"md-tool\" data-md-action=\"wiki\" "
      "aria-label=\"Wiki Link\">[[Wiki]]</button>\n"
      "<button type=\"button\" class=\"md-tool\" data-md-action=\"code\" "
      "aria-label=\"Inline Code\">&lt;/&gt; Code</button>\n"
      "<button type=\"button\" class=\"md-tool\" data-md-action=\"codeblock\" "
      "aria-label=\"Code Block\">Code Block</button>\n"
      "<button type=\"button\" class=\"md-tool\" data-md-action=\"list\" "
      "aria-label=\"List\">List</button>\n"
      "<button type=\"button\" class=\"md-tool\" data-md-action=\"footnote\" "
      "aria-label=\"Footnote\">Footnote</button>\n"
      "</div>\n");
}

/*
 * Append strict client-side markdown toolbar behavior script.
 */
bool AppendEditorToolbarScript(TextBuffer* content) {
  return BufferAppend(
      content,
      "<script>\n(function(){'use strict';"
      "function clamp(value,min,max){if(value<min){return min;}if(value>max){"
      "return max;}return value;}"
      "function findActionNode(node,root){while(node&&node!==root){if("
      "node.nodeType===1&&node.getAttribute('data-md-action')){return node;}"
      "node=node.parentNode;}return null;}"
      "function prefixLines(text,prefix){if(text.length===0){return prefix;}"
      "var parts=text.split('\\n');for(var i=0;i<parts.length;i++){parts[i]="
      "prefix+parts[i];}return parts.join('\\n');}"
      "var editor=document.getElementById('markdown');"
      "var toolbar=document.getElementById('markdown-toolbar');"
      "if(!editor||!toolbar){return;}"
      "if(typeof editor.selectionStart!=='number'||typeof "
      "editor.selectionEnd!=='number'){return;}"
      "var actions={"
      "h1:{mode:'line-prefix',prefix:'# ',placeholder:'Heading'},"
      "h2:{mode:'line-prefix',prefix:'## ',placeholder:'Heading'},"
      "bold:{mode:'wrap',prefix:'**',suffix:'**',placeholder:'bold text'},"
      "italic:{mode:'wrap',prefix:'*',suffix:'*',placeholder:'italic text'},"
      "link:{mode:'wrap',prefix:'[',suffix:'](https://example.com)',"
      "placeholder:'link text'},"
      "wiki:{mode:'wrap',prefix:'[[',suffix:']]',placeholder:'Target Page'},"
      "code:{mode:'wrap',prefix:'`',suffix:'`',placeholder:'code'},"
      "codeblock:{mode:'insert',insertText:'```\\ncode block\\n```'},"
      "list:{mode:'line-prefix',prefix:'- ',placeholder:'list item'},"
      "footnote:{mode:'insert',insertText:'[^note]\\n\\n[^note]: note "
      "text'}};"
      "function applyReplacement(start,end,replacement,cursorPos){"
      "if(typeof editor.setRangeText==='function'){editor.setRangeText("
      "replacement,start,end,'end');}else{var before=editor.value.slice(0,"
      "start);var after=editor.value.slice(end);editor.value=before+"
      "replacement+after;editor.selectionStart=start+replacement.length;"
      "editor.selectionEnd=editor.selectionStart;}"
      "if(typeof cursorPos==='number'){var bounded=clamp(cursorPos,0,"
      "editor.value.length);editor.selectionStart=bounded;editor.selectionEnd="
      "bounded;}editor.focus();}"
      "toolbar.addEventListener('click',function(event){var button="
      "findActionNode(event.target,toolbar);if(!button){return;}"
      "event.preventDefault();var actionName=button.getAttribute("
      "'data-md-action');if(!Object.prototype.hasOwnProperty.call(actions,"
      "actionName)){return;}var action=actions[actionName];var start=clamp("
      "editor.selectionStart,0,editor.value.length);var end=clamp("
      "editor.selectionEnd,0,editor.value.length);if(end<start){var tmp=start;"
      "start=end;end=tmp;}var selected=editor.value.slice(start,end);"
      "if(action.mode==='wrap'){var middle=(selected.length>0)?selected:"
      "action.placeholder;var replacement=action.prefix+middle+action.suffix;"
      "var cursor=(selected.length>0)?(start+replacement.length):(start+"
      "action.prefix.length+middle.length);applyReplacement(start,end,"
      "replacement,cursor);return;}"
      "if(action.mode==='line-prefix'){var replacementText=(selected.length>0)?"
      "prefixLines(selected,action.prefix):(action.prefix+action.placeholder);"
      "applyReplacement(start,end,replacementText,start+replacementText.length)"
      ";"
      "return;}"
      "if(action.mode==='insert'){applyReplacement(start,end,action.insertText,"
      "start+action.insertText.length);}});"
      "document.addEventListener('click',function(event){var target="
      "event.target;while(target&&target!==document){if(target.nodeType===1&&"
      "target.classList&&(target.classList.contains('md-insert-image')||"
      "target.classList.contains('md-insert-document'))){"
      "event.preventDefault();var snippet=target.getAttribute("
      "'data-md-snippet');if(typeof snippet!=='string'||snippet.length===0){"
      "return;}var start=clamp(editor.selectionStart,0,editor.value.length);"
      "var end=clamp(editor.selectionEnd,0,editor.value.length);if(end<start){"
      "var tmp=start;start=end;end=tmp;}applyReplacement(start,end,snippet,"
      "start+snippet.length);return;}target=target.parentNode;}});})();\n"
      "</script>\n");
}

/*
 * Render the page editor form for create/update operations.
 */
bool BuildEditHtml(sqlite3* db,
                   RequestArena* arena,
                   const char* slug,
                   const char* title,
                   const char* markdown,
                   TextBuffer* page,
                   char* err,
                   size_t errSize) {
  TextBuffer content;
  if (!ArenaAllocTextBuffer(arena, &content, MAX_RESPONSE_BYTES)) {
    (void)std::snprintf(err, errSize, "response arena exhausted");
    return false;
  }

  char encodedSlug[MAX_PATH];
  if (!UrlEncode(slug, encodedSlug, sizeof(encodedSlug))) {
    return false;
  }

  if (!BufferAppend(&content, "<article>\n<h1>Edit ")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, slug, std::strlen(slug))) {
    return false;
  }
  if (!BufferAppend(&content, "</h1>\n<form method=\"post\" action=\"/save/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(
          &content,
          "\">\n<label for=\"title\">Title</label><br>\n"
          "<input type=\"text\" id=\"title\" name=\"title\" value=\"")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, title, std::strlen(title))) {
    return false;
  }
  if (!BufferAppend(
          &content,
          "\" required><br><br>\n<label for=\"markdown\">Markdown</label><br>\n")) {
    return false;
  }
  if (!AppendEditorToolbarHtml(&content)) {
    return false;
  }
  if (!BufferAppend(&content,
                    "<textarea id=\"markdown\" name=\"markdown\" required>")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, markdown, std::strlen(markdown))) {
    return false;
  }
  if (!BufferAppend(&content, "</textarea>\n")) {
    return false;
  }
  if (!BufferAppend(
          &content,
          "<br>\n<p class=\"meta\">Wiki links: "
          "<code>[[Target Page]]</code> or <code>[[target|label]]</code>.</p>\n"
          "<p class=\"meta\">Footnotes: <code>[^id]</code> in text and "
          "<code>[^id]: note</code> definitions.</p>\n"
          "\n<div class=\"form-actions\">"
          "\n<button type=\"submit\" class=\"action-button\">Save</button>\n"
          "<a class=\"button-link button-secondary\" href=\"/page/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(&content,
                    "\">Cancel</a>\n</div>\n</form>\n"
                    "<form class=\"inline-delete\" method=\"post\" "
                    "onsubmit=\"return confirm('Delete this page and all "
                    "related images and links?');\" "
                    "action=\"/delete/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(&content,
                    "\">\n<div class=\"form-actions\">\n<button type=\"submit\" "
                    "class=\"action-button delete-button\">Delete page</button>\n"
                    "<a class=\"button-link button-secondary\" href=\"/raw/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(&content,
                    "\">View raw markdown</a>\n</div>\n</form>\n<section "
                    "class=\"panel\">\n"
                    "<h2>Images for this page</h2>\n<p><a class=\"button-link "
                    "button-secondary\" href=\"/images/new/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(&content, "\">Upload image</a></p>\n")) {
    return false;
  }
  if (!DbAppendImagesHtml(db, slug, &content, true, err, errSize)) {
    return false;
  }
  if (!BufferAppend(&content, "</section>\n")) {
    return false;
  }
  if (!BufferAppend(&content, "<section class=\"panel\">\n"
                              "<h2>Documents for this page</h2>\n"
                              "<p><a class=\"button-link button-secondary\" "
                              "href=\"/documents/new/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(&content, "\">Upload document</a></p>\n")) {
    return false;
  }
  if (!DbAppendDocumentsHtml(db, slug, &content, true, err, errSize)) {
    return false;
  }
  if (!BufferAppend(&content, "</section>\n")) {
    return false;
  }
  if (!AppendEditorToolbarScript(&content)) {
    return false;
  }
  if (!BufferAppend(&content, "</article>\n")) {
    return false;
  }

  BufferReset(page);
  return BuildPageLayout("Edit", content.data, nullptr, nullptr, page);
}

/*
 * Render placeholder page for a missing slug with create link.
 */
bool BuildMissingPageHtml(RequestArena* arena,
                          const char* slug,
                          TextBuffer* page) {
  TextBuffer content;
  if (!ArenaAllocTextBuffer(arena, &content, MAX_RESPONSE_BYTES)) {
    return false;
  }

  char encodedSlug[MAX_PATH];
  if (!UrlEncode(slug, encodedSlug, sizeof(encodedSlug))) {
    return false;
  }

  if (!BufferAppend(&content, "<article>\n<h1>")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, slug, std::strlen(slug))) {
    return false;
  }
  if (!BufferAppend(&content,
                    "</h1>\n<p class=\"notice\">This page does not exist yet.</p>\n"
                    "<p><a href=\"/edit/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(&content, "\">Create this page</a></p>\n</article>\n")) {
    return false;
  }

  BufferReset(page);
  return BuildPageLayout("Missing Page", content.data, nullptr, nullptr, page);
}

/*
 * Render full page view including metadata, backlinks, images, and documents.
 */
bool BuildViewHtml(sqlite3* db,
                   RequestArena* arena,
                   const PageRecord* pageRecord,
                   TextBuffer* page,
                   char* err,
                   size_t errSize) {
  TextBuffer markdownHtml;
  if (!ArenaAllocTextBuffer(arena, &markdownHtml, MAX_RESPONSE_BYTES)) {
    (void)std::snprintf(err, errSize, "response arena exhausted");
    return false;
  }
  if (!RenderMarkdownToHtml(pageRecord->markdown, &markdownHtml)) {
    (void)std::snprintf(err, errSize, "markdown render overflow");
    return false;
  }
  if (!DisableLazyLoadingForFirstImage(&markdownHtml)) {
    (void)std::snprintf(err, errSize, "image loading attribute update failed");
    return false;
  }

  TextBuffer content;
  if (!ArenaAllocTextBuffer(arena, &content, MAX_RESPONSE_BYTES)) {
    (void)std::snprintf(err, errSize, "response arena exhausted");
    return false;
  }
  char encodedSlug[MAX_PATH];
  if (!UrlEncode(pageRecord->slug, encodedSlug, sizeof(encodedSlug))) {
    (void)std::snprintf(err, errSize, "slug encode failed");
    return false;
  }

  if (!BufferAppend(&content, "<article>\n<h1>")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, pageRecord->title,
                           std::strlen(pageRecord->title))) {
    return false;
  }
  if (!BufferAppend(&content, "</h1>\n<p class=\"meta\">slug: <code>")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, pageRecord->slug,
                           std::strlen(pageRecord->slug))) {
    return false;
  }
  if (!BufferAppend(&content, "</code> | created: ")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, pageRecord->createdAt,
                           std::strlen(pageRecord->createdAt))) {
    return false;
  }
  if (!BufferAppend(&content, " | updated: ")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, pageRecord->updatedAt,
                           std::strlen(pageRecord->updatedAt))) {
    return false;
  }
  if (!BufferAppend(&content, "</p>\n")) {
    return false;
  }

  if (!BufferAppend(&content, "<div class=\"content-body\">\n")) {
    return false;
  }
  if (!BufferAppend(&content, markdownHtml.data)) {
    return false;
  }
  if (!BufferAppend(&content, "</div>\n")) {
    return false;
  }

  if (!BufferAppend(&content, "<section class=\"panel\">\n<h2>Backlinks</h2>\n")) {
    return false;
  }

  if (!DbAppendBacklinksHtml(db, pageRecord->slug, &content, err, errSize)) {
    return false;
  }

  if (!BufferAppend(&content,
                    "</section>\n<section class=\"panel\">\n<h2>Images</h2>\n")) {
    return false;
  }

  if (!DbAppendImagesHtml(db, pageRecord->slug, &content, false, err,
                          errSize)) {
    return false;
  }

  if (!BufferAppend(&content, "</section>\n<section class=\"panel\">\n"
                              "<h2>Documents</h2>\n")) {
    return false;
  }
  if (!DbAppendDocumentsHtml(db, pageRecord->slug, &content, false, err,
                             errSize)) {
    return false;
  }
  if (!BufferAppend(&content, "</section>\n</article>\n")) {
    return false;
  }

  BufferReset(page);
  char editPath[MAX_PATH];
  if (std::snprintf(editPath, sizeof(editPath), "/edit/%s", encodedSlug) < 0) {
    return false;
  }
  return BuildPageLayout(pageRecord->title, content.data, editPath, "Edit page",
                         page);
}

/*
 * Render the image upload form for a page slug.
 */
bool BuildImageUploadForm(RequestArena* arena,
                          const char* slug,
                          TextBuffer* page) {
  TextBuffer content;
  if (!ArenaAllocTextBuffer(arena, &content, MAX_RESPONSE_BYTES)) {
    return false;
  }

  char encodedSlug[MAX_PATH];
  if (!UrlEncode(slug, encodedSlug, sizeof(encodedSlug))) {
    return false;
  }

  if (!BufferAppend(&content, "<article>\n<h1>Upload image for ")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, slug, std::strlen(slug))) {
    return false;
  }
  if (!BufferAppend(
          &content,
          "</h1><form method=\"post\" enctype=\"multipart/form-data\" "
          "action=\"/images/upload/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(
          &content,
          "\">\n<input type=\"file\" name=\"image\" accept=\"image/*\" required>"
          "<br><br>\n<div class=\"form-actions\">"
          "<button type=\"submit\" class=\"action-button\">Upload</button> "
          "<a class=\"button-link button-secondary\" href=\"/page/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(&content, "\">Back</a></div>\n</form>\n</article>\n")) {
    return false;
  }

  BufferReset(page);
  return BuildPageLayout("Upload Image", content.data, nullptr, nullptr, page);
}

/*
 * Render upload-success page showing markdown embed syntax.
 */
bool BuildImageUploadedPage(RequestArena* arena,
                            const char* slug,
                            const char* filename,
                            int imageId,
                            TextBuffer* page) {
  TextBuffer content;
  if (!ArenaAllocTextBuffer(arena, &content, MAX_RESPONSE_BYTES)) {
    return false;
  }

  char encodedSlug[MAX_PATH];
  char markdownSnippet[1024];
  if (!UrlEncode(slug, encodedSlug, sizeof(encodedSlug))) {
    return false;
  }
  if (!BuildImageMarkdownSnippet(filename, imageId, markdownSnippet,
                                 sizeof(markdownSnippet))) {
    return false;
  }

  if (!BufferAppend(&content,
                    "<article>\n<h1>Image uploaded</h1>\n"
                    "<p>Use this markdown in your page:</p>\n"
                    "<pre><code>")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, markdownSnippet,
                           std::strlen(markdownSnippet))) {
    return false;
  }
  if (!BufferAppend(&content, "</code></pre>\n<p><a href=\"/edit/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(&content, "\">Back to edit page</a></p>\n</article>\n")) {
    return false;
  }

  BufferReset(page);
  return BuildPageLayout("Image Uploaded", content.data, nullptr, nullptr, page);
}

/*
 * Render the document upload form for a page slug.
 */
bool BuildDocumentUploadForm(RequestArena* arena,
                             const char* slug,
                             TextBuffer* page) {
  TextBuffer content;
  if (!ArenaAllocTextBuffer(arena, &content, MAX_RESPONSE_BYTES)) {
    return false;
  }

  char encodedSlug[MAX_PATH];
  if (!UrlEncode(slug, encodedSlug, sizeof(encodedSlug))) {
    return false;
  }

  if (!BufferAppend(&content, "<article>\n<h1>Upload document for ")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, slug, std::strlen(slug))) {
    return false;
  }
  if (!BufferAppend(
          &content,
          "</h1><form method=\"post\" enctype=\"multipart/form-data\" "
          "action=\"/documents/upload/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(
          &content,
          "\">\n<input type=\"file\" name=\"document\" "
          "accept=\"application/pdf,.pdf\" required>"
          "<br><br>\n<div class=\"form-actions\">"
          "<button type=\"submit\" class=\"action-button\">Upload</button> "
          "<a class=\"button-link button-secondary\" href=\"/page/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(&content, "\">Back</a></div>\n</form>\n</article>\n")) {
    return false;
  }

  BufferReset(page);
  return BuildPageLayout("Upload Document", content.data, nullptr, nullptr, page);
}

/*
 * Render upload-success page showing markdown link syntax for a PDF.
 */
bool BuildDocumentUploadedPage(RequestArena* arena,
                               const char* slug,
                               const char* filename,
                               int documentId,
                               TextBuffer* page) {
  TextBuffer content;
  if (!ArenaAllocTextBuffer(arena, &content, MAX_RESPONSE_BYTES)) {
    return false;
  }

  char encodedSlug[MAX_PATH];
  char markdownSnippet[1024];
  if (!UrlEncode(slug, encodedSlug, sizeof(encodedSlug))) {
    return false;
  }
  if (!BuildDocumentMarkdownSnippet(filename, documentId, markdownSnippet,
                                    sizeof(markdownSnippet))) {
    return false;
  }

  if (!BufferAppend(&content,
                    "<article>\n<h1>Document uploaded</h1>\n"
                    "<p>Use this markdown in your page:</p>\n"
                    "<pre><code>")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, markdownSnippet,
                           std::strlen(markdownSnippet))) {
    return false;
  }
  if (!BufferAppend(&content, "</code></pre>\n<p><a href=\"/edit/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(&content, "\">Back to edit page</a></p>\n</article>\n")) {
    return false;
  }

  BufferReset(page);
  return BuildPageLayout("Document Uploaded", content.data, nullptr, nullptr,
                         page);
}

/*
 * Build final stylesheet content.
 */
void BuildStyleCss(TextBuffer* css) {
  BufferReset(css);
  BuildDefaultCss(css);
}

/*
 * Execute routing and produce an HTTP response for one request.
 */
void HandleRequest(sqlite3* db,
                   RequestArena* arena,
                   const HttpRequest* request,
                   HttpResponse* response) {
  ArenaReset(arena, gRequestArenaStorage, sizeof(gRequestArenaStorage));

  if (std::strcmp(request->method, "GET") == 0 &&
      std::strcmp(request->path, "/healthz") == 0) {
    static const char BODY[] = "ok";
    SetResponse(response, 200, "text/plain; charset=utf-8",
                reinterpret_cast<const unsigned char*>(BODY),
                sizeof(BODY) - 1U);
    return;
  }

  if (std::strcmp(request->method, "GET") == 0 &&
      std::strcmp(request->path, "/") == 0) {
    SetRedirect(response, "/page/home");
    return;
  }

  if (std::strcmp(request->method, "GET") == 0 &&
      std::strcmp(request->path, "/style.css") == 0) {
    TextBuffer css;
    if (!ArenaAllocTextBuffer(arena, &css, MAX_RESPONSE_BYTES)) {
      SetError(response, 500, "Response arena exhausted");
      return;
    }
    BuildStyleCss(&css);
    if (!SetResponseCopy(response, 200, "text/css; charset=utf-8", css.data,
                         css.length)) {
      SetError(response, 500, "CSS response too large");
    }
    return;
  }

  if (std::strcmp(request->method, "GET") == 0 &&
      std::strcmp(request->path, "/pages") == 0) {
    TextBuffer html;
    char err[256];
    (void)CopyString(err, sizeof(err), "Failed to build pages");
    if (!ArenaAllocTextBuffer(arena, &html, MAX_RESPONSE_BYTES)) {
      SetError(response, 500, "Response arena exhausted");
      return;
    }
    if (!BuildPagesHtml(db, arena, &html, err, sizeof(err))) {
      SetError(response, 500, err);
      return;
    }
    if (!SetResponseCopy(response, 200, "text/html; charset=utf-8", html.data,
                         html.length)) {
      SetError(response, 500, "HTML response too large");
    }
    return;
  }

  if (std::strcmp(request->method, "GET") == 0 &&
      std::strcmp(request->path, "/search") == 0) {
    char query[MAX_SEARCH_QUERY + 1U];
    query[0] = '\0';
    const QueryFieldStatus queryStatus =
        ParseQueryField(request, "q", query, sizeof(query));
    if (queryStatus == QUERY_FIELD_OK) {
      TrimInPlace(query);
    }

    const char* feedback = nullptr;
    char ftsQuery[1024];
    ftsQuery[0] = '\0';
    char tokens[MAX_SEARCH_TOKENS][MAX_SEARCH_TOKEN];
    size_t tokenCount = 0U;

    if (queryStatus == QUERY_FIELD_INVALID) {
      feedback =
          "Invalid search query. Please use plain text with at least 3 "
          "characters.";
    } else if (query[0] != '\0') {
      if (std::strlen(query) < 3U) {
        feedback = "Search query must be at least 3 characters.";
      } else {
        tokenCount = ExtractSearchTokens(query, tokens, MAX_SEARCH_TOKENS);
        if (tokenCount == 0U ||
            !BuildSimpleSearchExpression(tokens, tokenCount, ftsQuery,
                                         sizeof(ftsQuery))) {
          feedback =
              "Search query must include at least one alphanumeric token with "
              "3 or more characters.";
        }
      }
    }

    TextBuffer html;
    if (!ArenaAllocTextBuffer(arena, &html, MAX_RESPONSE_BYTES)) {
      SetError(response, 500, "Response arena exhausted");
      return;
    }
    char err[256];
    (void)CopyString(err, sizeof(err), "Failed to build search page");
    const char* queryForSearch =
        (feedback == nullptr && tokenCount > 0U) ? ftsQuery : nullptr;
    if (!BuildSearchHtml(db, arena, query, queryForSearch, tokens, tokenCount,
                         feedback, &html, err, sizeof(err))) {
      SetError(response, 500, err);
      return;
    }

    if (!SetResponseCopy(response, 200, "text/html; charset=utf-8", html.data,
                         html.length)) {
      SetError(response, 500, "HTML response too large");
    }
    return;
  }

  if (std::strcmp(request->method, "GET") == 0 &&
      std::strncmp(request->path, "/image/", 7) == 0) {
    const char* idText = request->path + 7;
    int imageId = 0;
    if (!ParsePositiveIntStrict(idText, &imageId)) {
      SetError(response, 400, "Invalid image id");
      return;
    }

    char mime[MAX_MIME];
    const unsigned char* blobData = nullptr;
    int blobLen = 0;
    sqlite3_stmt* stmt = nullptr;
    bool found = false;
    char err[256];
    if (!DbGetImage(db, imageId, mime, sizeof(mime), &blobData, &blobLen, &stmt,
                    &found, err, sizeof(err))) {
      SetError(response, 500, err);
      return;
    }
    if (!found) {
      SetError(response, 404, "Image not found");
      return;
    }
    if (blobLen < 0 || static_cast<size_t>(blobLen) > sizeof(gBinaryBuffer)) {
      (void)sqlite3_finalize(stmt);
      SetError(response, 500, "Image too large");
      return;
    }
    (void)std::memcpy(gBinaryBuffer, blobData, static_cast<size_t>(blobLen));
    (void)sqlite3_finalize(stmt);
    const char* detectedMime =
        DetectImageMimeFromData(gBinaryBuffer, static_cast<size_t>(blobLen));
    const char* safeMime =
        (detectedMime == nullptr) ? "application/octet-stream" : detectedMime;
    (void)mime;
    SetResponse(response, 200, safeMime, gBinaryBuffer,
                static_cast<size_t>(blobLen));
    return;
  }

  if (std::strcmp(request->method, "GET") == 0 &&
      std::strncmp(request->path, "/document/", 10) == 0) {
    const char* idText = request->path + 10;
    int documentId = 0;
    if (!ParsePositiveIntStrict(idText, &documentId)) {
      SetError(response, 400, "Invalid document id");
      return;
    }

    char mime[MAX_MIME];
    const unsigned char* blobData = nullptr;
    int blobLen = 0;
    sqlite3_stmt* stmt = nullptr;
    bool found = false;
    char err[256];
    if (!DbGetDocument(db, documentId, mime, sizeof(mime), &blobData, &blobLen,
                       &stmt, &found, err, sizeof(err))) {
      SetError(response, 500, err);
      return;
    }
    if (!found) {
      SetError(response, 404, "Document not found");
      return;
    }
    if (blobLen < 0 || static_cast<size_t>(blobLen) > sizeof(gBinaryBuffer)) {
      (void)sqlite3_finalize(stmt);
      SetError(response, 500, "Document too large");
      return;
    }
    (void)std::memcpy(gBinaryBuffer, blobData, static_cast<size_t>(blobLen));
    (void)sqlite3_finalize(stmt);
    const char* detectedMime =
        DetectDocumentMimeFromData(gBinaryBuffer, static_cast<size_t>(blobLen));
    const char* safeMime =
        (detectedMime == nullptr) ? "application/octet-stream" : detectedMime;
    (void)mime;
    SetResponse(response, 200, safeMime, gBinaryBuffer,
                static_cast<size_t>(blobLen));
    return;
  }

  if (std::strcmp(request->method, "GET") == 0 &&
      std::strncmp(request->path, "/raw/", 5) == 0) {
    char slug[MAX_SLUG];
    if (!DecodeRouteSlugStrict(request->path + 5, slug, sizeof(slug))) {
      SetError(response, 400, "Invalid page slug");
      return;
    }

    PageRecord pageRecord;
    bool found = false;
    char err[256];
    if (!DbGetPage(db, slug, &pageRecord, &found, err, sizeof(err))) {
      SetError(response, 500, err);
      return;
    }
    if (!found) {
      SetError(response, 404, "Page not found");
      return;
    }

    const size_t markdownLen = std::strlen(pageRecord.markdown);
    if (!SetResponseCopy(response, 200, "text/markdown; charset=utf-8",
                         pageRecord.markdown, markdownLen)) {
      SetError(response, 500, "Markdown response too large");
    }
    return;
  }

  if (std::strcmp(request->method, "GET") == 0 &&
      std::strncmp(request->path, "/edit/", 6) == 0) {
    char slug[MAX_SLUG];
    if (!DecodeRouteSlugStrict(request->path + 6, slug, sizeof(slug))) {
      SetError(response, 400, "Invalid page slug");
      return;
    }

    PageRecord pageRecord;
    bool found = false;
    char err[256];
    if (!DbGetPage(db, slug, &pageRecord, &found, err, sizeof(err))) {
      SetError(response, 500, err);
      return;
    }

    TextBuffer html;
    if (!ArenaAllocTextBuffer(arena, &html, MAX_RESPONSE_BYTES)) {
      SetError(response, 500, "Response arena exhausted");
      return;
    }
    if (found) {
      if (!BuildEditHtml(db, arena, slug, pageRecord.title, pageRecord.markdown,
                         &html, err, sizeof(err))) {
        SetError(response, 500, "Failed to build edit page");
        return;
      }
    } else {
      char defaultMarkdown[1024];
      (void)std::snprintf(defaultMarkdown, sizeof(defaultMarkdown), "# %s\n",
                          slug);
      if (!BuildEditHtml(db, arena, slug, slug, defaultMarkdown, &html, err,
                         sizeof(err))) {
        SetError(response, 500, "Failed to build edit page");
        return;
      }
    }

    if (!SetResponseCopy(response, 200, "text/html; charset=utf-8", html.data,
                         html.length)) {
      SetError(response, 500, "HTML response too large");
    }
    return;
  }

  if (std::strcmp(request->method, "POST") == 0 &&
      std::strncmp(request->path, "/save/", 6) == 0) {
    const char* contentType = FindHeader(request, "content-type");
    if (contentType == nullptr ||
        std::strstr(contentType, "application/x-www-form-urlencoded") ==
            nullptr) {
      SetError(response, 400, "Expected form-urlencoded body");
      return;
    }

    char slug[MAX_SLUG];
    if (!DecodeRouteSlugStrict(request->path + 6, slug, sizeof(slug))) {
      SetError(response, 400, "Invalid page slug");
      return;
    }

    char title[MAX_TITLE];
    char markdown[MAX_PAGE_MARKDOWN];
    if (!ParseFormField(request, "title", title, sizeof(title)) ||
        !ParseFormField(request, "markdown", markdown, sizeof(markdown))) {
      SetError(response, 400, "Missing title or markdown");
      return;
    }
    TrimInPlace(title);
    if (title[0] == '\0') {
      (void)CopyString(title, sizeof(title), slug);
    }

    char err[256];
    if (!DbUpsertPage(db, slug, title, markdown, err, sizeof(err))) {
      SetError(response, 500, err);
      return;
    }

    char location[768];
    char encodedSlug[MAX_PATH];
    if (!UrlEncode(slug, encodedSlug, sizeof(encodedSlug))) {
      SetError(response, 500, "slug encode failed");
      return;
    }
    (void)std::snprintf(location, sizeof(location), "/page/%s", encodedSlug);
    SetRedirect(response, location);
    return;
  }

  if (std::strcmp(request->method, "POST") == 0 &&
      std::strncmp(request->path, "/delete/", 8) == 0) {
    char slug[MAX_SLUG];
    if (!DecodeRouteSlugStrict(request->path + 8, slug, sizeof(slug))) {
      SetError(response, 400, "Invalid page slug");
      return;
    }

    char err[256];
    if (!DbDeletePage(db, slug, err, sizeof(err))) {
      SetError(response, 500, err);
      return;
    }
    SetRedirect(response, "/pages");
    return;
  }

  if (std::strcmp(request->method, "GET") == 0 &&
      std::strncmp(request->path, "/images/new/", 12) == 0) {
    char slug[MAX_SLUG];
    if (!DecodeRouteSlugStrict(request->path + 12, slug, sizeof(slug))) {
      SetError(response, 400, "Invalid page slug");
      return;
    }
    PageRecord pageRecord;
    bool found = false;
    char err[256];
    if (!DbGetPage(db, slug, &pageRecord, &found, err, sizeof(err))) {
      SetError(response, 500, err);
      return;
    }
    if (!found) {
      SetError(response, 404, "Page not found");
      return;
    }

    TextBuffer html;
    if (!ArenaAllocTextBuffer(arena, &html, MAX_RESPONSE_BYTES)) {
      SetError(response, 500, "Response arena exhausted");
      return;
    }
    if (!BuildImageUploadForm(arena, slug, &html)) {
      SetError(response, 500, "Failed to build image upload form");
      return;
    }

    if (!SetResponseCopy(response, 200, "text/html; charset=utf-8", html.data,
                         html.length)) {
      SetError(response, 500, "HTML response too large");
    }
    return;
  }

  if (std::strcmp(request->method, "GET") == 0 &&
      std::strncmp(request->path, "/documents/new/", 15) == 0) {
    char slug[MAX_SLUG];
    if (!DecodeRouteSlugStrict(request->path + 15, slug, sizeof(slug))) {
      SetError(response, 400, "Invalid page slug");
      return;
    }
    PageRecord pageRecord;
    bool found = false;
    char err[256];
    if (!DbGetPage(db, slug, &pageRecord, &found, err, sizeof(err))) {
      SetError(response, 500, err);
      return;
    }
    if (!found) {
      SetError(response, 404, "Page not found");
      return;
    }

    TextBuffer html;
    if (!ArenaAllocTextBuffer(arena, &html, MAX_RESPONSE_BYTES)) {
      SetError(response, 500, "Response arena exhausted");
      return;
    }
    if (!BuildDocumentUploadForm(arena, slug, &html)) {
      SetError(response, 500, "Failed to build document upload form");
      return;
    }

    if (!SetResponseCopy(response, 200, "text/html; charset=utf-8", html.data,
                         html.length)) {
      SetError(response, 500, "HTML response too large");
    }
    return;
  }

  if (std::strcmp(request->method, "POST") == 0 &&
      std::strncmp(request->path, "/images/upload/", 15) == 0) {
    char slug[MAX_SLUG];
    if (!DecodeRouteSlugStrict(request->path + 15, slug, sizeof(slug))) {
      SetError(response, 400, "Invalid page slug");
      return;
    }
    PageRecord pageRecord;
    bool found = false;
    char err[256];
    if (!DbGetPage(db, slug, &pageRecord, &found, err, sizeof(err))) {
      SetError(response, 500, err);
      return;
    }
    if (!found) {
      SetError(response, 404, "Page not found");
      return;
    }

    MultipartImage image;
    ParseMultipartImage(request, &image);
    if (!image.valid) {
      SetError(response, 400, "Missing image data");
      return;
    }

    int imageId = 0;
    const char* detectedMime =
        DetectImageMimeFromData(image.data, image.dataLen);
    if (detectedMime == nullptr) {
      SetError(response, 400, "Unsupported image format");
      return;
    }

    if (!DbInsertImage(db, slug, image.filename, detectedMime, image.data,
                       image.dataLen, &imageId, err, sizeof(err))) {
      SetError(response, 500, err);
      return;
    }

    TextBuffer html;
    if (!ArenaAllocTextBuffer(arena, &html, MAX_RESPONSE_BYTES)) {
      SetError(response, 500, "Response arena exhausted");
      return;
    }
    if (!BuildImageUploadedPage(arena, slug, image.filename, imageId, &html)) {
      SetError(response, 500, "Failed to build success page");
      return;
    }

    if (!SetResponseCopy(response, 200, "text/html; charset=utf-8", html.data,
                         html.length)) {
      SetError(response, 500, "HTML response too large");
    }
    return;
  }

  if (std::strcmp(request->method, "POST") == 0 &&
      std::strncmp(request->path, "/documents/upload/", 18) == 0) {
    char slug[MAX_SLUG];
    if (!DecodeRouteSlugStrict(request->path + 18, slug, sizeof(slug))) {
      SetError(response, 400, "Invalid page slug");
      return;
    }
    PageRecord pageRecord;
    bool found = false;
    char err[256];
    if (!DbGetPage(db, slug, &pageRecord, &found, err, sizeof(err))) {
      SetError(response, 500, err);
      return;
    }
    if (!found) {
      SetError(response, 404, "Page not found");
      return;
    }

    MultipartImage document;
    ParseMultipartDocument(request, &document);
    if (!document.valid) {
      SetError(response, 400, "Missing document data");
      return;
    }

    int documentId = 0;
    const char* detectedMime =
        DetectDocumentMimeFromData(document.data, document.dataLen);
    if (detectedMime == nullptr) {
      SetError(response, 400, "Unsupported document format");
      return;
    }

    if (!DbInsertDocument(db, slug, document.filename, detectedMime,
                          document.data, document.dataLen, &documentId, err,
                          sizeof(err))) {
      SetError(response, 500, err);
      return;
    }

    TextBuffer html;
    if (!ArenaAllocTextBuffer(arena, &html, MAX_RESPONSE_BYTES)) {
      SetError(response, 500, "Response arena exhausted");
      return;
    }
    if (!BuildDocumentUploadedPage(arena, slug, document.filename, documentId,
                                   &html)) {
      SetError(response, 500, "Failed to build success page");
      return;
    }

    if (!SetResponseCopy(response, 200, "text/html; charset=utf-8", html.data,
                         html.length)) {
      SetError(response, 500, "HTML response too large");
    }
    return;
  }

  if (std::strcmp(request->method, "POST") == 0 &&
      std::strncmp(request->path, "/images/delete/", 15) == 0) {
    const char* rest = request->path + 15;
    const char* slash = std::strchr(rest, '/');
    if (slash == nullptr) {
      SetError(response, 400, "Invalid delete image path");
      return;
    }
    char idText[32];
    const size_t idLen = static_cast<size_t>(slash - rest);
    if (idLen == 0U || idLen + 1U > sizeof(idText)) {
      SetError(response, 400, "Invalid image id");
      return;
    }
    (void)std::memcpy(idText, rest, idLen);
    idText[idLen] = '\0';
    int imageId = 0;
    if (!ParsePositiveIntStrict(idText, &imageId)) {
      SetError(response, 400, "Invalid image id");
      return;
    }

    char slug[MAX_SLUG];
    if (!DecodeRouteSlugStrict(slash + 1U, slug, sizeof(slug))) {
      SetError(response, 400, "Invalid page slug");
      return;
    }

    char err[256];
    if (!DbDeleteImage(db, imageId, slug, err, sizeof(err))) {
      SetError(response, 500, err);
      return;
    }

    char encodedSlug[MAX_PATH];
    char location[768];
    if (!UrlEncode(slug, encodedSlug, sizeof(encodedSlug))) {
      SetError(response, 500, "slug encode failed");
      return;
    }
    (void)std::snprintf(location, sizeof(location), "/page/%s", encodedSlug);
    SetRedirect(response, location);
    return;
  }

  if (std::strcmp(request->method, "POST") == 0 &&
      std::strncmp(request->path, "/documents/delete/", 18) == 0) {
    const char* rest = request->path + 18;
    const char* slash = std::strchr(rest, '/');
    if (slash == nullptr) {
      SetError(response, 400, "Invalid delete document path");
      return;
    }
    char idText[32];
    const size_t idLen = static_cast<size_t>(slash - rest);
    if (idLen == 0U || idLen + 1U > sizeof(idText)) {
      SetError(response, 400, "Invalid document id");
      return;
    }
    (void)std::memcpy(idText, rest, idLen);
    idText[idLen] = '\0';
    int documentId = 0;
    if (!ParsePositiveIntStrict(idText, &documentId)) {
      SetError(response, 400, "Invalid document id");
      return;
    }

    char slug[MAX_SLUG];
    if (!DecodeRouteSlugStrict(slash + 1U, slug, sizeof(slug))) {
      SetError(response, 400, "Invalid page slug");
      return;
    }

    char err[256];
    if (!DbDeleteDocument(db, documentId, slug, err, sizeof(err))) {
      SetError(response, 500, err);
      return;
    }

    char encodedSlug[MAX_PATH];
    char location[768];
    if (!UrlEncode(slug, encodedSlug, sizeof(encodedSlug))) {
      SetError(response, 500, "slug encode failed");
      return;
    }
    (void)std::snprintf(location, sizeof(location), "/page/%s", encodedSlug);
    SetRedirect(response, location);
    return;
  }

  if (std::strcmp(request->method, "GET") == 0 &&
      std::strncmp(request->path, "/page/", 6) == 0) {
    char slug[MAX_SLUG];
    if (!DecodeRouteSlugStrict(request->path + 6, slug, sizeof(slug))) {
      SetError(response, 400, "Invalid page slug");
      return;
    }

    PageRecord pageRecord;
    bool found = false;
    char err[256];
    (void)CopyString(err, sizeof(err), "Failed to build page");
    if (!DbGetPage(db, slug, &pageRecord, &found, err, sizeof(err))) {
      SetError(response, 500, err);
      return;
    }

    TextBuffer html;
    if (!ArenaAllocTextBuffer(arena, &html, MAX_RESPONSE_BYTES)) {
      SetError(response, 500, "Response arena exhausted");
      return;
    }
    if (!found) {
      if (!BuildMissingPageHtml(arena, slug, &html)) {
        SetError(response, 500, "Failed to build missing page");
        return;
      }
    } else {
      if (!BuildViewHtml(db, arena, &pageRecord, &html, err, sizeof(err))) {
        SetError(response, 500, err);
        return;
      }
    }

    if (!SetResponseCopy(response, 200, "text/html; charset=utf-8", html.data,
                         html.length)) {
      SetError(response, 500, "HTML response too large");
    }
    return;
  }

  if (std::strcmp(request->method, "GET") != 0 &&
      std::strcmp(request->method, "POST") != 0) {
    SetError(response, 405, "Only GET and POST are supported");
    return;
  }

  SetError(response, 404, "Not found");
}

/*
 * Serialize and send HTTP response headers/body to client socket.
 */
bool SendResponse(int clientFd, const HttpResponse* response) {
  TextBuffer header;
  char headerData[4096];
  header.data = headerData;
  header.capacity = sizeof(headerData);
  BufferReset(&header);

  if (!BufferAppendFormat(&header, "HTTP/1.1 %d %s\r\n", response->status,
                          StatusText(response->status))) {
    return false;
  }
  if (!BufferAppendFormat(&header, "Content-Type: %s\r\n",
                          response->contentType)) {
    return false;
  }
  if (!BufferAppendFormat(&header, "Content-Length: %zu\r\n",
                          response->bodyLen)) {
    return false;
  }
  if (!BufferAppend(&header, "Connection: close\r\n")) {
    return false;
  }
  if (response->hasLocation) {
    if (!BufferAppendFormat(&header, "Location: %s\r\n", response->location)) {
      return false;
    }
  }
  if (!BufferAppend(&header, "\r\n")) {
    return false;
  }

  int sendFlags = 0;
#if defined(MSG_NOSIGNAL)
  sendFlags = MSG_NOSIGNAL;
#endif

  size_t sent = 0;
  while (sent < header.length) {
    const ssize_t n =
        send(clientFd, header.data + sent, header.length - sent, sendFlags);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }

  sent = 0;
  while (sent < response->bodyLen) {
    const ssize_t n = send(clientFd, response->body + sent,
                           response->bodyLen - sent, sendFlags);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }

  return true;
}

/*
 * Print CLI usage/help text to stdout.
 */
void PrintUsage() {
  std::printf("mswiki options:\n");
  std::printf(
      "  --listen <addr>            Listen address (default: 0.0.0.0)\n");
  std::printf("  --port <port>              Listen port (default: 8080)\n");
  std::printf(
      "  --db <path>                SQLite database path (default: "
      "./mswiki.db)\n");
  std::printf("  --max-body-bytes <bytes>   Maximum request body size\n");
  std::printf("  --self-test                Run in-process test suite\n");
  std::printf("  --help                     Show this help\n");
}

/*
 * Parse command-line arguments into runtime Config.
 */
ArgParseResult ParseArgs(int argc, char** argv, Config* config) {
  (void)CopyString(config->listenHost, sizeof(config->listenHost), "0.0.0.0");
  config->port = DEFAULT_PORT;
  (void)CopyString(config->dbPath, sizeof(config->dbPath), "./mswiki.db");
  config->maxBodyBytes = MAX_BODY_BYTES;
  config->runSelfTest = false;

  int i = 1;
  while (i < argc) {
    if (std::strcmp(argv[i], "--help") == 0) {
      PrintUsage();
      return ARG_PARSE_HELP;
    }

    if (std::strcmp(argv[i], "--self-test") == 0) {
      config->runSelfTest = true;
      i += 1;
      continue;
    }

    if (i + 1 >= argc) {
      std::fprintf(stderr, "Missing value for %s\n", argv[i]);
      return ARG_PARSE_ERROR;
    }

    const char* arg = argv[i];
    const char* value = argv[i + 1];
    if (std::strcmp(arg, "--listen") == 0) {
      if (!CopyString(config->listenHost, sizeof(config->listenHost), value)) {
        std::fprintf(stderr, "listen value too long\n");
        return ARG_PARSE_ERROR;
      }
    } else if (std::strcmp(arg, "--port") == 0) {
      int parsedPort = 0;
      if (!ParsePositiveIntStrict(value, &parsedPort)) {
        std::fprintf(stderr, "Invalid port\n");
        return ARG_PARSE_ERROR;
      }
      config->port = parsedPort;
      if (config->port <= 0 || config->port > 65535) {
        std::fprintf(stderr, "Invalid port\n");
        return ARG_PARSE_ERROR;
      }
    } else if (std::strcmp(arg, "--db") == 0) {
      if (!CopyString(config->dbPath, sizeof(config->dbPath), value)) {
        std::fprintf(stderr, "db path too long\n");
        return ARG_PARSE_ERROR;
      }
    } else if (std::strcmp(arg, "--max-body-bytes") == 0) {
      size_t parsedMaxBody = 0U;
      if (!ParseUnsignedSizeStrict(value, &parsedMaxBody)) {
        std::fprintf(stderr, "max-body-bytes must be 1..%zu\n",
                     static_cast<size_t>(MAX_BODY_BYTES));
        return ARG_PARSE_ERROR;
      }
      config->maxBodyBytes = parsedMaxBody;
      if (config->maxBodyBytes == 0U || config->maxBodyBytes > MAX_BODY_BYTES) {
        std::fprintf(stderr, "max-body-bytes must be 1..%zu\n",
                     static_cast<size_t>(MAX_BODY_BYTES));
        return ARG_PARSE_ERROR;
      }
    } else {
      std::fprintf(stderr, "Unknown argument: %s\n", arg);
      return ARG_PARSE_ERROR;
    }

    i += 2;
  }

  return ARG_PARSE_OK;
}

#include "sections/selftest.inc"

}  // namespace

/*
 * Program entrypoint that configures, runs, and shuts down the server.
 */
int main(int argc, char** argv) {
  Config config;
  const ArgParseResult parseResult = ParseArgs(argc, argv, &config);
  if (parseResult == ARG_PARSE_HELP) {
    return 0;
  }
  if (parseResult == ARG_PARSE_ERROR) {
    return 1;
  }
  if (config.runSelfTest) {
    return RunSelfTests();
  }

  sqlite3* db = nullptr;
  char err[256];
  if (!DbOpen(&db, config.dbPath, err, sizeof(err))) {
    std::fprintf(stderr, "Failed to open database: %s\n", err);
    return 1;
  }

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
  std::signal(SIGPIPE, SIG_IGN);

  const int serverFd = socket(AF_INET, SOCK_STREAM, 0);
  if (serverFd < 0) {
    std::fprintf(stderr, "socket() failed: %s\n", std::strerror(errno));
    (void)sqlite3_close(db);
    return 1;
  }

  int opt = 1;
  (void)setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  (void)std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(config.port));

  if (inet_pton(AF_INET, config.listenHost, &addr.sin_addr) != 1) {
    std::fprintf(stderr, "Invalid IPv4 address: %s\n", config.listenHost);
    (void)close(serverFd);
    (void)sqlite3_close(db);
    return 1;
  }

  if (bind(serverFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) !=
      0) {
    std::fprintf(stderr, "bind() failed: %s\n", std::strerror(errno));
    (void)close(serverFd);
    (void)sqlite3_close(db);
    return 1;
  }

  if (listen(serverFd, LISTEN_BACKLOG) != 0) {
    std::fprintf(stderr, "listen() failed: %s\n", std::strerror(errno));
    (void)close(serverFd);
    (void)sqlite3_close(db);
    return 1;
  }

  std::printf("mswiki listening on %s:%d (db=%s)\n", config.listenHost,
              config.port, config.dbPath);

  while (gKeepRunning != 0) {
    struct sockaddr_in clientAddr;
    socklen_t clientLen = sizeof(clientAddr);
    const int clientFd = accept(
        serverFd, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);
    if (clientFd < 0) {
      if (errno == EINTR) {
        continue;
      }
      // Prevent tight-loop CPU burn if accept() is failing persistently.
      (void)usleep(10000U);
      continue;
    }

    struct timeval timeout;
    timeout.tv_sec = CLIENT_SOCKET_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    (void)setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));
    (void)setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout));

    size_t reqLen = 0;
    bool tooLarge = false;
    HttpResponse response;

    if (!ReadRequestFromSocket(clientFd, config.maxBodyBytes, gRequestBuffer,
                               &reqLen, &tooLarge)) {
      if (tooLarge) {
        SetError(&response, 413, "Payload too large");
      } else {
        SetError(&response, 400, "Bad request");
      }
      (void)SendResponse(clientFd, &response);
      (void)close(clientFd);
      continue;
    }

    HttpRequest request;
    const unsigned char* bodyStart = nullptr;
    size_t bodyOffset = 0;
    if (!ParseHttpRequest(gRequestBuffer, reqLen, &request, &bodyStart,
                          &bodyOffset)) {
      SetError(&response, 400, "Invalid request");
      (void)SendResponse(clientFd, &response);
      (void)close(clientFd);
      continue;
    }

    RequestArena arena;
    HandleRequest(db, &arena, &request, &response);
    (void)SendResponse(clientFd, &response);
    (void)close(clientFd);
  }

  (void)close(serverFd);
  (void)sqlite3_close(db);
  return 0;
}
