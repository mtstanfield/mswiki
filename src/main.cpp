#include <arpa/inet.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
static const size_t MAX_BINARY_RESPONSE_BYTES = 10U * 1024U * 1024U;
static const size_t MAX_PAGE_MARKDOWN = 512U * 1024U;
static const size_t MAX_LOGO_BYTES = 2U * 1024U * 1024U;
static const size_t MAX_FOOTNOTES = 64U;
static const size_t MAX_FOOTNOTE_ID = 32U;
static const size_t MAX_FOOTNOTE_TEXT = 1024U;

typedef struct {
  char listenHost[64];
  int port;
  char dbPath[512];
  char assetsPath[512];
  size_t maxBodyBytes;
  bool runSelfTest;
} Config;

typedef struct {
  char key[MAX_HEADER_KEY];
  char value[MAX_HEADER_VALUE];
} Header;

typedef struct {
  char method[MAX_METHOD];
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
  char data[MAX_RESPONSE_BYTES];
  size_t length;
} TextBuffer;

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
void ExtractWikiLinks(const char* markdown,
                      char links[][MAX_SLUG],
                      size_t* linkCount,
                      size_t maxLinks);
bool RenderMarkdownToHtml(const char* markdown, TextBuffer* html);
bool DbAppendBacklinksHtml(sqlite3* db,
                           const char* slug,
                           TextBuffer* html,
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
bool DbDeleteImage(sqlite3* db,
                   int id,
                   const char* pageSlug,
                   char* err,
                   size_t errSize);
bool ParseFormField(const HttpRequest* request,
                    const char* key,
                    char* out,
                    size_t outSize);
void ParseMultipartImage(const HttpRequest* request, MultipartImage* image);
bool ParseRequestLine(const char* line, HttpRequest* request);
const char* DetectImageMimeFromData(const unsigned char* data, size_t len);
void HandleRequest(sqlite3* db,
                   const Config* config,
                   const HttpRequest* request,
                   HttpResponse* response);

void SignalHandler(int) {
  gKeepRunning = 0;
}

void BufferReset(TextBuffer* buffer) {
  buffer->length = 0;
  buffer->data[0] = '\0';
}

bool BufferAppendBytes(TextBuffer* buffer, const char* text, size_t len) {
  if (buffer->length + len + 1U > sizeof(buffer->data)) {
    return false;
  }
  (void)std::memcpy(buffer->data + buffer->length, text, len);
  buffer->length += len;
  buffer->data[buffer->length] = '\0';
  return true;
}

bool BufferAppend(TextBuffer* buffer, const char* text) {
  return BufferAppendBytes(buffer, text, std::strlen(text));
}

bool BufferAppendChar(TextBuffer* buffer, char ch) {
  if (buffer->length + 2U > sizeof(buffer->data)) {
    return false;
  }
  buffer->data[buffer->length] = ch;
  buffer->length += 1U;
  buffer->data[buffer->length] = '\0';
  return true;
}

bool BufferAppendFormat(TextBuffer* buffer, const char* format, ...) {
  va_list args;
  va_start(args, format);
  const size_t remaining = sizeof(buffer->data) - buffer->length;
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

int ToLowerAscii(int ch) {
  if (ch >= 'A' && ch <= 'Z') {
    return ch - 'A' + 'a';
  }
  return ch;
}

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

void LowerString(char* text) {
  const size_t len = std::strlen(text);
  for (size_t i = 0; i < len; i++) {
    text[i] = static_cast<char>(ToLowerAscii(text[i]));
  }
}

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

size_t UrlDecode(char* out, size_t outSize, const char* in, size_t inLen) {
  size_t outLen = 0;
  for (size_t i = 0; i < inLen; i++) {
    if (outLen + 1U >= outSize) {
      break;
    }
    if (in[i] == '%' && i + 2U < inLen) {
      char hex[3];
      hex[0] = in[i + 1U];
      hex[1] = in[i + 2U];
      hex[2] = '\0';
      const long value = std::strtol(hex, nullptr, 16);
      out[outLen++] = static_cast<char>(value);
      i += 2U;
    } else if (in[i] == '+') {
      out[outLen++] = ' ';
    } else {
      out[outLen++] = in[i];
    }
  }
  out[outLen] = '\0';
  return outLen;
}

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

typedef struct {
  int passed;
  int failed;
} SelfTestState;

void SelfTestExpect(SelfTestState* state, bool condition, const char* name) {
  if (condition) {
    state->passed += 1;
  } else {
    state->failed += 1;
    std::fprintf(stderr, "SELFTEST FAIL: %s\n", name);
  }
}

void SelfTestSlugAndCodec(SelfTestState* state) {
  char slug[MAX_SLUG];
  SelfTestExpect(state,
                 Slugify("Hello World", slug, sizeof(slug)) &&
                     std::strcmp(slug, "hello-world") == 0,
                 "Slugify basic text");
  SelfTestExpect(state, IsSafeSlug("good/page_1"),
                 "IsSafeSlug allows valid slugs");
  SelfTestExpect(state, !IsSafeSlug("../bad"),
                 "IsSafeSlug rejects parent refs");

  char encoded[128];
  SelfTestExpect(state,
                 UrlEncode("a b/c", encoded, sizeof(encoded)) &&
                     std::strcmp(encoded, "a%20b/c") == 0,
                 "UrlEncode escapes spaces");

  char decoded[128];
  (void)UrlDecode(decoded, sizeof(decoded), "a%20b%2Fc+z", 11U);
  SelfTestExpect(state, std::strcmp(decoded, "a b/c z") == 0,
                 "UrlDecode reverses escapes");

  int parsedPositive = 0;
  SelfTestExpect(state, ParsePositiveIntStrict("42", &parsedPositive),
                 "ParsePositiveIntStrict valid");
  SelfTestExpect(state, parsedPositive == 42, "ParsePositiveIntStrict value");
  SelfTestExpect(state, !ParsePositiveIntStrict("42x", &parsedPositive),
                 "ParsePositiveIntStrict rejects suffix");
}

void SelfTestLinksAndMarkdown(SelfTestState* state) {
  char links[8][MAX_SLUG];
  size_t linkCount = 0;
  ExtractWikiLinks("See [[Target Page]] and [[target-page|label]]", links,
                   &linkCount, 8U);
  SelfTestExpect(state, linkCount == 1U, "ExtractWikiLinks deduplicates");
  SelfTestExpect(state, std::strcmp(links[0], "target-page") == 0,
                 "ExtractWikiLinks slugifies");

  TextBuffer html;
  BufferReset(&html);
  const bool rendered = RenderMarkdownToHtml(
      "# Title\nWiki [[Other Page|Label]][^n]\n"
      "![Side Image](/image/7)\n"
      "[^n]: Footnote body.\n"
      "* item\n",
      &html);
  SelfTestExpect(state, rendered, "RenderMarkdownToHtml success");
  SelfTestExpect(state, std::strstr(html.data, "<h1>Title</h1>") != nullptr,
                 "Render markdown heading");
  SelfTestExpect(state,
                 std::strstr(html.data, "href=\"/page/other-page\"") != nullptr,
                 "Render wiki href");
  SelfTestExpect(state, std::strstr(html.data, ">Label</a>") != nullptr,
                 "Render wiki label");
  SelfTestExpect(state,
                 std::strstr(html.data, "class=\"margin-figure\"") != nullptr,
                 "Render image as margin figure");
  SelfTestExpect(state, std::strstr(html.data, "class=\"sidenote\"") != nullptr,
                 "Render sidenote");
  SelfTestExpect(
      state,
      std::strstr(html.data, "<section class=\"panel footnotes\">") != nullptr,
      "Render footnotes section");

  static const unsigned char PNG_SIG[] = {0x89U, 0x50U, 0x4EU, 0x47U,
                                          0x0DU, 0x0AU, 0x1AU, 0x0AU};
  SelfTestExpect(state,
                 std::strcmp(DetectImageMimeFromData(PNG_SIG, sizeof(PNG_SIG)),
                             "image/png") == 0,
                 "DetectImageMimeFromData png");
}

void SelfTestFormAndMultipart(SelfTestState* state) {
  HttpRequest request;
  (void)std::memset(&request, 0, sizeof(request));
  static const char FORM_BODY[] = "title=Hello+World&markdown=Line1%0ALine2";
  request.body = reinterpret_cast<const unsigned char*>(FORM_BODY);
  request.bodyLen = sizeof(FORM_BODY) - 1U;

  char title[MAX_TITLE];
  char markdown[128];
  SelfTestExpect(state, ParseFormField(&request, "title", title, sizeof(title)),
                 "ParseFormField title exists");
  SelfTestExpect(state, std::strcmp(title, "Hello World") == 0,
                 "ParseFormField decodes +");
  SelfTestExpect(
      state, ParseFormField(&request, "markdown", markdown, sizeof(markdown)),
      "ParseFormField markdown exists");
  SelfTestExpect(state, std::strcmp(markdown, "Line1\nLine2") == 0,
                 "ParseFormField decodes percent escapes");

  (void)std::memset(&request, 0, sizeof(request));
  request.headerCount = 1;
  (void)CopyString(request.headers[0].key, sizeof(request.headers[0].key),
                   "content-type");
  (void)CopyString(request.headers[0].value, sizeof(request.headers[0].value),
                   "multipart/form-data; boundary=abc123");
  static const char MULTIPART_BODY[] =
      "--abc123\r\n"
      "Content-Disposition: form-data; name=\"image\"; filename=\"p.png\"\r\n"
      "Content-Type: image/png\r\n"
      "\r\n"
      "PNGDATA\r\n"
      "--abc123--\r\n";
  request.body = reinterpret_cast<const unsigned char*>(MULTIPART_BODY);
  request.bodyLen = sizeof(MULTIPART_BODY) - 1U;

  MultipartImage image;
  ParseMultipartImage(&request, &image);
  SelfTestExpect(state, image.valid, "ParseMultipartImage sets valid");
  SelfTestExpect(state, std::strcmp(image.filename, "p.png") == 0,
                 "ParseMultipartImage filename");
  SelfTestExpect(state, std::strcmp(image.mimeType, "image/png") == 0,
                 "ParseMultipartImage mime type");
  SelfTestExpect(state, image.dataLen == 7U, "ParseMultipartImage data len");
  SelfTestExpect(state, std::memcmp(image.data, "PNGDATA", 7U) == 0,
                 "ParseMultipartImage payload");
}

void SelfTestDatabase(SelfTestState* state) {
  char dbPath[128];
  (void)std::snprintf(dbPath, sizeof(dbPath), "/tmp/mswiki-selftest-%d.db",
                      static_cast<int>(getpid()));

  sqlite3* db = nullptr;
  char err[256];
  if (!DbOpen(&db, dbPath, err, sizeof(err))) {
    SelfTestExpect(state, false, "DbOpen");
    return;
  }

  const bool upsertHome =
      DbUpsertPage(db, "home", "Home", "Link to [[other]].", err, sizeof(err));
  const bool upsertOther =
      DbUpsertPage(db, "other", "Other", "# Other", err, sizeof(err));
  SelfTestExpect(state, upsertHome, "DbUpsertPage home");
  SelfTestExpect(state, upsertOther, "DbUpsertPage other");

  PageRecord page;
  bool found = false;
  const bool getOk = DbGetPage(db, "home", &page, &found, err, sizeof(err));
  SelfTestExpect(state, getOk, "DbGetPage call");
  SelfTestExpect(state, found, "DbGetPage found row");
  SelfTestExpect(state, std::strcmp(page.title, "Home") == 0,
                 "DbGetPage title");

  TextBuffer backlinksHtml;
  BufferReset(&backlinksHtml);
  const bool backlinksOk =
      DbAppendBacklinksHtml(db, "other", &backlinksHtml, err, sizeof(err));
  SelfTestExpect(state, backlinksOk, "DbAppendBacklinksHtml call");
  SelfTestExpect(state,
                 std::strstr(backlinksHtml.data, "/page/home") != nullptr,
                 "Backlink contains home");

  static const unsigned char IMAGE_DATA[] = {0x47U, 0x49U, 0x46U};
  int imageId = 0;
  const bool insertImageOk =
      DbInsertImage(db, "home", "tiny.gif", "image/gif", IMAGE_DATA,
                    sizeof(IMAGE_DATA), &imageId, err, sizeof(err));
  SelfTestExpect(state, insertImageOk, "DbInsertImage call");
  SelfTestExpect(state, imageId > 0, "DbInsertImage id");

  sqlite3_stmt* imageStmt = nullptr;
  char mime[MAX_MIME];
  const unsigned char* blobData = nullptr;
  int blobLen = 0;
  found = false;
  const bool getImageOk =
      DbGetImage(db, imageId, mime, sizeof(mime), &blobData, &blobLen,
                 &imageStmt, &found, err, sizeof(err));
  SelfTestExpect(state, getImageOk, "DbGetImage call");
  SelfTestExpect(state, found, "DbGetImage found row");
  SelfTestExpect(state, std::strcmp(mime, "image/gif") == 0, "DbGetImage mime");
  SelfTestExpect(state, blobLen == 3, "DbGetImage size");
  if (imageStmt != nullptr) {
    (void)sqlite3_finalize(imageStmt);
  }

  const bool deleteImageOk =
      DbDeleteImage(db, imageId, "home", err, sizeof(err));
  SelfTestExpect(state, deleteImageOk, "DbDeleteImage call");
  imageStmt = nullptr;
  found = true;
  const bool getDeletedImageOk =
      DbGetImage(db, imageId, mime, sizeof(mime), &blobData, &blobLen,
                 &imageStmt, &found, err, sizeof(err));
  SelfTestExpect(state, getDeletedImageOk, "DbGetImage deleted call");
  SelfTestExpect(state, !found, "DbDeleteImage removed row");
  if (imageStmt != nullptr) {
    (void)sqlite3_finalize(imageStmt);
  }

  (void)sqlite3_close(db);
  (void)unlink(dbPath);
}

void SelfTestHttpHandler(SelfTestState* state) {
  sqlite3* db = nullptr;
  char err[256];
  if (!DbOpen(&db, ":memory:", err, sizeof(err))) {
    SelfTestExpect(state, false, "DbOpen in-memory");
    return;
  }

  Config config;
  (void)CopyString(config.listenHost, sizeof(config.listenHost), "127.0.0.1");
  config.port = 8080;
  (void)CopyString(config.dbPath, sizeof(config.dbPath), ":memory:");
  config.assetsPath[0] = '\0';
  config.maxBodyBytes = MAX_BODY_BYTES;
  config.runSelfTest = false;

  HttpRequest request;
  HttpResponse response;
  (void)std::memset(&request, 0, sizeof(request));
  (void)CopyString(request.method, sizeof(request.method), "GET");
  (void)CopyString(request.path, sizeof(request.path), "/page/home");
  HandleRequest(db, &config, &request, &response);
  SelfTestExpect(state, response.status == 200, "HandleRequest GET /page/home");
  SelfTestExpect(state, response.bodyLen > 0U, "HandleRequest body exists");
  SelfTestExpect(state,
                 std::strstr(reinterpret_cast<const char*>(response.body),
                             "Create this page") != nullptr,
                 "Missing page response content");

  (void)std::memset(&request, 0, sizeof(request));
  (void)CopyString(request.method, sizeof(request.method), "GET");
  (void)CopyString(request.path, sizeof(request.path), "/pages");
  HandleRequest(db, &config, &request, &response);
  SelfTestExpect(state, response.status == 200, "HandleRequest GET /pages");
  SelfTestExpect(state,
                 std::strstr(reinterpret_cast<const char*>(response.body),
                             "All Pages") != nullptr,
                 "Pages response content");

  HttpRequest parsedRequest;
  (void)std::memset(&parsedRequest, 0, sizeof(parsedRequest));
  SelfTestExpect(state, !ParseRequestLine("GET / HTTP/2", &parsedRequest),
                 "ParseRequestLine rejects HTTP/2");
  SelfTestExpect(state, ParseRequestLine("GET / HTTP/1.1", &parsedRequest),
                 "ParseRequestLine accepts HTTP/1.1");

  (void)sqlite3_close(db);
}

int RunSelfTests() {
  SelfTestState state;
  state.passed = 0;
  state.failed = 0;

  SelfTestSlugAndCodec(&state);
  SelfTestLinksAndMarkdown(&state);
  SelfTestFormAndMultipart(&state);
  SelfTestDatabase(&state);
  SelfTestHttpHandler(&state);

  std::printf("Self-test results: passed=%d failed=%d\n", state.passed,
              state.failed);
  return (state.failed == 0) ? 0 : 1;
}

void SetError(HttpResponse* response, int status, const char* message) {
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

const char* FindHeader(const HttpRequest* request, const char* key) {
  for (size_t i = 0; i < request->headerCount; i++) {
    if (std::strcmp(request->headers[i].key, key) == 0) {
      return request->headers[i].value;
    }
  }
  return nullptr;
}

bool ParseRequestLine(const char* line, HttpRequest* request) {
  char version[16];
  int matched = std::sscanf(line, "%7s %1023s %15s", request->method,
                            request->target, version);
  if (matched != 3) {
    return false;
  }
  if (std::strcmp(version, "HTTP/1.1") != 0 &&
      std::strcmp(version, "HTTP/1.0") != 0) {
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
    if (lineEnd == nullptr) {
      break;
    }
    *lineEnd = '\0';

    char* colon = std::strchr(cursor, ':');
    if (colon != nullptr) {
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
    }

    cursor = lineEnd + 2;
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
  const unsigned char* bodyStart = nullptr;
  size_t bodyOffset = 0U;
  if (!ParseHttpRequest(out, *outLen, &parsed, &bodyStart, &bodyOffset)) {
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

bool DbOpen(sqlite3** db, const char* dbPath, char* err, size_t errSize) {
  const int openFlags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  if (sqlite3_open_v2(dbPath, db, openFlags, nullptr) != SQLITE_OK) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(*db));
    return false;
  }
  sqlite3_extended_result_codes(*db, 1);

  const char* migrationSql =
      "PRAGMA journal_mode=DELETE;"
      "PRAGMA synchronous=NORMAL;"
      "PRAGMA busy_timeout=5000;"
      "PRAGMA foreign_keys=ON;"
      "CREATE TABLE IF NOT EXISTS pages ("
      "id INTEGER PRIMARY KEY,"
      "slug TEXT NOT NULL UNIQUE,"
      "title TEXT NOT NULL,"
      "markdown TEXT NOT NULL,"
      "created_at TEXT NOT NULL,"
      "updated_at TEXT NOT NULL"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_pages_slug ON pages(slug);"
      "CREATE TABLE IF NOT EXISTS page_links ("
      "from_slug TEXT NOT NULL,"
      "to_slug TEXT NOT NULL,"
      "PRIMARY KEY (from_slug, to_slug)"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_page_links_to ON page_links(to_slug);"
      "CREATE TABLE IF NOT EXISTS images ("
      "id INTEGER PRIMARY KEY,"
      "page_slug TEXT NOT NULL,"
      "filename TEXT NOT NULL,"
      "mime_type TEXT NOT NULL,"
      "data BLOB NOT NULL,"
      "created_at TEXT NOT NULL"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_images_page_slug ON images(page_slug);";

  char* sqliteErr = nullptr;
  if (sqlite3_exec(*db, migrationSql, nullptr, nullptr, &sqliteErr) !=
      SQLITE_OK) {
    (void)std::snprintf(err, errSize, "%s",
                        sqliteErr == nullptr ? "sqlite error" : sqliteErr);
    if (sqliteErr != nullptr) {
      sqlite3_free(sqliteErr);
    }
    return false;
  }

  return true;
}

bool DbGetPage(sqlite3* db,
               const char* slug,
               PageRecord* page,
               bool* found,
               char* err,
               size_t errSize) {
  *found = false;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT slug, title, markdown, created_at, updated_at FROM pages WHERE "
      "slug = ?";

  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
    return false;
  }

  (void)sqlite3_bind_text(stmt, 1, slug, -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const unsigned char* dbSlug = sqlite3_column_text(stmt, 0);
    const unsigned char* dbTitle = sqlite3_column_text(stmt, 1);
    const unsigned char* dbMarkdown = sqlite3_column_text(stmt, 2);
    const unsigned char* dbCreated = sqlite3_column_text(stmt, 3);
    const unsigned char* dbUpdated = sqlite3_column_text(stmt, 4);

    if (dbSlug == nullptr || dbTitle == nullptr || dbMarkdown == nullptr ||
        dbCreated == nullptr || dbUpdated == nullptr) {
      (void)sqlite3_finalize(stmt);
      (void)std::snprintf(err, errSize, "invalid database row");
      return false;
    }

    if (!CopyString(page->slug, sizeof(page->slug),
                    reinterpret_cast<const char*>(dbSlug)) ||
        !CopyString(page->title, sizeof(page->title),
                    reinterpret_cast<const char*>(dbTitle)) ||
        !CopyString(page->createdAt, sizeof(page->createdAt),
                    reinterpret_cast<const char*>(dbCreated)) ||
        !CopyString(page->updatedAt, sizeof(page->updatedAt),
                    reinterpret_cast<const char*>(dbUpdated))) {
      (void)sqlite3_finalize(stmt);
      (void)std::snprintf(err, errSize, "field too long");
      return false;
    }

    const int markdownLen = sqlite3_column_bytes(stmt, 2);
    if (markdownLen < 0 ||
        static_cast<size_t>(markdownLen) + 1U > sizeof(page->markdown)) {
      (void)sqlite3_finalize(stmt);
      (void)std::snprintf(err, errSize, "markdown too large");
      return false;
    }
    (void)std::memcpy(page->markdown, dbMarkdown,
                      static_cast<size_t>(markdownLen));
    page->markdown[markdownLen] = '\0';

    *found = true;
  } else if (rc != SQLITE_DONE) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
    (void)sqlite3_finalize(stmt);
    return false;
  }

  (void)sqlite3_finalize(stmt);
  return true;
}

void ExtractWikiLinks(const char* markdown,
                      char links[][MAX_SLUG],
                      size_t* linkCount,
                      size_t maxLinks) {
  *linkCount = 0;
  const size_t len = std::strlen(markdown);
  size_t i = 0;
  while (i + 3U < len) {
    if (markdown[i] == '[' && markdown[i + 1U] == '[') {
      size_t j = i + 2U;
      while (j + 1U < len && !(markdown[j] == ']' && markdown[j + 1U] == ']')) {
        j++;
      }
      if (j + 1U < len) {
        char rawTarget[MAX_SLUG];
        size_t k = 0;
        size_t pos = i + 2U;
        while (pos < j && markdown[pos] != '|' && k + 1U < sizeof(rawTarget)) {
          rawTarget[k++] = markdown[pos++];
        }
        rawTarget[k] = '\0';

        char slug[MAX_SLUG];
        if (Slugify(rawTarget, slug, sizeof(slug))) {
          bool duplicate = false;
          for (size_t n = 0; n < *linkCount; n++) {
            if (std::strcmp(links[n], slug) == 0) {
              duplicate = true;
              break;
            }
          }
          if (!duplicate && *linkCount < maxLinks) {
            (void)CopyString(links[*linkCount], MAX_SLUG, slug);
            *linkCount += 1U;
          }
        }

        i = j + 2U;
        continue;
      }
    }
    i++;
  }
}

bool DbUpsertPage(sqlite3* db,
                  const char* slug,
                  const char* title,
                  const char* markdown,
                  char* err,
                  size_t errSize) {
  char timestamp[MAX_TIMESTAMP];
  NowIso8601(timestamp, sizeof(timestamp));

  char* sqliteErr = nullptr;
  if (sqlite3_exec(db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr,
                   &sqliteErr) != SQLITE_OK) {
    (void)std::snprintf(
        err, errSize, "%s",
        sqliteErr == nullptr ? "begin transaction failed" : sqliteErr);
    if (sqliteErr != nullptr) {
      sqlite3_free(sqliteErr);
    }
    return false;
  }

  sqlite3_stmt* stmt = nullptr;
  const char* upsertSql =
      "INSERT INTO pages (slug, title, markdown, created_at, updated_at) "
      "VALUES (?, ?, ?, ?, ?) "
      "ON CONFLICT(slug) DO UPDATE SET title=excluded.title, "
      "markdown=excluded.markdown, "
      "updated_at=excluded.updated_at";

  if (sqlite3_prepare_v2(db, upsertSql, -1, &stmt, nullptr) != SQLITE_OK) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
    (void)sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  (void)sqlite3_bind_text(stmt, 1, slug, -1, SQLITE_TRANSIENT);
  (void)sqlite3_bind_text(stmt, 2, title, -1, SQLITE_TRANSIENT);
  (void)sqlite3_bind_text(stmt, 3, markdown, -1, SQLITE_TRANSIENT);
  (void)sqlite3_bind_text(stmt, 4, timestamp, -1, SQLITE_TRANSIENT);
  (void)sqlite3_bind_text(stmt, 5, timestamp, -1, SQLITE_TRANSIENT);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
    (void)sqlite3_finalize(stmt);
    (void)sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  (void)sqlite3_finalize(stmt);

  const char* deleteSql = "DELETE FROM page_links WHERE from_slug = ?";
  if (sqlite3_prepare_v2(db, deleteSql, -1, &stmt, nullptr) != SQLITE_OK) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
    (void)sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  (void)sqlite3_bind_text(stmt, 1, slug, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
    (void)sqlite3_finalize(stmt);
    (void)sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  (void)sqlite3_finalize(stmt);

  char links[128][MAX_SLUG];
  size_t linkCount = 0;
  ExtractWikiLinks(markdown, links, &linkCount, 128U);

  const char* insertLinkSql =
      "INSERT OR IGNORE INTO page_links (from_slug, to_slug) VALUES (?, ?)";
  if (sqlite3_prepare_v2(db, insertLinkSql, -1, &stmt, nullptr) != SQLITE_OK) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
    (void)sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  for (size_t i = 0; i < linkCount; i++) {
    (void)sqlite3_reset(stmt);
    (void)sqlite3_clear_bindings(stmt);
    (void)sqlite3_bind_text(stmt, 1, slug, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(stmt, 2, links[i], -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
      (void)sqlite3_finalize(stmt);
      (void)sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
  }
  (void)sqlite3_finalize(stmt);

  if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &sqliteErr) != SQLITE_OK) {
    (void)std::snprintf(err, errSize, "%s",
                        sqliteErr == nullptr ? "commit failed" : sqliteErr);
    if (sqliteErr != nullptr) {
      sqlite3_free(sqliteErr);
    }
    (void)sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  return true;
}

bool DbListPagesHtml(sqlite3* db, TextBuffer* html, char* err, size_t errSize) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT slug, title FROM pages ORDER BY lower(title)";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
    return false;
  }

  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
      break;
    }
    if (rc != SQLITE_ROW) {
      (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
      (void)sqlite3_finalize(stmt);
      return false;
    }

    const char* slug =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const char* title =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (slug == nullptr || title == nullptr) {
      continue;
    }

    char encodedSlug[MAX_PATH];
    if (!UrlEncode(slug, encodedSlug, sizeof(encodedSlug))) {
      continue;
    }

    if (!BufferAppend(html, "<li><a href=\"/page/")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
    if (!BufferAppend(html, encodedSlug)) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
    if (!BufferAppend(html, "\">")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
    if (!BufferAppendEscaped(html, title, std::strlen(title))) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
    if (!BufferAppend(html, "</a> <span class=\"meta\">(")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
    if (!BufferAppendEscaped(html, slug, std::strlen(slug))) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
    if (!BufferAppend(html, ")</span></li>")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
  }

  (void)sqlite3_finalize(stmt);
  return true;
}

bool DbAppendBacklinksHtml(sqlite3* db,
                           const char* slug,
                           TextBuffer* html,
                           char* err,
                           size_t errSize) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT from_slug FROM page_links WHERE to_slug = ? ORDER BY from_slug";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
    return false;
  }

  (void)sqlite3_bind_text(stmt, 1, slug, -1, SQLITE_TRANSIENT);
  bool any = false;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
      break;
    }
    if (rc != SQLITE_ROW) {
      (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
      (void)sqlite3_finalize(stmt);
      return false;
    }
    const char* fromSlug =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (fromSlug == nullptr) {
      continue;
    }
    if (!any) {
      if (!BufferAppend(html, "<ul class=\"backlinks\">")) {
        (void)sqlite3_finalize(stmt);
        return false;
      }
      any = true;
    }
    char encoded[MAX_PATH];
    if (!UrlEncode(fromSlug, encoded, sizeof(encoded))) {
      continue;
    }
    if (!BufferAppend(html, "<li><a href=\"/page/")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
    if (!BufferAppend(html, encoded)) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
    if (!BufferAppend(html, "\">")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
    if (!BufferAppendEscaped(html, fromSlug, std::strlen(fromSlug))) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
    if (!BufferAppend(html, "</a></li>")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
  }

  if (!any) {
    if (!BufferAppend(html, "<p class=\"meta\">No backlinks yet.</p>")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
  } else {
    if (!BufferAppend(html, "</ul>")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
  }

  (void)sqlite3_finalize(stmt);
  return true;
}

bool DbAppendImagesHtml(sqlite3* db,
                        const char* slug,
                        TextBuffer* html,
                        char* err,
                        size_t errSize) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT id, filename FROM images WHERE page_slug = ? ORDER BY id DESC";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
    return false;
  }

  (void)sqlite3_bind_text(stmt, 1, slug, -1, SQLITE_TRANSIENT);
  bool any = false;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
      break;
    }
    if (rc != SQLITE_ROW) {
      (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
      (void)sqlite3_finalize(stmt);
      return false;
    }

    const int id = sqlite3_column_int(stmt, 0);
    const char* filename =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (filename == nullptr) {
      continue;
    }

    if (!any) {
      if (!BufferAppend(html, "<ul class=\"images\">")) {
        (void)sqlite3_finalize(stmt);
        return false;
      }
      any = true;
    }

    char encodedSlug[MAX_PATH];
    if (!UrlEncode(slug, encodedSlug, sizeof(encodedSlug))) {
      continue;
    }

    if (!BufferAppendFormat(html, "<li><a href=\"/image/%d\">", id)) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
    if (!BufferAppendEscaped(html, filename, std::strlen(filename))) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
    if (!BufferAppendFormat(html,
                            "</a> (<code>![](/image/%d)</code>) "
                            "<form class=\"inline-delete\" method=\"post\" "
                            "action=\"/images/delete/%d/",
                            id, id)) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
    if (!BufferAppend(html, encodedSlug)) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
    if (!BufferAppend(html,
                      "\"><button type=\"submit\" "
                      "class=\"delete-button\">Delete</button></form></li>")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
  }

  if (!any) {
    if (!BufferAppend(
            html, "<p class=\"meta\">No images uploaded for this page.</p>")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
  } else {
    if (!BufferAppend(html, "</ul>")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
  }

  (void)sqlite3_finalize(stmt);
  return true;
}

bool DbInsertImage(sqlite3* db,
                   const char* pageSlug,
                   const char* filename,
                   const char* mimeType,
                   const unsigned char* data,
                   size_t dataLen,
                   int* imageId,
                   char* err,
                   size_t errSize) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO images (page_slug, filename, mime_type, data, created_at) "
      "VALUES (?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
    return false;
  }

  char timestamp[MAX_TIMESTAMP];
  NowIso8601(timestamp, sizeof(timestamp));

  (void)sqlite3_bind_text(stmt, 1, pageSlug, -1, SQLITE_TRANSIENT);
  (void)sqlite3_bind_text(stmt, 2, filename, -1, SQLITE_TRANSIENT);
  (void)sqlite3_bind_text(stmt, 3, mimeType, -1, SQLITE_TRANSIENT);
  (void)sqlite3_bind_blob(stmt, 4, data, static_cast<int>(dataLen),
                          SQLITE_TRANSIENT);
  (void)sqlite3_bind_text(stmt, 5, timestamp, -1, SQLITE_TRANSIENT);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
    (void)sqlite3_finalize(stmt);
    return false;
  }

  *imageId = static_cast<int>(sqlite3_last_insert_rowid(db));
  (void)sqlite3_finalize(stmt);
  return true;
}

bool DbGetImage(sqlite3* db,
                int id,
                char* mimeType,
                size_t mimeTypeSize,
                const unsigned char** data,
                int* dataLen,
                sqlite3_stmt** stmtOut,
                bool* found,
                char* err,
                size_t errSize) {
  *found = false;
  *stmtOut = nullptr;

  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT mime_type, data FROM images WHERE id = ?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
    return false;
  }

  (void)sqlite3_bind_int(stmt, 1, id);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE) {
    (void)sqlite3_finalize(stmt);
    *found = false;
    return true;
  }
  if (rc != SQLITE_ROW) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
    (void)sqlite3_finalize(stmt);
    return false;
  }

  const char* dbMime =
      reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  const unsigned char* dbData =
      reinterpret_cast<const unsigned char*>(sqlite3_column_blob(stmt, 1));
  const int blobSize = sqlite3_column_bytes(stmt, 1);
  if (dbMime == nullptr || dbData == nullptr || blobSize < 0) {
    (void)sqlite3_finalize(stmt);
    (void)std::snprintf(err, errSize, "invalid image row");
    return false;
  }

  if (!CopyString(mimeType, mimeTypeSize, dbMime)) {
    (void)sqlite3_finalize(stmt);
    (void)std::snprintf(err, errSize, "mime type too long");
    return false;
  }

  *data = dbData;
  *dataLen = blobSize;
  *stmtOut = stmt;
  *found = true;
  return true;
}

bool DbDeleteImage(sqlite3* db,
                   int id,
                   const char* pageSlug,
                   char* err,
                   size_t errSize) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "DELETE FROM images WHERE id = ? AND page_slug = ?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
    return false;
  }
  (void)sqlite3_bind_int(stmt, 1, id);
  (void)sqlite3_bind_text(stmt, 2, pageSlug, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
    (void)sqlite3_finalize(stmt);
    return false;
  }
  (void)sqlite3_finalize(stmt);
  return true;
}

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

  char boundary[128];
  if (!CopyString(boundary, sizeof(boundary), boundaryPos)) {
    return;
  }
  TrimInPlace(boundary);
  if (boundary[0] == '"') {
    const size_t len = std::strlen(boundary);
    if (len > 1U && boundary[len - 1U] == '"') {
      (void)std::memmove(boundary, boundary + 1, len - 2U);
      boundary[len - 2U] = '\0';
    }
  }

  char delimiter[160];
  if (std::snprintf(delimiter, sizeof(delimiter), "--%s", boundary) < 0) {
    return;
  }

  const unsigned char* body = request->body;
  const size_t bodyLen = request->bodyLen;
  const unsigned char* search = body;
  size_t remaining = bodyLen;

  while (true) {
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

    while (true) {
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
        break;
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

bool ParseFormField(const HttpRequest* request,
                    const char* key,
                    char* out,
                    size_t outSize) {
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
      (void)UrlDecode(out, outSize, encodedValue, encodedLen);
      return true;
    }

    if (end >= request->bodyLen) {
      break;
    }
    pos = end + 1U;
  }

  return false;
}

bool ParseFootnoteDefinitionLine(const char* line,
                                 size_t lineLen,
                                 char* id,
                                 size_t idSize,
                                 char* text,
                                 size_t textSize) {
  if (lineLen < 6U || line[0] != '[' || line[1] != '^') {
    return false;
  }
  size_t i = 2U;
  size_t idLen = 0U;
  while (i < lineLen && line[i] != ']') {
    if (idLen + 1U >= idSize) {
      return false;
    }
    id[idLen++] = line[i++];
  }
  if (i + 2U >= lineLen || line[i] != ']' || line[i + 1U] != ':') {
    return false;
  }
  i += 2U;
  while (i < lineLen && line[i] == ' ') {
    i++;
  }
  size_t textLen = 0U;
  while (i < lineLen) {
    if (textLen + 1U >= textSize) {
      return false;
    }
    text[textLen++] = line[i++];
  }
  id[idLen] = '\0';
  text[textLen] = '\0';
  return idLen > 0U;
}

int FindFootnoteIndex(const char ids[][MAX_FOOTNOTE_ID],
                      size_t count,
                      const char* id) {
  for (size_t i = 0; i < count; i++) {
    if (std::strcmp(ids[i], id) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool WriteInlineMarkdown(TextBuffer* out,
                         const char* text,
                         size_t len,
                         const char footnoteIds[][MAX_FOOTNOTE_ID],
                         const char footnoteText[][MAX_FOOTNOTE_TEXT],
                         size_t footnoteCount,
                         int* footnoteOrder,
                         int* nextFootnoteNumber) {
  size_t i = 0;
  while (i < len) {
    if (i + 5U < len && text[i] == '!' && text[i + 1U] == '[') {
      size_t closeBracket = i + 2U;
      while (closeBracket < len && text[closeBracket] != ']') {
        closeBracket++;
      }
      if (closeBracket + 1U < len && text[closeBracket + 1U] == '(') {
        size_t closeParen = closeBracket + 2U;
        while (closeParen < len && text[closeParen] != ')') {
          closeParen++;
        }
        if (closeParen < len) {
          if (!BufferAppend(out,
                            "<figure class=\"margin-figure\"><a href=\"")) {
            return false;
          }
          if (!BufferAppendEscaped(out, text + closeBracket + 2U,
                                   closeParen - (closeBracket + 2U))) {
            return false;
          }
          if (!BufferAppend(
                  out, "\" target=\"_blank\" rel=\"noopener\"><img src=\"")) {
            return false;
          }
          if (!BufferAppendEscaped(out, text + closeBracket + 2U,
                                   closeParen - (closeBracket + 2U))) {
            return false;
          }
          if (!BufferAppend(out, "\" alt=\"")) {
            return false;
          }
          if (!BufferAppendEscaped(out, text + i + 2U,
                                   closeBracket - (i + 2U))) {
            return false;
          }
          if (!BufferAppend(out, "\" loading=\"lazy\"></a><figcaption>")) {
            return false;
          }
          if (!BufferAppendEscaped(out, text + i + 2U,
                                   closeBracket - (i + 2U))) {
            return false;
          }
          if (!BufferAppend(out, "</figcaption></figure>")) {
            return false;
          }
          i = closeParen + 1U;
          continue;
        }
      }
    }

    if (i + 3U < len && text[i] == '[' && text[i + 1U] == '^') {
      size_t closeBracket = i + 2U;
      while (closeBracket < len && text[closeBracket] != ']') {
        closeBracket++;
      }
      if (closeBracket < len) {
        char id[MAX_FOOTNOTE_ID];
        size_t idLen = 0U;
        for (size_t j = i + 2U; j < closeBracket; j++) {
          if (idLen + 1U >= sizeof(id)) {
            break;
          }
          id[idLen++] = text[j];
        }
        id[idLen] = '\0';
        const int idx = FindFootnoteIndex(footnoteIds, footnoteCount, id);
        if (idx >= 0) {
          if (footnoteOrder[idx] == 0) {
            footnoteOrder[idx] = *nextFootnoteNumber;
            *nextFootnoteNumber += 1;
          }
          const int number = footnoteOrder[idx];
          if (!BufferAppendFormat(
                  out,
                  "<sup class=\"footnote-ref\"><a href=\"#fn-%s\">%d</a></sup>"
                  "<span class=\"sidenote\" id=\"sidenote-%s\"><span "
                  "class=\"sidenote-number\">%d.</span> ",
                  footnoteIds[idx], number, footnoteIds[idx], number)) {
            return false;
          }
          if (!BufferAppendEscaped(out, footnoteText[idx],
                                   std::strlen(footnoteText[idx]))) {
            return false;
          }
          if (!BufferAppend(out, "</span>")) {
            return false;
          }
          i = closeBracket + 1U;
          continue;
        }
      }
    }

    if (i + 3U < len && text[i] == '[' && text[i + 1U] == '[') {
      size_t end = i + 2U;
      while (end + 1U < len && !(text[end] == ']' && text[end + 1U] == ']')) {
        end++;
      }
      if (end + 1U < len) {
        char targetRaw[MAX_SLUG];
        char labelRaw[MAX_TITLE];
        size_t targetLen = 0;
        size_t labelLen = 0;
        bool split = false;

        for (size_t j = i + 2U; j < end; j++) {
          if (text[j] == '|' && !split) {
            split = true;
            continue;
          }
          if (!split) {
            if (targetLen + 1U < sizeof(targetRaw)) {
              targetRaw[targetLen++] = text[j];
            }
          } else {
            if (labelLen + 1U < sizeof(labelRaw)) {
              labelRaw[labelLen++] = text[j];
            }
          }
        }

        targetRaw[targetLen] = '\0';
        labelRaw[labelLen] = '\0';
        if (!split) {
          (void)CopyString(labelRaw, sizeof(labelRaw), targetRaw);
        }

        char slug[MAX_SLUG];
        if (!Slugify(targetRaw, slug, sizeof(slug))) {
          if (!BufferAppendEscaped(out, text + i, end + 2U - i)) {
            return false;
          }
          i = end + 2U;
          continue;
        }

        char encodedSlug[MAX_PATH];
        if (!UrlEncode(slug, encodedSlug, sizeof(encodedSlug))) {
          return false;
        }

        if (!BufferAppend(out, "<a class=\"wiki-link\" href=\"/page/")) {
          return false;
        }
        if (!BufferAppend(out, encodedSlug)) {
          return false;
        }
        if (!BufferAppend(out, "\">")) {
          return false;
        }
        if (!BufferAppendEscaped(out, labelRaw, std::strlen(labelRaw))) {
          return false;
        }
        if (!BufferAppend(out, "</a>")) {
          return false;
        }

        i = end + 2U;
        continue;
      }
    }

    if (i + 4U < len && text[i] == '[') {
      size_t closeBracket = i + 1U;
      while (closeBracket < len && text[closeBracket] != ']') {
        closeBracket++;
      }
      if (closeBracket + 1U < len && text[closeBracket + 1U] == '(') {
        size_t closeParen = closeBracket + 2U;
        while (closeParen < len && text[closeParen] != ')') {
          closeParen++;
        }
        if (closeParen < len) {
          if (!BufferAppend(out, "<a href=\"")) {
            return false;
          }
          if (!BufferAppendEscaped(out, text + closeBracket + 2U,
                                   closeParen - (closeBracket + 2U))) {
            return false;
          }
          if (!BufferAppend(out, "\">")) {
            return false;
          }
          if (!BufferAppendEscaped(out, text + i + 1U,
                                   closeBracket - (i + 1U))) {
            return false;
          }
          if (!BufferAppend(out, "</a>")) {
            return false;
          }
          i = closeParen + 1U;
          continue;
        }
      }
    }

    if (!BufferAppendEscaped(out, text + i, 1U)) {
      return false;
    }
    i++;
  }

  return true;
}

bool RenderMarkdownToHtml(const char* markdown, TextBuffer* html) {
  const size_t len = std::strlen(markdown);
  char footnoteIds[MAX_FOOTNOTES][MAX_FOOTNOTE_ID];
  char footnoteText[MAX_FOOTNOTES][MAX_FOOTNOTE_TEXT];
  int footnoteOrder[MAX_FOOTNOTES];
  size_t footnoteCount = 0U;
  int nextFootnoteNumber = 1;
  for (size_t i = 0; i < MAX_FOOTNOTES; i++) {
    footnoteOrder[i] = 0;
  }

  size_t scanStart = 0;
  while (scanStart <= len) {
    size_t scanEnd = scanStart;
    while (scanEnd < len && markdown[scanEnd] != '\n') {
      scanEnd++;
    }
    size_t lineLen = scanEnd - scanStart;
    if (lineLen > 0U && markdown[scanEnd - 1U] == '\r') {
      lineLen--;
    }
    char id[MAX_FOOTNOTE_ID];
    char text[MAX_FOOTNOTE_TEXT];
    if (footnoteCount < MAX_FOOTNOTES &&
        ParseFootnoteDefinitionLine(markdown + scanStart, lineLen, id,
                                    sizeof(id), text, sizeof(text))) {
      (void)CopyString(footnoteIds[footnoteCount], sizeof(footnoteIds[0]), id);
      (void)CopyString(footnoteText[footnoteCount], sizeof(footnoteText[0]),
                       text);
      footnoteCount += 1U;
    }
    if (scanEnd == len) {
      break;
    }
    scanStart = scanEnd + 1U;
  }

  size_t lineStart = 0;
  bool inCodeBlock = false;
  bool inList = false;

  while (lineStart <= len) {
    size_t lineEnd = lineStart;
    while (lineEnd < len && markdown[lineEnd] != '\n') {
      lineEnd++;
    }

    size_t lineLen = lineEnd - lineStart;
    if (lineLen > 0U && markdown[lineEnd - 1U] == '\r') {
      lineLen--;
    }

    const char* line = markdown + lineStart;
    char footId[MAX_FOOTNOTE_ID];
    char footText[MAX_FOOTNOTE_TEXT];
    if (ParseFootnoteDefinitionLine(line, lineLen, footId, sizeof(footId),
                                    footText, sizeof(footText))) {
      if (lineEnd == len) {
        break;
      }
      lineStart = lineEnd + 1U;
      continue;
    }

    if (lineLen >= 3U && std::strncmp(line, "```", 3) == 0) {
      if (inList) {
        if (!BufferAppend(html, "</ul>")) {
          return false;
        }
        inList = false;
      }
      if (!inCodeBlock) {
        if (!BufferAppend(html, "<pre><code>")) {
          return false;
        }
        inCodeBlock = true;
      } else {
        if (!BufferAppend(html, "</code></pre>")) {
          return false;
        }
        inCodeBlock = false;
      }
    } else if (inCodeBlock) {
      if (!BufferAppendEscaped(html, line, lineLen)) {
        return false;
      }
      if (!BufferAppend(html, "\n")) {
        return false;
      }
    } else if (lineLen == 0U) {
      if (inList) {
        if (!BufferAppend(html, "</ul>")) {
          return false;
        }
        inList = false;
      }
    } else if (lineLen > 2U && (line[0] == '-' || line[0] == '*') &&
               line[1] == ' ') {
      if (!inList) {
        if (!BufferAppend(html, "<ul>")) {
          return false;
        }
        inList = true;
      }
      if (!BufferAppend(html, "<li>")) {
        return false;
      }
      if (!WriteInlineMarkdown(html, line + 2, lineLen - 2U, footnoteIds,
                               footnoteText, footnoteCount, footnoteOrder,
                               &nextFootnoteNumber)) {
        return false;
      }
      if (!BufferAppend(html, "</li>")) {
        return false;
      }
    } else {
      if (inList) {
        if (!BufferAppend(html, "</ul>")) {
          return false;
        }
        inList = false;
      }

      size_t hashes = 0;
      while (hashes < lineLen && line[hashes] == '#') {
        hashes++;
      }
      if (hashes > 0U && hashes <= 6U && hashes < lineLen &&
          line[hashes] == ' ') {
        if (!BufferAppendFormat(html, "<h%zu>", hashes)) {
          return false;
        }
        if (!WriteInlineMarkdown(html, line + hashes + 1U,
                                 lineLen - hashes - 1U, footnoteIds,
                                 footnoteText, footnoteCount, footnoteOrder,
                                 &nextFootnoteNumber)) {
          return false;
        }
        if (!BufferAppendFormat(html, "</h%zu>", hashes)) {
          return false;
        }
      } else {
        if (!BufferAppend(html, "<p>")) {
          return false;
        }
        if (!WriteInlineMarkdown(html, line, lineLen, footnoteIds, footnoteText,
                                 footnoteCount, footnoteOrder,
                                 &nextFootnoteNumber)) {
          return false;
        }
        if (!BufferAppend(html, "</p>")) {
          return false;
        }
      }
    }

    if (lineEnd == len) {
      break;
    }
    lineStart = lineEnd + 1U;
  }

  if (inList) {
    if (!BufferAppend(html, "</ul>")) {
      return false;
    }
  }
  if (inCodeBlock) {
    if (!BufferAppend(html, "</code></pre>")) {
      return false;
    }
  }

  bool anyFootnotes = false;
  for (size_t i = 0; i < footnoteCount; i++) {
    if (footnoteOrder[i] > 0) {
      anyFootnotes = true;
      break;
    }
  }
  if (anyFootnotes) {
    if (!BufferAppend(
            html,
            "<section class=\"panel footnotes\"><h2>Footnotes</h2><ol>")) {
      return false;
    }
    for (size_t i = 0; i < footnoteCount; i++) {
      if (footnoteOrder[i] == 0) {
        continue;
      }
      if (!BufferAppendFormat(html,
                              "<li id=\"fn-%s\"><span class=\"meta\">%d."
                              "</span> ",
                              footnoteIds[i], footnoteOrder[i])) {
        return false;
      }
      if (!BufferAppendEscaped(html, footnoteText[i],
                               std::strlen(footnoteText[i]))) {
        return false;
      }
      if (!BufferAppend(html, "</li>")) {
        return false;
      }
    }
    if (!BufferAppend(html, "</ol></section>")) {
      return false;
    }
  }

  return true;
}

const char* DetectMimeType(const char* filename) {
  const char* dot = std::strrchr(filename, '.');
  if (dot == nullptr) {
    return "application/octet-stream";
  }
  dot++;
  if (strcasecmp(dot, "css") == 0) {
    return "text/css; charset=utf-8";
  }
  if (strcasecmp(dot, "svg") == 0) {
    return "image/svg+xml";
  }
  if (strcasecmp(dot, "png") == 0) {
    return "image/png";
  }
  if (strcasecmp(dot, "jpg") == 0 || strcasecmp(dot, "jpeg") == 0) {
    return "image/jpeg";
  }
  if (strcasecmp(dot, "gif") == 0) {
    return "image/gif";
  }
  if (strcasecmp(dot, "webp") == 0) {
    return "image/webp";
  }
  return "application/octet-stream";
}

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

bool ReadFile(const char* path,
              unsigned char* dst,
              size_t dstSize,
              size_t* outLen) {
  *outLen = 0;
  const int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return false;
  }

  while (*outLen < dstSize) {
    const ssize_t n = read(fd, dst + *outLen, dstSize - *outLen);
    if (n < 0) {
      (void)close(fd);
      return false;
    }
    if (n == 0) {
      break;
    }
    *outLen += static_cast<size_t>(n);
  }

  (void)close(fd);
  return true;
}

bool FindLogo(const Config* config, char* path, size_t pathSize) {
  if (config->assetsPath[0] == '\0') {
    return false;
  }

  static const char* NAMES[] = {"logo.svg", "logo.png", "logo.jpg", "logo.jpeg",
                                "logo.gif"};
  for (size_t i = 0; i < 5U; i++) {
    if (std::snprintf(path, pathSize, "%s/%s", config->assetsPath, NAMES[i]) <
        0) {
      continue;
    }
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
      return true;
    }
  }

  return false;
}

bool BuildPageLayout(const Config* config,
                     const char* title,
                     const char* content,
                     TextBuffer* out) {
  char logoPath[768];
  const bool hasLogo = FindLogo(config, logoPath, sizeof(logoPath));

  if (!BufferAppend(out,
                    "<!doctype html><html><head><meta charset=\"utf-8\">"
                    "<meta name=\"viewport\" "
                    "content=\"width=device-width,initial-scale=1\">"
                    "<title>")) {
    return false;
  }
  if (!BufferAppendEscaped(out, title, std::strlen(title))) {
    return false;
  }
  if (!BufferAppend(out,
                    " - mswiki</title>"
                    "<link rel=\"stylesheet\" href=\"/style.css\"></head><body>"
                    "<header><div class=\"header-inner\"><a class=\"brand\" "
                    "href=\"/page/home\">")) {
    return false;
  }
  if (hasLogo) {
    if (!BufferAppend(out, "<img src=\"/logo\" alt=\"logo\">")) {
      return false;
    }
  }
  if (!BufferAppend(out,
                    "<strong>mswiki</strong></a><nav>"
                    "<a href=\"/page/home\">Home</a>"
                    "<a href=\"/pages\">All Pages</a>"
                    "<a href=\"/edit/home\">Edit Home</a>"
                    "</nav></div></header><main>")) {
    return false;
  }
  if (!BufferAppend(out, content)) {
    return false;
  }
  if (!BufferAppend(out, "</main></body></html>")) {
    return false;
  }

  return true;
}

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
      "main{max-width:1120px;margin:0 auto;padding:30px 24px 56px;}"
      "article{position:relative;background:var(--paper);border:1px solid "
      "var(--line);border-radius:6px;padding:36px 42% 36px 56px;}"
      ".content-body{max-width:66ch;}"
      "h1,h2,h3,h4{font-weight:500;line-height:1.25;letter-spacing:-.01em;}"
      "p,li{font-size:1.08rem;}"
      "a{color:var(--accent);}"
      "textarea,input[type=\"text\"]{width:100%;border:1px solid "
      "var(--line);border-radius:4px;padding:10px;font:inherit;background:#fff;"
      "}"
      "textarea{min-height:340px;font-family:\"SF "
      "Mono\",Menlo,Consolas,monospace;}"
      "button{border:0;border-radius:8px;background:var(--accent);color:#fff;"
      "padding:10px 14px;font:inherit;cursor:pointer;}"
      "button:hover{background:var(--accent-hover);}"
      ".delete-button{background:#8a2f2f;padding:6px 10px;font-size:.85rem;}"
      ".delete-button:hover{background:#6f2525;}"
      ".inline-delete{display:inline-block;margin-left:10px;}"
      ".meta{color:var(--muted);font-size:.9rem;}"
      ".notice{padding:10px 12px;border-left:4px solid "
      "var(--accent);background:#eef7fb;}"
      ".panel{margin-top:16px;padding-top:12px;border-top:1px solid "
      "var(--line);}"
      "ul.page-list,ul.backlinks,ul.images{padding-left:18px;}"
      "pre{background:#141414;color:#f4f4f4;padding:12px;border-radius:8px;"
      "overflow-x:auto;}"
      "code{font-family:\"SF "
      "Mono\",Menlo,Consolas,monospace;background:#eef2ff;padding:0 "
      "4px;border-radius:4px;color:#111827;}"
      "pre code{background:transparent;color:#f9fafb;padding:0;}"
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

bool BuildPagesHtml(sqlite3* db,
                    const Config* config,
                    TextBuffer* page,
                    char* err,
                    size_t errSize) {
  TextBuffer content;
  BufferReset(&content);

  if (!BufferAppend(&content,
                    "<article><h1>All Pages</h1><p><a "
                    "href=\"/edit/new-page\">Create a page</a></p>")) {
    return false;
  }

  TextBuffer items;
  BufferReset(&items);
  if (!DbListPagesHtml(db, &items, err, errSize)) {
    return false;
  }

  if (items.length == 0U) {
    if (!BufferAppend(&content,
                      "<p class=\"notice\">No pages exist yet. Start with <a "
                      "href=\"/edit/home\">Home</a>.</p>")) {
      return false;
    }
  } else {
    if (!BufferAppend(&content, "<ul class=\"page-list\">")) {
      return false;
    }
    if (!BufferAppend(&content, items.data)) {
      return false;
    }
    if (!BufferAppend(&content, "</ul>")) {
      return false;
    }
  }

  if (!BufferAppend(&content, "</article>")) {
    return false;
  }

  BufferReset(page);
  return BuildPageLayout(config, "All Pages", content.data, page);
}

bool BuildEditHtml(const Config* config,
                   const char* slug,
                   const char* title,
                   const char* markdown,
                   TextBuffer* page) {
  TextBuffer content;
  BufferReset(&content);

  char encodedSlug[MAX_PATH];
  if (!UrlEncode(slug, encodedSlug, sizeof(encodedSlug))) {
    return false;
  }

  if (!BufferAppend(&content, "<article><h1>Edit ")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, slug, std::strlen(slug))) {
    return false;
  }
  if (!BufferAppend(&content, "</h1><form method=\"post\" action=\"/save/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(
          &content,
          "\"><label for=\"title\">Title</label><br>"
          "<input type=\"text\" id=\"title\" name=\"title\" value=\"")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, title, std::strlen(title))) {
    return false;
  }
  if (!BufferAppend(
          &content,
          "\" required><br><br><label for=\"markdown\">Markdown</label><br>"
          "<textarea id=\"markdown\" name=\"markdown\" required>")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, markdown, std::strlen(markdown))) {
    return false;
  }
  if (!BufferAppend(
          &content,
          "</textarea><br><p class=\"meta\">Wiki links: "
          "<code>[[Target Page]]</code> or <code>[[target|label]]</code>.</p>"
          "<p class=\"meta\">Footnotes: <code>[^id]</code> in text and "
          "<code>[^id]: note</code> definitions.</p>"
          "<button type=\"submit\">Save</button> <a href=\"/page/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(&content, "\">Cancel</a></form></article>")) {
    return false;
  }

  BufferReset(page);
  return BuildPageLayout(config, "Edit", content.data, page);
}

bool BuildMissingPageHtml(const Config* config,
                          const char* slug,
                          TextBuffer* page) {
  TextBuffer content;
  BufferReset(&content);

  char encodedSlug[MAX_PATH];
  if (!UrlEncode(slug, encodedSlug, sizeof(encodedSlug))) {
    return false;
  }

  if (!BufferAppend(&content, "<article><h1>")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, slug, std::strlen(slug))) {
    return false;
  }
  if (!BufferAppend(&content,
                    "</h1><p class=\"notice\">This page does not exist yet.</p>"
                    "<p><a href=\"/edit/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(&content, "\">Create this page</a></p></article>")) {
    return false;
  }

  BufferReset(page);
  return BuildPageLayout(config, "Missing Page", content.data, page);
}

bool BuildViewHtml(sqlite3* db,
                   const Config* config,
                   const PageRecord* pageRecord,
                   TextBuffer* page,
                   char* err,
                   size_t errSize) {
  TextBuffer markdownHtml;
  BufferReset(&markdownHtml);
  if (!RenderMarkdownToHtml(pageRecord->markdown, &markdownHtml)) {
    (void)std::snprintf(err, errSize, "markdown render overflow");
    return false;
  }

  TextBuffer content;
  BufferReset(&content);
  char encodedSlug[MAX_PATH];
  if (!UrlEncode(pageRecord->slug, encodedSlug, sizeof(encodedSlug))) {
    (void)std::snprintf(err, errSize, "slug encode failed");
    return false;
  }

  if (!BufferAppend(&content, "<article><h1>")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, pageRecord->title,
                           std::strlen(pageRecord->title))) {
    return false;
  }
  if (!BufferAppend(&content, "</h1><p class=\"meta\">slug: <code>")) {
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
  if (!BufferAppend(&content, "</p>")) {
    return false;
  }

  if (!BufferAppend(&content, "<div class=\"content-body\">")) {
    return false;
  }
  if (!BufferAppend(&content, markdownHtml.data)) {
    return false;
  }
  if (!BufferAppend(&content, "</div>")) {
    return false;
  }

  if (!BufferAppend(&content, "<p><a href=\"/edit/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(&content, "\">Edit page</a> | <a href=\"/raw/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(&content,
                    "\">View raw markdown</a> | <a href=\"/images/new/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(&content,
                    "\">Upload image</a></p>"
                    "<section class=\"panel\"><h2>Backlinks</h2>")) {
    return false;
  }

  if (!DbAppendBacklinksHtml(db, pageRecord->slug, &content, err, errSize)) {
    return false;
  }

  if (!BufferAppend(&content,
                    "</section><section class=\"panel\"><h2>Images</h2>")) {
    return false;
  }

  if (!DbAppendImagesHtml(db, pageRecord->slug, &content, err, errSize)) {
    return false;
  }

  if (!BufferAppend(&content, "</section></article>")) {
    return false;
  }

  BufferReset(page);
  return BuildPageLayout(config, pageRecord->title, content.data, page);
}

bool BuildImageUploadForm(const Config* config,
                          const char* slug,
                          TextBuffer* page) {
  TextBuffer content;
  BufferReset(&content);

  char encodedSlug[MAX_PATH];
  if (!UrlEncode(slug, encodedSlug, sizeof(encodedSlug))) {
    return false;
  }

  if (!BufferAppend(&content, "<article><h1>Upload image for ")) {
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
          "\"><input type=\"file\" name=\"image\" accept=\"image/*\" required>"
          "<br><br><button type=\"submit\">Upload</button> <a href=\"/page/")) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(&content, "\">Back</a></form></article>")) {
    return false;
  }

  BufferReset(page);
  return BuildPageLayout(config, "Upload Image", content.data, page);
}

bool BuildImageUploadedPage(const Config* config,
                            const char* slug,
                            const char* filename,
                            int imageId,
                            TextBuffer* page) {
  TextBuffer content;
  BufferReset(&content);

  char encodedSlug[MAX_PATH];
  if (!UrlEncode(slug, encodedSlug, sizeof(encodedSlug))) {
    return false;
  }

  if (!BufferAppend(&content,
                    "<article><h1>Image uploaded</h1><p>Use this markdown in "
                    "your page:</p><pre><code>![")) {
    return false;
  }
  if (!BufferAppendEscaped(&content, filename, std::strlen(filename))) {
    return false;
  }
  if (!BufferAppendFormat(
          &content, "](/image/%d)</code></pre><p><a href=\"/edit/", imageId)) {
    return false;
  }
  if (!BufferAppend(&content, encodedSlug)) {
    return false;
  }
  if (!BufferAppend(&content, "\">Back to edit page</a></p></article>")) {
    return false;
  }

  BufferReset(page);
  return BuildPageLayout(config, "Image Uploaded", content.data, page);
}

bool BuildStyleCss(const Config* config, TextBuffer* css) {
  BufferReset(css);
  BuildDefaultCss(css);

  if (config->assetsPath[0] == '\0') {
    return true;
  }

  char path[768];
  if (std::snprintf(path, sizeof(path), "%s/style.css", config->assetsPath) <
      0) {
    return true;
  }

  size_t fileLen = 0;
  if (!ReadFile(path, gBinaryBuffer, sizeof(gBinaryBuffer) - 1U, &fileLen)) {
    return true;
  }

  gBinaryBuffer[fileLen] = '\0';
  if (!BufferAppend(css, "\n/* custom style.css */\n")) {
    return false;
  }
  if (!BufferAppend(css, reinterpret_cast<const char*>(gBinaryBuffer))) {
    return false;
  }

  return true;
}

void HandleRequest(sqlite3* db,
                   const Config* config,
                   const HttpRequest* request,
                   HttpResponse* response) {
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
    if (!BuildStyleCss(config, &css)) {
      SetError(response, 500, "Failed to build CSS");
      return;
    }
    if (!SetResponseCopy(response, 200, "text/css; charset=utf-8", css.data,
                         css.length)) {
      SetError(response, 500, "CSS response too large");
    }
    return;
  }

  if (std::strcmp(request->method, "GET") == 0 &&
      std::strcmp(request->path, "/logo") == 0) {
    char logoPath[768];
    if (!FindLogo(config, logoPath, sizeof(logoPath))) {
      SetError(response, 404, "Logo not found");
      return;
    }
    size_t logoLen = 0;
    if (!ReadFile(logoPath, gBinaryBuffer, MAX_LOGO_BYTES, &logoLen)) {
      SetError(response, 404, "Logo not found");
      return;
    }
    SetResponse(response, 200, DetectMimeType(logoPath), gBinaryBuffer,
                logoLen);
    return;
  }

  if (std::strcmp(request->method, "GET") == 0 &&
      std::strcmp(request->path, "/pages") == 0) {
    TextBuffer html;
    char err[256];
    if (!BuildPagesHtml(db, config, &html, err, sizeof(err))) {
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
      std::strncmp(request->path, "/raw/", 5) == 0) {
    char decoded[MAX_PATH];
    (void)UrlDecode(decoded, sizeof(decoded), request->path + 5,
                    std::strlen(request->path + 5));
    char slug[MAX_SLUG];
    if (!Slugify(decoded, slug, sizeof(slug)) || !IsSafeSlug(slug)) {
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
    char decoded[MAX_PATH];
    (void)UrlDecode(decoded, sizeof(decoded), request->path + 6,
                    std::strlen(request->path + 6));
    char slug[MAX_SLUG];
    if (!Slugify(decoded, slug, sizeof(slug)) || !IsSafeSlug(slug)) {
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
    if (found) {
      if (!BuildEditHtml(config, slug, pageRecord.title, pageRecord.markdown,
                         &html)) {
        SetError(response, 500, "Failed to build edit page");
        return;
      }
    } else {
      char defaultMarkdown[1024];
      (void)std::snprintf(defaultMarkdown, sizeof(defaultMarkdown), "# %s\n",
                          slug);
      if (!BuildEditHtml(config, slug, slug, defaultMarkdown, &html)) {
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

    char decoded[MAX_PATH];
    (void)UrlDecode(decoded, sizeof(decoded), request->path + 6,
                    std::strlen(request->path + 6));
    char slug[MAX_SLUG];
    if (!Slugify(decoded, slug, sizeof(slug)) || !IsSafeSlug(slug)) {
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

  if (std::strcmp(request->method, "GET") == 0 &&
      std::strncmp(request->path, "/images/new/", 12) == 0) {
    char decoded[MAX_PATH];
    (void)UrlDecode(decoded, sizeof(decoded), request->path + 12,
                    std::strlen(request->path + 12));
    char slug[MAX_SLUG];
    if (!Slugify(decoded, slug, sizeof(slug)) || !IsSafeSlug(slug)) {
      SetError(response, 400, "Invalid page slug");
      return;
    }

    TextBuffer html;
    if (!BuildImageUploadForm(config, slug, &html)) {
      SetError(response, 500, "Failed to build image upload form");
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
    char decoded[MAX_PATH];
    (void)UrlDecode(decoded, sizeof(decoded), request->path + 15,
                    std::strlen(request->path + 15));
    char slug[MAX_SLUG];
    if (!Slugify(decoded, slug, sizeof(slug)) || !IsSafeSlug(slug)) {
      SetError(response, 400, "Invalid page slug");
      return;
    }

    MultipartImage image;
    ParseMultipartImage(request, &image);
    if (!image.valid) {
      SetError(response, 400, "Missing image data");
      return;
    }

    int imageId = 0;
    char err[256];
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
    if (!BuildImageUploadedPage(config, slug, image.filename, imageId, &html)) {
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

    char decodedSlug[MAX_PATH];
    (void)UrlDecode(decodedSlug, sizeof(decodedSlug), slash + 1U,
                    std::strlen(slash + 1U));
    char slug[MAX_SLUG];
    if (!Slugify(decodedSlug, slug, sizeof(slug)) || !IsSafeSlug(slug)) {
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

  if (std::strcmp(request->method, "GET") == 0 &&
      std::strncmp(request->path, "/page/", 6) == 0) {
    char decoded[MAX_PATH];
    (void)UrlDecode(decoded, sizeof(decoded), request->path + 6,
                    std::strlen(request->path + 6));
    char slug[MAX_SLUG];
    if (!Slugify(decoded, slug, sizeof(slug)) || !IsSafeSlug(slug)) {
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
    if (!found) {
      if (!BuildMissingPageHtml(config, slug, &html)) {
        SetError(response, 500, "Failed to build missing page");
        return;
      }
    } else {
      if (!BuildViewHtml(db, config, &pageRecord, &html, err, sizeof(err))) {
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

bool SendResponse(int clientFd, const HttpResponse* response) {
  TextBuffer header;
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

  size_t sent = 0;
  while (sent < header.length) {
    const ssize_t n =
        send(clientFd, header.data + sent, header.length - sent, 0);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }

  sent = 0;
  while (sent < response->bodyLen) {
    const ssize_t n =
        send(clientFd, response->body + sent, response->bodyLen - sent, 0);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }

  return true;
}

void PrintUsage() {
  std::printf("mswiki options:\n");
  std::printf(
      "  --listen <addr>            Listen address (default: 0.0.0.0)\n");
  std::printf("  --port <port>              Listen port (default: 8080)\n");
  std::printf(
      "  --db <path>                SQLite database path (default: "
      "./mswiki.db)\n");
  std::printf(
      "  --assets <dir>             Optional customization directory\n");
  std::printf("  --max-body-bytes <bytes>   Maximum request body size\n");
  std::printf("  --self-test                Run in-process test suite\n");
  std::printf("  --help                     Show this help\n");
}

ArgParseResult ParseArgs(int argc, char** argv, Config* config) {
  (void)CopyString(config->listenHost, sizeof(config->listenHost), "0.0.0.0");
  config->port = DEFAULT_PORT;
  (void)CopyString(config->dbPath, sizeof(config->dbPath), "./mswiki.db");
  config->assetsPath[0] = '\0';
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
      config->port = std::atoi(value);
      if (config->port <= 0 || config->port > 65535) {
        std::fprintf(stderr, "Invalid port\n");
        return ARG_PARSE_ERROR;
      }
    } else if (std::strcmp(arg, "--db") == 0) {
      if (!CopyString(config->dbPath, sizeof(config->dbPath), value)) {
        std::fprintf(stderr, "db path too long\n");
        return ARG_PARSE_ERROR;
      }
    } else if (std::strcmp(arg, "--assets") == 0) {
      if (!CopyString(config->assetsPath, sizeof(config->assetsPath), value)) {
        std::fprintf(stderr, "assets path too long\n");
        return ARG_PARSE_ERROR;
      }
    } else if (std::strcmp(arg, "--max-body-bytes") == 0) {
      config->maxBodyBytes =
          static_cast<size_t>(std::strtoull(value, nullptr, 10));
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

}  // namespace

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
      continue;
    }

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

    HandleRequest(db, &config, &request, &response);
    (void)SendResponse(clientFd, &response);
    (void)close(clientFd);
  }

  (void)close(serverFd);
  (void)sqlite3_close(db);
  return 0;
}
