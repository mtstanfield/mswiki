/*
 * Page and backlink database operations.
 * This section depends on sqlite3 APIs, PageRecord/TextBuffer types,
 * and shared helpers declared in main.cpp (CopyString, BufferAppend*,
 * UrlEncode, ExtractWikiLinks, and timestamp helpers).
 */

/*
 * Load a wiki page row by slug from SQLite.
 */
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

/*
 * Insert or update a page and refresh its outbound wiki-link index.
 */
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

/*
 * Delete a page and all direct page-owned data by slug.
 */
bool DbDeletePage(sqlite3* db, const char* slug, char* err, size_t errSize) {
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

  struct DeleteStep {
    const char* sql;
    int bindCount;
  };
  static const DeleteStep STEPS[] = {
      {"DELETE FROM page_links WHERE from_slug = ?", 1},
      {"DELETE FROM page_links WHERE to_slug = ?", 1},
      {"DELETE FROM images WHERE page_slug = ?", 1},
      {"DELETE FROM documents WHERE page_slug = ?", 1},
      {"DELETE FROM pages WHERE slug = ?", 1},
  };

  for (size_t i = 0; i < sizeof(STEPS) / sizeof(STEPS[0]); i++) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, STEPS[i].sql, -1, &stmt, nullptr) != SQLITE_OK) {
      (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
      (void)sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
    if (STEPS[i].bindCount == 1) {
      (void)sqlite3_bind_text(stmt, 1, slug, -1, SQLITE_TRANSIENT);
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
      (void)sqlite3_finalize(stmt);
      (void)sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
    (void)sqlite3_finalize(stmt);
  }

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

/*
 * Query all pages and append HTML list items for index rendering.
 */
bool DbListPagesHtml(sqlite3* db, TextBuffer* html, char* err, size_t errSize) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT slug, title FROM pages ORDER BY lower(title)";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    (void)std::snprintf(err, errSize, "%s", sqlite3_errmsg(db));
    return false;
  }

  bool done = false;
  while (!done) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
      done = true;
      continue;
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
    if (!BufferAppend(html, ")</span></li>\n")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
  }

  (void)sqlite3_finalize(stmt);
  return true;
}

/*
 * Append backlink list HTML for pages linking to `slug`.
 */
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
  bool done = false;
  while (!done) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
      done = true;
      continue;
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
      if (!BufferAppend(html, "<ul class=\"backlinks\">\n")) {
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
    if (!BufferAppend(html, "</a></li>\n")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
  }

  if (!any) {
    if (!BufferAppend(html, "<p class=\"meta\">No backlinks yet.</p>\n")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
  } else {
    if (!BufferAppend(html, "</ul>\n")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
  }

  (void)sqlite3_finalize(stmt);
  return true;
}
