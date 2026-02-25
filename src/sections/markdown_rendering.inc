/*
 * Markdown rendering helpers for headings, lists, links, sidenotes, and images.
 * This section depends on TextBuffer/MAX_* constants and shared escaping/slug
 * helpers defined in main.cpp.
 */

/*
 * Parse a markdown footnote-definition line (`[^id]: text`).
 */
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
    const char ch = line[i];
    const bool safeIdChar =
        ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') || ch == '-' || ch == '_');
    if (!safeIdChar) {
      return false;
    }
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

/*
 * Determine if markdown URL target uses a permitted scheme/prefix.
 */
bool IsSafeMarkdownUrl(const char* url, size_t len, bool allowMailto) {
  if (url == nullptr || len == 0U) {
    return false;
  }
  size_t start = 0U;
  while (start < len && (url[start] == ' ' || url[start] == '\t')) {
    start++;
  }
  while (len > start && (url[len - 1U] == ' ' || url[len - 1U] == '\t')) {
    len--;
  }
  if (start >= len) {
    return false;
  }

  const char first = url[start];
  if (first == '/' || first == '#') {
    return true;
  }
  if (len - start >= 2U && url[start] == '.' && url[start + 1U] == '/') {
    return true;
  }
  if (len - start >= 3U && url[start] == '.' && url[start + 1U] == '.' &&
      url[start + 2U] == '/') {
    return true;
  }

  size_t colonPos = static_cast<size_t>(-1);
  for (size_t i = start; i < len; i++) {
    const char ch = url[i];
    if (ch == ':') {
      colonPos = i;
      break;
    }
    if (ch == '/' || ch == '?' || ch == '#') {
      break;
    }
  }
  if (colonPos == static_cast<size_t>(-1)) {
    return true;
  }

  char scheme[16];
  const size_t schemeLen = colonPos - start;
  if (schemeLen == 0U || schemeLen + 1U > sizeof(scheme)) {
    return false;
  }
  for (size_t i = 0; i < schemeLen; i++) {
    scheme[i] = static_cast<char>(ToLowerAscii(url[start + i]));
  }
  scheme[schemeLen] = '\0';

  if (std::strcmp(scheme, "http") == 0 || std::strcmp(scheme, "https") == 0) {
    return true;
  }
  if (allowMailto && std::strcmp(scheme, "mailto") == 0) {
    return true;
  }
  return false;
}

/*
 * Find closing `)` for markdown link/image target with nested parens.
 */
bool FindMarkdownLinkCloseParen(const char* text,
                                size_t len,
                                size_t start,
                                size_t* closeParenOut) {
  size_t depth = 0U;
  for (size_t i = start; i < len; i++) {
    if (text[i] == '\\' && i + 1U < len) {
      i++;
      continue;
    }
    if (text[i] == '(') {
      depth++;
      continue;
    }
    if (text[i] == ')') {
      if (depth == 0U) {
        *closeParenOut = i;
        return true;
      }
      depth--;
    }
  }
  return false;
}

/*
 * Find footnote identifier index in the collected footnote table.
 */
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

/*
 * Check whether markdown URL target is a strict `/document/<id>` path.
 */
bool IsDocumentLinkTarget(const char* url, size_t len) {
  if (url == nullptr || len <= 10U || std::strncmp(url, "/document/", 10U) != 0) {
    return false;
  }
  const size_t idLen = len - 10U;
  char idText[32];
  if (idLen + 1U > sizeof(idText)) {
    return false;
  }
  (void)std::memcpy(idText, url + 10U, idLen);
  idText[idLen] = '\0';
  int documentId = 0;
  return ParsePositiveIntStrict(idText, &documentId);
}

/*
 * Find the next unescaped `*` or `**` run that closes emphasis markup.
 */
bool FindClosingAsteriskRun(const char* text,
                            size_t len,
                            size_t start,
                            size_t runLen,
                            size_t* closeOut) {
  if (runLen != 1U && runLen != 2U && runLen != 3U) {
    return false;
  }
  for (size_t i = start; i < len; i++) {
    if (text[i] == '\\' && i + 1U < len) {
      i++;
      continue;
    }
    if (runLen == 3U) {
      if (i + 2U < len && text[i] == '*' && text[i + 1U] == '*' &&
          text[i + 2U] == '*') {
        *closeOut = i;
        return true;
      }
      continue;
    }
    if (runLen == 2U) {
      if (i + 1U < len && text[i] == '*' && text[i + 1U] == '*') {
        *closeOut = i;
        return true;
      }
      continue;
    }
    if (text[i] == '*') {
      const bool previousIsStar = (i > start && text[i - 1U] == '*');
      const bool nextIsStar = (i + 1U < len && text[i + 1U] == '*');
      if (!previousIsStar && !nextIsStar) {
        *closeOut = i;
        return true;
      }
    }
  }
  return false;
}

/*
 * Find closing backtick for an inline code span.
 */
bool FindClosingBacktick(const char* text,
                         size_t len,
                         size_t start,
                         size_t* closeOut) {
  for (size_t i = start; i < len; i++) {
    if (text[i] == '\\' && i + 1U < len) {
      i++;
      continue;
    }
    if (text[i] == '`') {
      *closeOut = i;
      return true;
    }
  }
  return false;
}

/*
 * Convert one markdown line into inline HTML with links/notes/images.
 */
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
    if (i + 1U < len && text[i] == '\\' &&
        (text[i + 1U] == '*' || text[i + 1U] == '`')) {
      if (!BufferAppendEscaped(out, text + i + 1U, 1U)) {
        return false;
      }
      i += 2U;
      continue;
    }

    if (i + 5U < len && text[i] == '!' && text[i + 1U] == '[') {
      size_t closeBracket = i + 2U;
      while (closeBracket < len && text[closeBracket] != ']') {
        closeBracket++;
      }
      if (closeBracket + 1U < len && text[closeBracket + 1U] == '(') {
        size_t closeParen = 0U;
        if (FindMarkdownLinkCloseParen(text, len, closeBracket + 2U,
                                       &closeParen)) {
          const size_t urlLen = closeParen - (closeBracket + 2U);
          if (!IsSafeMarkdownUrl(text + closeBracket + 2U, urlLen, false)) {
            if (!BufferAppendEscaped(out, text + i, closeParen + 1U - i)) {
              return false;
            }
            i = closeParen + 1U;
            continue;
          }
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

    if (i + 5U < len && text[i] == '*' && text[i + 1U] == '*' &&
        text[i + 2U] == '*') {
      size_t close = 0U;
      if (FindClosingAsteriskRun(text, len, i + 3U, 3U, &close) &&
          close > i + 3U) {
        if (!BufferAppend(out, "<strong><em>")) {
          return false;
        }
        if (!WriteInlineMarkdown(out, text + i + 3U, close - (i + 3U),
                                 footnoteIds, footnoteText, footnoteCount,
                                 footnoteOrder, nextFootnoteNumber)) {
          return false;
        }
        if (!BufferAppend(out, "</em></strong>")) {
          return false;
        }
        i = close + 3U;
        continue;
      }
    }

    if (i + 2U < len && text[i] == '`') {
      size_t close = 0U;
      if (FindClosingBacktick(text, len, i + 1U, &close) && close > i + 1U) {
        if (!BufferAppend(out, "<code>")) {
          return false;
        }
        if (!BufferAppendEscaped(out, text + i + 1U, close - (i + 1U))) {
          return false;
        }
        if (!BufferAppend(out, "</code>")) {
          return false;
        }
        i = close + 1U;
        continue;
      }
    }

    if (i + 3U < len && text[i] == '*' && text[i + 1U] == '*') {
      size_t close = 0U;
      if (FindClosingAsteriskRun(text, len, i + 2U, 2U, &close) &&
          close > i + 2U) {
        if (!BufferAppend(out, "<strong>")) {
          return false;
        }
        if (!WriteInlineMarkdown(out, text + i + 2U, close - (i + 2U),
                                 footnoteIds, footnoteText, footnoteCount,
                                 footnoteOrder, nextFootnoteNumber)) {
          return false;
        }
        if (!BufferAppend(out, "</strong>")) {
          return false;
        }
        i = close + 2U;
        continue;
      }
    }

    if (i + 2U < len && text[i] == '*') {
      size_t close = 0U;
      if (FindClosingAsteriskRun(text, len, i + 1U, 1U, &close) &&
          close > i + 1U) {
        if (!BufferAppend(out, "<em>")) {
          return false;
        }
        if (!WriteInlineMarkdown(out, text + i + 1U, close - (i + 1U),
                                 footnoteIds, footnoteText, footnoteCount,
                                 footnoteOrder, nextFootnoteNumber)) {
          return false;
        }
        if (!BufferAppend(out, "</em>")) {
          return false;
        }
        i = close + 1U;
        continue;
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
        size_t closeParen = 0U;
        if (FindMarkdownLinkCloseParen(text, len, closeBracket + 2U,
                                       &closeParen)) {
          const size_t urlLen = closeParen - (closeBracket + 2U);
          const bool isDocumentLink =
              IsDocumentLinkTarget(text + closeBracket + 2U, urlLen);
          if (!IsSafeMarkdownUrl(text + closeBracket + 2U, urlLen, true)) {
            if (!BufferAppendEscaped(out, text + i, closeParen + 1U - i)) {
              return false;
            }
            i = closeParen + 1U;
            continue;
          }
          if (!BufferAppend(out, "<a href=\"")) {
            return false;
          }
          if (!BufferAppendEscaped(out, text + closeBracket + 2U,
                                   closeParen - (closeBracket + 2U))) {
            return false;
          }
          if (isDocumentLink) {
            if (!BufferAppend(out, "\" target=\"_blank\" rel=\"noopener\">")) {
              return false;
            }
          } else if (!BufferAppend(out, "\">")) {
            return false;
          }
          if (!BufferAppendEscaped(out, text + i + 1U,
                                   closeBracket - (i + 1U))) {
            return false;
          }
          if (!BufferAppend(out, "</a>")) {
            return false;
          }
          if (isDocumentLink) {
            if (!BufferAppend(out, "<span class=\"pdf-tag\">[pdf]</span>")) {
              return false;
            }
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

/*
 * Close active list state, including nested sublist and parent list item.
 */
bool CloseListBlocks(TextBuffer* html,
                     bool* inList,
                     bool* inTopListItem,
                     bool* inSublist) {
  if (!*inList) {
    return true;
  }
  if (*inSublist) {
    if (!BufferAppend(html, "</ul>\n")) {
      return false;
    }
    *inSublist = false;
  }
  if (*inTopListItem) {
    if (!BufferAppend(html, "</li>\n")) {
      return false;
    }
    *inTopListItem = false;
  }
  if (!BufferAppend(html, "</ul>\n")) {
    return false;
  }
  *inList = false;
  return true;
}

/*
 * Render supported markdown subset to HTML, including wiki links,
 * sidenotes, and margin images.
 * Parsing is strict for block markers: headings/lists/quotes/fences/footnote
 * definitions are recognized only when the marker starts at column 1.
 */
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
  bool codeBlockHasLine = false;
  bool inList = false;
  bool inTopListItem = false;
  bool inSublist = false;
  bool inBlockquote = false;

  while (lineStart <= len) {
    size_t lineEnd = lineStart;
    while (lineEnd < len && markdown[lineEnd] != '\n') {
      lineEnd++;
    }

    size_t lineLen = lineEnd - lineStart;
    if (lineLen > 0U && markdown[lineEnd - 1U] == '\r') {
      lineLen--;
    }

    const char* lineRaw = markdown + lineStart;
    const char* line = lineRaw;
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
      if (!CloseListBlocks(html, &inList, &inTopListItem, &inSublist)) {
        return false;
      }
      if (inBlockquote) {
        if (!BufferAppend(html, "</blockquote>\n")) {
          return false;
        }
        inBlockquote = false;
      }
      if (!inCodeBlock) {
        if (!BufferAppend(html, "<pre><code>")) {
          return false;
        }
        inCodeBlock = true;
        codeBlockHasLine = false;
      } else {
        if (!BufferAppend(html, "</code></pre>\n")) {
          return false;
        }
        inCodeBlock = false;
      }
    } else if (inCodeBlock) {
      if (codeBlockHasLine) {
        if (!BufferAppend(html, "\n")) {
          return false;
        }
      }
      if (!BufferAppendEscaped(html, lineRaw, lineLen)) {
        return false;
      }
      codeBlockHasLine = true;
    } else if (lineLen == 0U) {
      if (!CloseListBlocks(html, &inList, &inTopListItem, &inSublist)) {
        return false;
      }
      if (inBlockquote) {
        if (!BufferAppend(html, "</blockquote>\n")) {
          return false;
        }
        inBlockquote = false;
      }
    } else if (lineLen > 2U && (line[0] == '-' || line[0] == '*') &&
               line[1] == ' ') {
      if (inBlockquote) {
        if (!BufferAppend(html, "</blockquote>\n")) {
          return false;
        }
        inBlockquote = false;
      }
      if (!inList) {
        if (!BufferAppend(html, "<ul>\n")) {
          return false;
        }
        inList = true;
      }
      if (inSublist) {
        if (!BufferAppend(html, "</ul>\n")) {
          return false;
        }
        inSublist = false;
      }
      if (inTopListItem) {
        if (!BufferAppend(html, "</li>\n")) {
          return false;
        }
      }
      if (!BufferAppend(html, "<li>")) {
        return false;
      }
      if (!WriteInlineMarkdown(html, line + 2, lineLen - 2U, footnoteIds,
                               footnoteText, footnoteCount, footnoteOrder,
                               &nextFootnoteNumber)) {
        return false;
      }
      inTopListItem = true;
    } else if (lineLen > 3U && line[0] == '-' && line[1] == '-' &&
               line[2] == ' ') {
      if (!inList || !inTopListItem) {
        if (!BufferAppend(html, "<p>")) {
          return false;
        }
        if (!WriteInlineMarkdown(html, lineRaw, lineLen, footnoteIds,
                                 footnoteText, footnoteCount, footnoteOrder,
                                 &nextFootnoteNumber)) {
          return false;
        }
        if (!BufferAppend(html, "</p>\n")) {
          return false;
        }
      } else {
        if (!inSublist) {
          if (!BufferAppend(html, "<ul>\n")) {
            return false;
          }
          inSublist = true;
        }
        if (!BufferAppend(html, "<li>")) {
          return false;
        }
        if (!WriteInlineMarkdown(html, line + 3, lineLen - 3U, footnoteIds,
                                 footnoteText, footnoteCount, footnoteOrder,
                                 &nextFootnoteNumber)) {
          return false;
        }
        if (!BufferAppend(html, "</li>\n")) {
          return false;
        }
      }
    } else if (lineLen > 2U && line[0] == '>' && line[1] == ' ') {
      if (!CloseListBlocks(html, &inList, &inTopListItem, &inSublist)) {
        return false;
      }
      if (!inBlockquote) {
        if (!BufferAppend(html, "<blockquote>\n")) {
          return false;
        }
        inBlockquote = true;
      }
      if (!BufferAppend(html, "<p>")) {
        return false;
      }
      if (!WriteInlineMarkdown(html, line + 2, lineLen - 2U, footnoteIds,
                               footnoteText, footnoteCount, footnoteOrder,
                               &nextFootnoteNumber)) {
        return false;
      }
      if (!BufferAppend(html, "</p>\n")) {
        return false;
      }
    } else {
      if (!CloseListBlocks(html, &inList, &inTopListItem, &inSublist)) {
        return false;
      }
      if (inBlockquote) {
        if (!BufferAppend(html, "</blockquote>\n")) {
          return false;
        }
        inBlockquote = false;
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
        if (!BufferAppendFormat(html, "</h%zu>\n", hashes)) {
          return false;
        }
      } else {
        if (!BufferAppend(html, "<p>")) {
          return false;
        }
        if (!WriteInlineMarkdown(html, lineRaw, lineLen, footnoteIds,
                                 footnoteText, footnoteCount, footnoteOrder,
                                 &nextFootnoteNumber)) {
          return false;
        }
        if (!BufferAppend(html, "</p>\n")) {
          return false;
        }
      }
    }

    if (lineEnd == len) {
      break;
    }
    lineStart = lineEnd + 1U;
  }

  if (!CloseListBlocks(html, &inList, &inTopListItem, &inSublist)) {
    return false;
  }
  if (inCodeBlock) {
    if (!BufferAppend(html, "</code></pre>\n")) {
      return false;
    }
  }
  if (inBlockquote) {
    if (!BufferAppend(html, "</blockquote>\n")) {
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
            "<section class=\"panel footnotes\">\n<h2>Footnotes</h2>\n<ol>\n")) {
      return false;
    }
    for (size_t i = 0; i < footnoteCount; i++) {
      if (footnoteOrder[i] == 0) {
        continue;
      }
      if (!BufferAppendFormat(html, "<li id=\"fn-%s\">", footnoteIds[i])) {
        return false;
      }
      if (!BufferAppendEscaped(html, footnoteText[i],
                               std::strlen(footnoteText[i]))) {
        return false;
      }
      if (!BufferAppend(html, "</li>\n")) {
        return false;
      }
    }
    if (!BufferAppend(html, "</ol>\n</section>\n")) {
      return false;
    }
  }

  return true;
}
