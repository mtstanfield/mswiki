/*
 * Document metadata and blob database operations.
 * This section depends on sqlite3 APIs, TextBuffer, and shared helpers
 * from main.cpp (BufferAppend*, UrlEncode, CopyString, and time helpers).
 */

/*
 * Append page-document list HTML with link, insert, and delete actions.
 */
bool DbAppendDocumentsHtml(sqlite3* db,
                           const char* slug,
                           TextBuffer* html,
                           bool includeInsertButtons,
                           char* err,
                           size_t errSize) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT id, filename FROM documents WHERE page_slug = ? ORDER BY id DESC";
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

    const int id = sqlite3_column_int(stmt, 0);
    const char* filename =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (filename == nullptr) {
      continue;
    }

    if (!any) {
      if (!BufferAppend(html, "<ul class=\"documents\">\n")) {
        (void)sqlite3_finalize(stmt);
        return false;
      }
      any = true;
    }

    char encodedSlug[MAX_PATH];
    if (!UrlEncode(slug, encodedSlug, sizeof(encodedSlug))) {
      continue;
    }

    if (!BufferAppendFormat(
            html,
            "<li>\n<a href=\"/document/%d\" target=\"_blank\" rel=\"noopener\">",
            id)) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
    if (!BufferAppendEscaped(html, filename, std::strlen(filename))) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
    if (!BufferAppend(html, "</a> <span class=\"pdf-tag\">[pdf]</span>")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }

    char snippet[512];
    if (std::snprintf(snippet, sizeof(snippet), "[%s](/document/%d)", filename,
                      id) < 0) {
      (void)sqlite3_finalize(stmt);
      return false;
    }

    if (includeInsertButtons) {
      if (!BufferAppend(html, "\n")) {
        (void)sqlite3_finalize(stmt);
        return false;
      }
      if (!BufferAppend(html,
                        "<button type=\"button\" "
                        "class=\"action-button button-secondary "
                        "md-insert-document\" "
                        "data-md-snippet=\"")) {
        (void)sqlite3_finalize(stmt);
        return false;
      }
      if (!BufferAppendEscaped(html, snippet, std::strlen(snippet))) {
        (void)sqlite3_finalize(stmt);
        return false;
      }
      if (!BufferAppend(html, "\">Insert</button>\n")) {
        (void)sqlite3_finalize(stmt);
        return false;
      }
      if (!BufferAppendFormat(
              html,
              "<form class=\"inline-delete\" method=\"post\" "
              "onsubmit=\"return confirm('Delete this document?');\" "
              "action=\"/documents/delete/%d/",
              id)) {
        (void)sqlite3_finalize(stmt);
        return false;
      }
      if (!BufferAppend(html, encodedSlug)) {
        (void)sqlite3_finalize(stmt);
        return false;
      }
      if (!BufferAppend(html,
                        "\">\n<button type=\"submit\" "
                        "class=\"action-button delete-button\">"
                        "Delete document</button>\n</form>")) {
        (void)sqlite3_finalize(stmt);
        return false;
      }
    }
    if (!BufferAppend(html, "\n</li>\n")) {
      (void)sqlite3_finalize(stmt);
      return false;
    }
  }

  if (!any) {
    if (!BufferAppend(
            html,
            "<p class=\"meta\">No documents uploaded for this page.</p>\n")) {
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

/*
 * Insert a new document blob and metadata row into SQLite.
 */
bool DbInsertDocument(sqlite3* db,
                      const char* pageSlug,
                      const char* filename,
                      const char* mimeType,
                      const unsigned char* data,
                      size_t dataLen,
                      int* documentId,
                      char* err,
                      size_t errSize) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO documents (page_slug, filename, mime_type, data, created_at) "
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

  *documentId = static_cast<int>(sqlite3_last_insert_rowid(db));
  (void)sqlite3_finalize(stmt);
  return true;
}

/*
 * Load one document blob row by numeric id.
 */
bool DbGetDocument(sqlite3* db,
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
  const char* sql = "SELECT mime_type, data FROM documents WHERE id = ?";
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

  const unsigned char* dbMime = sqlite3_column_text(stmt, 0);
  const void* dbData = sqlite3_column_blob(stmt, 1);
  const int dbDataLen = sqlite3_column_bytes(stmt, 1);
  if (dbMime == nullptr || dbData == nullptr || dbDataLen <= 0) {
    (void)sqlite3_finalize(stmt);
    (void)std::snprintf(err, errSize, "invalid document row");
    return false;
  }
  if (!CopyString(mimeType, mimeTypeSize, reinterpret_cast<const char*>(dbMime))) {
    (void)sqlite3_finalize(stmt);
    (void)std::snprintf(err, errSize, "mime type too long");
    return false;
  }

  *data = reinterpret_cast<const unsigned char*>(dbData);
  *dataLen = dbDataLen;
  *stmtOut = stmt;
  *found = true;
  return true;
}

/*
 * Delete a document row scoped to the owning page slug.
 */
bool DbDeleteDocument(sqlite3* db,
                      int id,
                      const char* pageSlug,
                      char* err,
                      size_t errSize) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "DELETE FROM documents WHERE id = ? AND page_slug = ?";
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
