#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#define main mswiki_main
#include "main.cpp"
#undef main

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0U || size > MAX_REQUEST_BYTES) {
    return 0;
  }

  std::vector<unsigned char> input(size + 1U);
  (void)std::memcpy(input.data(), data, size);
  input[size] = '\0';

  HttpRequest request;
  const unsigned char* bodyStart = nullptr;
  size_t bodyOffset = 0U;
  (void)ParseHttpRequest(input.data(), size, &request, &bodyStart, &bodyOffset);

  return 0;
}
