/*
 * Database initialization and schema migration logic.
 * This section depends on sqlite3 APIs and shared constants/helpers
 * defined in main.cpp (MAX_* limits, CopyString-compatible buffers).
 */

/*
 * Open SQLite database, enable pragmas, and apply schema migrations.
 */
bool DbOpen(sqlite3** db, const char* dbPath, char* err, size_t errSize) {
  if (db == nullptr) {
    (void)std::snprintf(err, errSize, "invalid database handle pointer");
    return false;
  }
  const int openFlags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  if (sqlite3_open_v2(dbPath, db, openFlags, nullptr) != SQLITE_OK) {
    const char* msg = "sqlite open failed";
    if (*db != nullptr) {
      msg = sqlite3_errmsg(*db);
      (void)sqlite3_close(*db);
      *db = nullptr;
    }
    (void)std::snprintf(err, errSize, "%s", msg);
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
    if (*db != nullptr) {
      (void)sqlite3_close(*db);
      *db = nullptr;
    }
    return false;
  }

  return true;
}
