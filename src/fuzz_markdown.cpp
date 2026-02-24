#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#define main mswiki_main
#include "main.cpp"
#undef main

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (data == nullptr) {
    return 0;
  }

  if (size > MAX_PAGE_MARKDOWN) {
    size = MAX_PAGE_MARKDOWN;
  }

  std::vector<char> markdown(size + 1U);
  if (size > 0U) {
    (void)std::memcpy(markdown.data(), data, size);
  }
  markdown[size] = '\0';

  std::vector<char> htmlStorage(MAX_RESPONSE_BYTES);
  TextBuffer html;
  html.data = htmlStorage.data();
  html.capacity = htmlStorage.size();
  BufferReset(&html);

  (void)RenderMarkdownToHtml(markdown.data(), &html);

  char links[32][MAX_SLUG];
  size_t linkCount = 0U;
  ExtractWikiLinks(markdown.data(), links, &linkCount, 32U);

  if (html.length > 0U) {
    volatile char sink = html.data[0];
    (void)sink;
  }

  return 0;
}
