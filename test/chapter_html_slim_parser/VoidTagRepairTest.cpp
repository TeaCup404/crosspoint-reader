// Unclosed HTML void elements, e.g. `<meta charset="utf-8">` in every XHTML file of "Red Rising"
// (Pierce Brown), used to make expat fail with "mismatched tag" -> "Failed to index - invalid book".
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Epub/parsers/ChapterHtmlSlimParser.h"
#include "Epub/parsers/VoidTagRepair.h"

namespace {

std::string repairInChunks(const std::string& input, size_t chunk) {
  VoidTagRepair repair;
  std::string out;
  size_t pos = 0;
  do {
    const size_t len = std::min(chunk, input.size() - pos);
    const bool final = pos + len >= input.size();
    // Same in-place layout as ChapterHtmlSlimParser::parseStep(): input at +slack, output from 0.
    const size_t slack = VoidTagRepair::maxGrowth(len);
    std::vector<char> buf(slack + len + 1);
    std::memcpy(buf.data() + slack, input.data() + pos, len);
    const size_t n = repair.process(buf.data() + slack, len, buf.data(), final);
    out.append(buf.data(), n);
    pos += len;
  } while (pos < input.size());
  return out;
}

using Status = ChapterHtmlSlimParser::ParseStatus;

Status parseFile(const std::string& path, size_t* pages) {
  GfxRenderer renderer;
  CssParser css{"/tmp"};
  const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)> onPage =
      [pages](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t) { ++*pages; };
  ChapterHtmlSlimParser p{nullptr,
                          path,
                          renderer,
                          0,
                          1.0f,
                          false,
                          0,
                          static_cast<uint16_t>(renderer.getScreenWidth()),
                          static_cast<uint16_t>(renderer.getScreenHeight()),
                          false,
                          false,
                          onPage,
                          true,
                          "",
                          "",
                          0,
                          {},
                          nullptr,
                          &css};
  if (!p.beginParse()) return Status::Error;
  Status st = Status::More;
  while (st == Status::More) st = p.parseStep();
  if (st == Status::Done && !p.finishParse()) return Status::Error;
  if (st == Status::Error) p.abortParse();
  return st;
}

}  // namespace

TEST(VoidTagRepair, SelfClosesVoidTagsAndKeepsValidXhtmlUnchanged) {
  const std::string in =
      "<head><meta charset=\"utf-8\"><link href=\"a/b.css\" rel=\"stylesheet\" /></head>"
      "<p>a<br>b<BR class='x>y'>c<br/>d<br></br>e<img src=\"i.jpg\" alt=\"\"></img>"
      "<image href=\"x\"></image><!-- <hr> --><brx>z</brx><hr\n></p>";
  const std::string expected =
      "<head><meta charset=\"utf-8\"/><link href=\"a/b.css\" rel=\"stylesheet\" /></head>"
      "<p>a<br/>b<BR class='x>y'/>c<br/>d<br/>e<img src=\"i.jpg\" alt=\"\"/>"
      "<image href=\"x\"></image><!-- <hr/> --><brx>z</brx><hr\n/></p>";
  for (size_t chunk = 1; chunk <= in.size(); ++chunk) {
    ASSERT_EQ(repairInChunks(in, chunk), expected) << "chunk=" << chunk;
  }
  const std::string valid =
      "<?xml version=\"1.0\"?><!DOCTYPE html><html><body><p>x<br/>y<img src=\"a\" /></p></body></html>";
  for (size_t chunk = 1; chunk <= valid.size(); ++chunk) {
    ASSERT_EQ(repairInChunks(valid, chunk), valid) << "chunk=" << chunk;
  }
}

TEST(VoidTagRepair, ParserIndexesChapterWithUnclosedMetaTag) {
  const std::string path = "/tmp/crosspoint_void_tag_test.xhtml";
  std::string html =
      "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"no\"?>\n"
      "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\"\n  \"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">\n"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n<head>\n    <meta charset=\"utf-8\">\n"
      "  <title>Red Rising</title>\n</head>\n<body>\n";
  for (int i = 0; i < 300; ++i) {
    html += "<p>The first thing you should know about me is I am my father&#8217;s son.<br>x</p>\n";
  }
  html += "</body>\n</html>\n";
  std::FILE* f = std::fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  std::fwrite(html.data(), 1, html.size(), f);
  std::fclose(f);

  size_t pages = 0;
  EXPECT_EQ(parseFile(path, &pages), Status::Done);
  EXPECT_GT(pages, 0u);
  std::remove(path.c_str());
}

// Optional: CROSSPOINT_TEST_XHTML_DIR=<unzipped OEBPS/Text> parses every real chapter file.
TEST(VoidTagRepair, ParsesRealChaptersFromEnvDir) {
  const char* dir = std::getenv("CROSSPOINT_TEST_XHTML_DIR");
  if (!dir) GTEST_SKIP() << "CROSSPOINT_TEST_XHTML_DIR not set";
  size_t files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    size_t pages = 0;
    EXPECT_EQ(parseFile(entry.path().string(), &pages), Status::Done) << entry.path();
    ++files;
  }
  EXPECT_GT(files, 0u);
}
