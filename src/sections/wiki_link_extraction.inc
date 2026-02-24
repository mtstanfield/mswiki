/*
 * Wiki-link extraction logic for [[target]] references in page markdown.
 * This section depends on MAX_SLUG and shared helpers declared in main.cpp
 * (Slugify and CopyString).
 */

/*
 * Extract and deduplicate wiki-link targets from markdown text.
 */
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
