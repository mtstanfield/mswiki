/*
 * In-process self-test suite for parser, renderer, database, and routing logic.
 * This section depends on nearly all server helpers and constants declared in
 * main.cpp and is executed via the --self-test runtime mode.
 */

/*
 * Record one self-test assertion result and report failures.
 */
void SelfTestExpect(SelfTestState* state, bool condition, const char* name) {
  if (condition) {
    state->passed += 1;
  } else {
    state->failed += 1;
    std::fprintf(stderr, "SELFTEST FAIL: %s\n", name);
  }
}

/*
 * Write an entire byte range to a file descriptor.
 */
bool WriteAllFd(int fd, const unsigned char* data, size_t len) {
  size_t sent = 0U;
  while (sent < len) {
    const ssize_t n = write(fd, data + sent, len - sent);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

/*
 * Execute unit-level tests for slug, URL codec, and integer parsing
 * helpers.
 */
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
  (void)UrlDecode(decoded, sizeof(decoded), "bad%GGvalue", 11U);
  SelfTestExpect(state, std::strcmp(decoded, "bad%GGvalue") == 0,
                 "UrlDecode leaves malformed escapes literal");
  (void)UrlDecode(decoded, sizeof(decoded), "bad%00value", 11U);
  SelfTestExpect(state, std::strcmp(decoded, "bad%00value") == 0,
                 "UrlDecode leaves encoded NUL literal");
  SelfTestExpect(state, !DecodeRouteSlugStrict("ok%00bad", slug, sizeof(slug)),
                 "DecodeRouteSlugStrict rejects encoded NUL");
  char longSlugInput[MAX_SLUG + 32U];
  (void)std::memset(longSlugInput, 'a', sizeof(longSlugInput));
  longSlugInput[sizeof(longSlugInput) - 1U] = '\0';
  SelfTestExpect(state,
                 !DecodeRouteSlugStrict(longSlugInput, slug, sizeof(slug)),
                 "DecodeRouteSlugStrict rejects decode truncation");

  int parsedPositive = 0;
  SelfTestExpect(state, ParsePositiveIntStrict("42", &parsedPositive),
                 "ParsePositiveIntStrict valid");
  SelfTestExpect(state, parsedPositive == 42, "ParsePositiveIntStrict value");
  SelfTestExpect(state, !ParsePositiveIntStrict("42x", &parsedPositive),
                 "ParsePositiveIntStrict rejects suffix");
}

/*
 * Verify markdown rendering, wiki-link extraction, footnotes, and MIME
 * sniffing.
 */
void SelfTestLinksAndMarkdown(SelfTestState* state) {
  char links[8][MAX_SLUG];
  size_t linkCount = 0;
  ExtractWikiLinks("See [[Target Page]] and [[target-page|label]]", links,
                   &linkCount, 8U);
  SelfTestExpect(state, linkCount == 1U, "ExtractWikiLinks deduplicates");
  SelfTestExpect(state, std::strcmp(links[0], "target-page") == 0,
                 "ExtractWikiLinks slugifies");

  static unsigned char arenaStorage[4U * MAX_RESPONSE_BYTES];
  RequestArena arena;
  ArenaReset(&arena, arenaStorage, sizeof(arenaStorage));
  TextBuffer html;
  if (!ArenaAllocTextBuffer(&arena, &html, MAX_RESPONSE_BYTES)) {
    SelfTestExpect(state, false, "RenderMarkdownToHtml arena alloc");
    return;
  }
  const bool rendered = RenderMarkdownToHtml(
      "# Title\nWiki [[Other Page|Label]][^n]\n"
      "**Bold** and *Italic* text\n"
      "***Both*** styles\n"
      "`inline` sample\n"
      "```\n"
      "if (a < b) {\n"
      "  return c;\n"
      "}\n"
      "```\n"
      "> quoted line\n"
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
  SelfTestExpect(state, std::strstr(html.data, "<strong>Bold</strong>") != nullptr,
                 "Render bold emphasis");
  SelfTestExpect(state, std::strstr(html.data, "<em>Italic</em>") != nullptr,
                 "Render italic emphasis");
  SelfTestExpect(state,
                 std::strstr(html.data, "<strong><em>Both</em></strong>") !=
                     nullptr,
                 "Render bold+italic emphasis");
  SelfTestExpect(state, std::strstr(html.data, "<code>inline</code>") != nullptr,
                 "Render inline code span");
  SelfTestExpect(
      state,
      std::strstr(html.data, "<pre><code>if (a &lt; b) {\n  return c;\n}"
                             "</code></pre>\n") != nullptr,
      "Render multiline fenced code block");
  SelfTestExpect(state,
                 std::strstr(html.data, "<blockquote>\n<p>quoted line</p>\n"
                                        "</blockquote>\n") != nullptr,
                 "Render blockquote");
  SelfTestExpect(
      state,
      std::strstr(html.data, "<section class=\"panel footnotes\">") != nullptr,
      "Render footnotes section");

  TextBuffer indentedHtml;
  if (!ArenaAllocTextBuffer(&arena, &indentedHtml, MAX_RESPONSE_BYTES)) {
    SelfTestExpect(state, false, "RenderMarkdownToHtml indented arena alloc");
    return;
  }
  const bool renderedIndented = RenderMarkdownToHtml(
      "  # Spaced Heading\n"
      "  [^n]: note text\n"
      "  ref[^n]\n"
      "  > quote line\n"
      "  ```\n"
      "  code line\n"
      "  ```\n",
      &indentedHtml);
  SelfTestExpect(state, renderedIndented,
                 "RenderMarkdownToHtml accepts leading-space literal text");
  SelfTestExpect(state,
                 std::strstr(indentedHtml.data, "<p>  # Spaced Heading</p>") !=
                     nullptr,
                 "Leading-space heading marker remains paragraph text");
  SelfTestExpect(state,
                 std::strstr(indentedHtml.data,
                             "<p>  [^n]: note text</p>") != nullptr,
                 "Leading-space footnote definition remains paragraph text");
  SelfTestExpect(state,
                 std::strstr(indentedHtml.data, "<p>  ref[^n]</p>") != nullptr,
                 "Leading-space footnote reference remains inline text");
  SelfTestExpect(state,
                 std::strstr(indentedHtml.data, "<p>  &gt; quote line</p>") !=
                     nullptr,
                 "Leading-space blockquote marker remains paragraph text");
  SelfTestExpect(state,
                 std::strstr(indentedHtml.data,
                             "<p>  ```</p>") != nullptr,
                 "Leading-space fence marker remains paragraph text");
  SelfTestExpect(state,
                 std::strstr(indentedHtml.data, "<p>  code line</p>") !=
                     nullptr,
                 "Leading-space code line remains paragraph text");

  static const unsigned char PNG_SIG[] = {0x89U, 0x50U, 0x4EU, 0x47U,
                                          0x0DU, 0x0AU, 0x1AU, 0x0AU};
  SelfTestExpect(state,
                 std::strcmp(DetectImageMimeFromData(PNG_SIG, sizeof(PNG_SIG)),
                             "image/png") == 0,
                 "DetectImageMimeFromData png");

  ArenaReset(&arena, arenaStorage, sizeof(arenaStorage));
  TextBuffer securityHtml;
  if (!ArenaAllocTextBuffer(&arena, &securityHtml, MAX_RESPONSE_BYTES)) {
    SelfTestExpect(state, false, "RenderMarkdownToHtml security arena alloc");
    return;
  }
  const bool renderedSecurity = RenderMarkdownToHtml(
      "[link](javascript:alert(1))\n"
      "![img](javascript:alert(1))\n"
      "[safe](https://example.com/a_(b))\n"
      "[^bad\"id]: note\n"
      "ref[^bad\"id]\n",
      &securityHtml);
  SelfTestExpect(state, renderedSecurity,
                 "RenderMarkdownToHtml security cases");
  SelfTestExpect(
      state, std::strstr(securityHtml.data, "href=\"javascript:") == nullptr,
      "Markdown link blocks javascript scheme");
  SelfTestExpect(state,
                 std::strstr(securityHtml.data, "src=\"javascript:") == nullptr,
                 "Markdown image blocks javascript scheme");
  SelfTestExpect(state,
                 std::strstr(securityHtml.data,
                             "href=\"https://example.com/a_(b)\"") != nullptr,
                 "Markdown link supports nested URL parentheses");
  SelfTestExpect(state, std::strstr(securityHtml.data, "bad\"id") == nullptr,
                 "Footnote id rejects unsafe characters");
}

/*
 * Validate URL form parsing and multipart image parsing behavior.
 */
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
  char tinyTitle[8];
  SelfTestExpect(
      state, !ParseFormField(&request, "title", tinyTitle, sizeof(tinyTitle)),
      "ParseFormField rejects decode truncation");

  (void)std::memset(&request, 0, sizeof(request));
  request.headerCount = 1;
  (void)CopyString(request.headers[0].key, sizeof(request.headers[0].key),
                   "content-type");
  (void)CopyString(request.headers[0].value, sizeof(request.headers[0].value),
                   "multipart/form-data; boundary=abc123; charset=utf-8");
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

/*
 * Exercise SQLite CRUD flow for pages, backlinks, and image lifecycle.
 */
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

  static unsigned char arenaStorage[MAX_RESPONSE_BYTES];
  RequestArena arena;
  ArenaReset(&arena, arenaStorage, sizeof(arenaStorage));
  TextBuffer backlinksHtml;
  if (!ArenaAllocTextBuffer(&arena, &backlinksHtml, MAX_RESPONSE_BYTES)) {
    SelfTestExpect(state, false, "DbAppendBacklinksHtml arena alloc");
    (void)sqlite3_close(db);
    (void)unlink(dbPath);
    return;
  }
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

  const bool insertImageAgainOk =
      DbInsertImage(db, "home", "tiny.gif", "image/gif", IMAGE_DATA,
                    sizeof(IMAGE_DATA), &imageId, err, sizeof(err));
  SelfTestExpect(state, insertImageAgainOk, "DbInsertImage second call");

  const bool deletePageOk = DbDeletePage(db, "home", err, sizeof(err));
  SelfTestExpect(state, deletePageOk, "DbDeletePage call");

  found = true;
  const bool getDeletedPageOk =
      DbGetPage(db, "home", &page, &found, err, sizeof(err));
  SelfTestExpect(state, getDeletedPageOk, "DbGetPage after delete call");
  SelfTestExpect(state, !found, "DbDeletePage removed page row");

  imageStmt = nullptr;
  found = true;
  const bool getImageAfterPageDeleteOk =
      DbGetImage(db, imageId, mime, sizeof(mime), &blobData, &blobLen,
                 &imageStmt, &found, err, sizeof(err));
  SelfTestExpect(state, getImageAfterPageDeleteOk,
                 "DbGetImage after page delete");
  SelfTestExpect(state, !found, "DbDeletePage removed page images");
  if (imageStmt != nullptr) {
    (void)sqlite3_finalize(imageStmt);
  }

  (void)sqlite3_close(db);
  (void)unlink(dbPath);
}

/*
 * Validate handler routing and HTTP request parser guardrails.
 */
void SelfTestHttpHandler(SelfTestState* state) {
  sqlite3* db = nullptr;
  char err[256];
  if (!DbOpen(&db, ":memory:", err, sizeof(err))) {
    SelfTestExpect(state, false, "DbOpen in-memory");
    return;
  }

  RequestArena arena;

  HttpRequest request;
  HttpResponse response;
  (void)std::memset(&request, 0, sizeof(request));
  (void)CopyString(request.method, sizeof(request.method), "GET");
  (void)CopyString(request.path, sizeof(request.path), "/page/home");
  HandleRequest(db, &arena, &request, &response);
  SelfTestExpect(state, response.status == 200, "HandleRequest GET /page/home");
  SelfTestExpect(state, response.bodyLen > 0U, "HandleRequest body exists");
  SelfTestExpect(state,
                 std::strstr(reinterpret_cast<const char*>(response.body),
                             "Create this page") != nullptr,
                 "Missing page response content");
  SelfTestExpect(
      state,
      std::strstr(reinterpret_cast<const char*>(response.body),
                  "rel=\"icon\" href=\"data:image/svg+xml") != nullptr,
      "Page layout includes inline favicon");

  (void)std::memset(&request, 0, sizeof(request));
  (void)CopyString(request.method, sizeof(request.method), "GET");
  (void)CopyString(request.path, sizeof(request.path), "/pages");
  HandleRequest(db, &arena, &request, &response);
  SelfTestExpect(state, response.status == 200, "HandleRequest GET /pages");
  SelfTestExpect(state,
                 std::strstr(reinterpret_cast<const char*>(response.body),
                             "All Pages") != nullptr,
                 "Pages response content");

  (void)std::memset(&request, 0, sizeof(request));
  (void)CopyString(request.method, sizeof(request.method), "GET");
  (void)CopyString(request.path, sizeof(request.path), "/edit/home");
  HandleRequest(db, &arena, &request, &response);
  SelfTestExpect(state, response.status == 200, "HandleRequest GET /edit/home");
  SelfTestExpect(state,
                 std::strstr(reinterpret_cast<const char*>(response.body),
                             "id=\"markdown-toolbar\"") != nullptr,
                 "Edit page renders markdown toolbar");
  SelfTestExpect(state,
                 std::strstr(reinterpret_cast<const char*>(response.body),
                             "data-md-action=\"bold\"") != nullptr,
                 "Edit page includes toolbar action buttons");
  SelfTestExpect(state,
                 std::strstr(reinterpret_cast<const char*>(response.body),
                             "data-md-action=\"codeblock\"") != nullptr,
                 "Edit page includes fenced code block toolbar button");
  SelfTestExpect(
      state,
      std::strstr(reinterpret_cast<const char*>(response.body),
                  "insertText:'```\\ncode block\\n```'") != nullptr,
      "Edit page code block action uses JS newline escapes");
  SelfTestExpect(
      state,
      std::strstr(reinterpret_cast<const char*>(response.body),
                  "insertText:'[^note]\\n\\n[^note]: note text'") != nullptr,
      "Edit page footnote action uses JS newline escapes");
  SelfTestExpect(state,
                 std::strstr(reinterpret_cast<const char*>(response.body),
                             "data-md-action=\"quote\"") == nullptr,
                 "Edit page omits blockquote toolbar button");
  SelfTestExpect(state,
                 std::strstr(reinterpret_cast<const char*>(response.body),
                             "Images for this page") != nullptr,
                 "Edit page includes image list section");
  SelfTestExpect(state,
                 std::strstr(reinterpret_cast<const char*>(response.body),
                             "delete-button md-insert-image") == nullptr,
                 "Edit page delete controls are not insert targets");

  const bool createHomeForDelete =
      DbUpsertPage(db, "home", "Home", "Body", err, sizeof(err));
  SelfTestExpect(state, createHomeForDelete, "DbUpsertPage for delete route");

  static const unsigned char IMAGE_DATA[] = {0x47U, 0x49U, 0x46U};
  int editImageId = 0;
  const bool insertEditImageOk =
      DbInsertImage(db, "home", "tiny.gif", "image/gif", IMAGE_DATA,
                    sizeof(IMAGE_DATA), &editImageId, err, sizeof(err));
  SelfTestExpect(state, insertEditImageOk, "DbInsertImage for edit html");
  (void)editImageId;
  (void)std::memset(&request, 0, sizeof(request));
  (void)CopyString(request.method, sizeof(request.method), "GET");
  (void)CopyString(request.path, sizeof(request.path), "/edit/home");
  HandleRequest(db, &arena, &request, &response);
  SelfTestExpect(state, response.status == 200,
                 "HandleRequest GET /edit/home with image");
  SelfTestExpect(
      state,
      std::strstr(reinterpret_cast<const char*>(response.body),
                  "name=\"markdown\" required>Body</textarea>") != nullptr,
      "Edit page preserves textarea markdown without layout indentation");
  SelfTestExpect(
      state,
      std::strstr(reinterpret_cast<const char*>(response.body),
                  "\n      </textarea>\n") != nullptr,
      "Edit page aligns closing textarea tag");
  SelfTestExpect(state,
                 std::strstr(reinterpret_cast<const char*>(response.body),
                             "\n      </script>\n") != nullptr,
                 "Edit page aligns closing script tag");
  SelfTestExpect(
      state,
      std::strstr(reinterpret_cast<const char*>(response.body),
                  "class=\"action-button button-secondary md-insert-image\"") !=
          nullptr,
      "Edit page insert image button retains insert class");
  SelfTestExpect(state,
                 std::strstr(reinterpret_cast<const char*>(response.body),
                             "class=\"action-button delete-button\"") != nullptr,
                 "Edit page delete image button uses delete class");
  SelfTestExpect(state,
                 std::strstr(reinterpret_cast<const char*>(response.body),
                             "delete-button md-insert-image") == nullptr,
                 "Edit page delete image button not tagged as insert class");

  (void)std::memset(&request, 0, sizeof(request));
  (void)CopyString(request.method, sizeof(request.method), "GET");
  (void)CopyString(request.path, sizeof(request.path), "/page/home");
  HandleRequest(db, &arena, &request, &response);
  static const unsigned char DELETE_PAGE_TEXT[] = "Delete page";
  static const unsigned char DELETE_IMAGE_ROUTE[] = "/images/delete/";
  SelfTestExpect(state,
                 FindBytes(response.body, response.bodyLen, DELETE_PAGE_TEXT,
                           sizeof(DELETE_PAGE_TEXT) - 1U) == nullptr,
                 "View page does not render delete page button");
  SelfTestExpect(state,
                 FindBytes(response.body, response.bodyLen, DELETE_IMAGE_ROUTE,
                           sizeof(DELETE_IMAGE_ROUTE) - 1U) == nullptr,
                 "View page does not render delete image actions");
  (void)std::memset(&request, 0, sizeof(request));
  (void)CopyString(request.method, sizeof(request.method), "POST");
  (void)CopyString(request.path, sizeof(request.path), "/delete/home");
  HandleRequest(db, &arena, &request, &response);
  SelfTestExpect(state, response.status == 302,
                 "HandleRequest POST /delete/home");
  SelfTestExpect(state, std::strcmp(response.location, "/pages") == 0,
                 "Delete page redirects to /pages");
  (void)std::memset(&request, 0, sizeof(request));
  (void)CopyString(request.method, sizeof(request.method), "GET");
  (void)CopyString(request.path, sizeof(request.path), "/page/home");
  HandleRequest(db, &arena, &request, &response);
  SelfTestExpect(state,
                 std::strstr(reinterpret_cast<const char*>(response.body),
                             "Create this page") != nullptr,
                 "Deleted page behaves as missing");
  (void)std::memset(&request, 0, sizeof(request));
  (void)CopyString(request.method, sizeof(request.method), "POST");
  (void)CopyString(request.path, sizeof(request.path), "/delete/%00");
  HandleRequest(db, &arena, &request, &response);
  SelfTestExpect(state, response.status == 400,
                 "HandleRequest rejects encoded-NUL slug");

  HttpRequest parsedRequest;
  (void)std::memset(&parsedRequest, 0, sizeof(parsedRequest));
  SelfTestExpect(state, !ParseRequestLine("GET / HTTP/2", &parsedRequest),
                 "ParseRequestLine rejects HTTP/2");
  SelfTestExpect(state, ParseRequestLine("GET / HTTP/1.1", &parsedRequest),
                 "ParseRequestLine accepts HTTP/1.1");

  unsigned char rawNoHost[] = "GET / HTTP/1.1\r\nUser-Agent: x\r\n\r\n";
  const unsigned char* bodyStart = nullptr;
  size_t bodyOffset = 0U;
  SelfTestExpect(state,
                 !ParseHttpRequest(rawNoHost, sizeof(rawNoHost) - 1U,
                                   &parsedRequest, &bodyStart, &bodyOffset),
                 "ParseHttpRequest rejects HTTP/1.1 without host");

  unsigned char rawWithHost[] = "GET / HTTP/1.1\r\nHost: wiki.local\r\n\r\n";
  SelfTestExpect(state,
                 ParseHttpRequest(rawWithHost, sizeof(rawWithHost) - 1U,
                                  &parsedRequest, &bodyStart, &bodyOffset),
                 "ParseHttpRequest accepts HTTP/1.1 with host");
  unsigned char rawBadHeader[] =
      "GET / HTTP/1.1\r\nHost: wiki.local\r\nXBad\r\n\r\n";
  SelfTestExpect(state,
                 !ParseHttpRequest(rawBadHeader, sizeof(rawBadHeader) - 1U,
                                   &parsedRequest, &bodyStart, &bodyOffset),
                 "ParseHttpRequest rejects malformed header line");
  unsigned char rawBadHeaderName[] =
      "GET / HTTP/1.1\r\nHost: wiki.local\r\nBad Header: x\r\n\r\n";
  SelfTestExpect(
      state,
      !ParseHttpRequest(rawBadHeaderName, sizeof(rawBadHeaderName) - 1U,
                        &parsedRequest, &bodyStart, &bodyOffset),
      "ParseHttpRequest rejects invalid header-name token");
  SelfTestExpect(state,
                 !ParseRequestLine("GET noslash HTTP/1.1", &parsedRequest),
                 "ParseRequestLine rejects non-origin-form target");
  unsigned char rawEmbeddedNul[] = {
      'G', 'E',  'T',  ' ', '/', ' ', 'H', 'T',  'T',  'P',  '/', '1',  '.',
      '1', '\r', '\n', 'H', 'o', 's', 't', ':',  ' ',  'w',  'i', '\0', 'k',
      'i', '.',  'l',  'o', 'c', 'a', 'l', '\r', '\n', '\r', '\n'};
  SelfTestExpect(state,
                 !ParseHttpRequest(rawEmbeddedNul, sizeof(rawEmbeddedNul),
                                   &parsedRequest, &bodyStart, &bodyOffset),
                 "ParseHttpRequest rejects embedded NUL in headers");

  HttpRequest badImageIdRequest;
  HttpResponse badImageIdResponse;
  (void)std::memset(&badImageIdRequest, 0, sizeof(badImageIdRequest));
  (void)CopyString(badImageIdRequest.method, sizeof(badImageIdRequest.method),
                   "GET");
  (void)CopyString(badImageIdRequest.path, sizeof(badImageIdRequest.path),
                   "/image/1abc");
  HandleRequest(db, &arena, &badImageIdRequest, &badImageIdResponse);
  SelfTestExpect(state, badImageIdResponse.status == 400,
                 "HandleRequest rejects /image suffix garbage");

  HttpRequest badDeleteIdRequest;
  HttpResponse badDeleteIdResponse;
  (void)std::memset(&badDeleteIdRequest, 0, sizeof(badDeleteIdRequest));
  (void)CopyString(badDeleteIdRequest.method, sizeof(badDeleteIdRequest.method),
                   "POST");
  (void)CopyString(badDeleteIdRequest.path, sizeof(badDeleteIdRequest.path),
                   "/images/delete/1abc/home");
  HandleRequest(db, &arena, &badDeleteIdRequest, &badDeleteIdResponse);
  SelfTestExpect(state, badDeleteIdResponse.status == 400,
                 "HandleRequest rejects /images/delete suffix garbage");

  HttpRequest logoRequest;
  HttpResponse logoResponse;
  (void)std::memset(&logoRequest, 0, sizeof(logoRequest));
  (void)CopyString(logoRequest.method, sizeof(logoRequest.method), "GET");
  (void)CopyString(logoRequest.path, sizeof(logoRequest.path), "/logo");
  HandleRequest(db, &arena, &logoRequest, &logoResponse);
  SelfTestExpect(state, logoResponse.status == 404,
                 "HandleRequest no longer serves /logo");

  (void)sqlite3_close(db);
}

/*
 * Verify socket-level request framing checks with realistic read
 * paths.
 */
void SelfTestSocketRequestValidation(SelfTestState* state) {
  struct {
    const char* name;
    const char* raw;
    bool expectSuccess;
    bool expectTooLarge;
  } cases[] = {
      {"GET no content-length allowed",
       "GET / HTTP/1.1\r\nHost: wiki.local\r\n\r\n", true, false},
      {"POST requires content-length",
       "POST /save/home HTTP/1.1\r\nHost: wiki.local\r\n\r\n"
       "title=x&markdown=y",
       false, false},
      {"Transfer-Encoding rejected",
       "POST /save/home HTTP/1.1\r\nHost: wiki.local\r\nTransfer-Encoding: "
       "chunked\r\nContent-Length: 4\r\n\r\n"
       "test",
       false, false},
      {"Duplicate mismatched content-length rejected",
       "POST /save/home HTTP/1.1\r\nHost: wiki.local\r\nContent-Length: 4\r\n"
       "Content-Length: 7\r\n\r\n"
       "test",
       false, false},
      {"Duplicate matching content-length allowed",
       "POST /save/home HTTP/1.1\r\nHost: wiki.local\r\nContent-Length: 4\r\n"
       "Content-Length: 4\r\n\r\n"
       "test",
       true, false},
      {"Too large content-length flagged",
       "POST /save/home HTTP/1.1\r\nHost: wiki.local\r\nContent-Length: "
       "10485761\r\n\r\n",
       false, true},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    int fds[2] = {-1, -1};
    const int sp = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    if (sp != 0) {
      SelfTestExpect(state, false, cases[i].name);
      continue;
    }

    const unsigned char* raw =
        reinterpret_cast<const unsigned char*>(cases[i].raw);
    const size_t rawLen = std::strlen(cases[i].raw);
    const bool writeOk = WriteAllFd(fds[0], raw, rawLen);
    (void)shutdown(fds[0], SHUT_WR);
    if (!writeOk) {
      (void)close(fds[0]);
      (void)close(fds[1]);
      SelfTestExpect(state, false, cases[i].name);
      continue;
    }

    static unsigned char scratch[MAX_REQUEST_BYTES];

    size_t outLen = 0U;
    bool tooLarge = false;
    const bool ok = ReadRequestFromSocket(fds[1], MAX_BODY_BYTES, scratch,
                                          &outLen, &tooLarge);
    (void)close(fds[0]);
    (void)close(fds[1]);

    const bool pass =
        (ok == cases[i].expectSuccess) && (tooLarge == cases[i].expectTooLarge);
    SelfTestExpect(state, pass, cases[i].name);
  }
}

/*
 * Verify command-line parsing accepts currently supported flags.
 */
void SelfTestParseArgs(SelfTestState* state) {
  Config config;

  char progName[] = "mswiki";
  char dbFlag[] = "--db";
  char dbPath[] = "/tmp/wiki.db";
  char maxBodyFlag[] = "--max-body-bytes";
  char maxBodyValue[] = "1024";
  char* validArgv[] = {progName, dbFlag, dbPath, maxBodyFlag, maxBodyValue};
  const ArgParseResult validResult =
      ParseArgs(static_cast<int>(sizeof(validArgv) / sizeof(validArgv[0])),
                validArgv, &config);
  SelfTestExpect(state, validResult == ARG_PARSE_OK,
                 "ParseArgs accepts supported flags");
  SelfTestExpect(state, std::strcmp(config.dbPath, "/tmp/wiki.db") == 0,
                 "ParseArgs sets db path");
  SelfTestExpect(state, config.maxBodyBytes == 1024U,
                 "ParseArgs sets max body size");
}

/*
 * Verify response body copy preserves bytes and respects buffer limits.
 */
void SelfTestResponseCopy(SelfTestState* state) {
  HttpResponse response;
  static const char HTML_BODY[] =
      "<script>if(a<b){window.x='</not-a-tag>';}</script><p>ok</p>";
  const bool htmlOk = SetResponseCopy(
      &response, 200, "text/html; charset=utf-8", HTML_BODY,
      sizeof(HTML_BODY) - 1U);
  SelfTestExpect(state, htmlOk, "SetResponseCopy accepts HTML body");
  SelfTestExpect(state, response.bodyLen == (sizeof(HTML_BODY) - 1U),
                 "SetResponseCopy preserves HTML length");
  SelfTestExpect(
      state,
      std::memcmp(response.body, HTML_BODY, sizeof(HTML_BODY) - 1U) == 0,
      "SetResponseCopy preserves HTML bytes");

  static const char TEXT_BODY[] = "plain text";
  const bool textOk = SetResponseCopy(
      &response, 200, "text/plain; charset=utf-8", TEXT_BODY,
      sizeof(TEXT_BODY) - 1U);
  SelfTestExpect(state, textOk, "SetResponseCopy accepts text body");
  SelfTestExpect(state, response.bodyLen == (sizeof(TEXT_BODY) - 1U),
                 "SetResponseCopy preserves text length");
  SelfTestExpect(
      state, std::memcmp(response.body, TEXT_BODY, sizeof(TEXT_BODY) - 1U) == 0,
      "SetResponseCopy preserves text bytes");
}

/*
 * Run full built-in self-test suite and print aggregate result.
 */
int RunSelfTests() {
  SelfTestState state;
  state.passed = 0;
  state.failed = 0;

  SelfTestSlugAndCodec(&state);
  SelfTestLinksAndMarkdown(&state);
  SelfTestFormAndMultipart(&state);
  SelfTestDatabase(&state);
  SelfTestHttpHandler(&state);
  SelfTestSocketRequestValidation(&state);
  SelfTestParseArgs(&state);
  SelfTestResponseCopy(&state);

  std::printf("Self-test results: passed=%d failed=%d\n", state.passed,
              state.failed);
  return (state.failed == 0) ? 0 : 1;
}
