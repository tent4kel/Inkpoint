# TODO / Known Improvements

## Instapaper: stream HTML download directly to SD card

**Current state:** `InstapaperClient::getArticleText()` buffers the full article HTML in a
`std::string` (capped at 32 KB). A probe+reserve trick pre-allocates the string buffer before
the TLS context is created, avoiding `__throw_bad_alloc()` crashes from heap fragmentation on
no-exceptions builds. This works in practice but is fragile under severe fragmentation (e.g.
after multiple failed SSL attempts or concurrent background syncs).

**Ideal fix:** Stream the HTTP response directly to a temp file on SD, then convert.

### Step 1 — `HttpDownloader::postUrlToFile()`

Add a variant of `postUrl()` that writes the response body directly to an `FsFile` instead of
a `std::string`. Same OAuth/auth-header support, same chunk loop, but each chunk is
`file.write(chunkBuf, bytesRead)` instead of `outContent.append(...)`. Peak RAM during download
drops to ~1 KB (chunk buffer) + TLS context (~34 KB). No large contiguous malloc needed at all.

### Step 2 — `HtmlToMarkdown` streaming interface

Add a chunk-based API alongside the existing `convert(const std::string&)`:

```cpp
class HtmlToMarkdown {
public:
  static std::string convert(const std::string& html);  // existing

  // New streaming API — feed input in arbitrary chunks, collect output via callback
  void begin();
  void feed(const char* chunk, size_t len);
  std::string end();   // or: void end(FsFile& outFile) to write markdown directly
};
```

The state machine is already character-by-character and stateful, so splitting across chunk
boundaries is straightforward. The output can be written directly to the `.md` file instead of
building a return string.

### Step 3 — wire it up in `downloadSingleArticle()`

```
POST → postUrlToFile() → /instapaper/tmp.html   (no heap string at all)
     → HtmlToMarkdown::feed() in 4 KB chunks    (4 KB read buffer)
     → write markdown directly to .de.md        (4 KB write buffer)
     → Storage.remove(tmp.html)
```

Peak heap during the entire download+convert pipeline: ~TLS (34 KB) + 2× 4 KB buffers.
No large contiguous malloc anywhere. Equivalent to how OTA updates stream firmware directly
to flash via `esp_https_ota_perform()` without ever buffering the binary in RAM.

### Why not done yet

The probe+reserve approach is currently stable. This refactor touches `HttpDownloader`,
`HtmlToMarkdown`, `InstapaperClient`, and `InstapaperActivity` — worth doing if crashes
return or article size cap needs to be raised.
