// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "format.hpp"

#include "gtest.hpp"

using namespace automat;

TEST(BlobSummary, Empty) { EXPECT_EQ(BlobSummary(""), "(0 B)"); }

TEST(BlobSummary, Ascii) { EXPECT_EQ(BlobSummary("Hello, world"), "(utf-8 12 B) Hello, world"); }

TEST(BlobSummary, Utf8) { EXPECT_EQ(BlobSummary("Hello 😀"), "(utf-8 10 B) Hello 😀"); }

TEST(BlobSummary, Utf8SkipsByteOrderMark) {
  EXPECT_EQ(BlobSummary("\xEF\xBB\xBF"
                        "hi"),
            "(utf-8 5 B) \xEF\xBB\xBFhi");
  EXPECT_EQ(BlobSummary("\xEF\xBB\xBF"), "(utf-8 3 B) \xEF\xBB\xBF");
}

TEST(BlobSummary, EscapesLineBreaks) {
  EXPECT_EQ(BlobSummary("file:///a\r\nb\tc"), "(utf-8 14 B) file:///a\\r\\nb\\tc");
}

TEST(BlobSummary, RejectsControlCharacters) {
  EXPECT_EQ(BlobSummary("\x01\x02"), "(blob 2 B) ..  0102");
  EXPECT_EQ(BlobSummary("a\x7F"), "(blob 2 B) a.  617f");
  EXPECT_EQ(BlobSummary("\xC2\x85"), "(blob 2 B) ..  c285");
}

TEST(BlobSummary, RejectsBrokenUtf8) {
  EXPECT_EQ(BlobSummary("\xC3\x28"), "(blob 2 B) .(  c328");
  EXPECT_EQ(BlobSummary("\xC0\x80"), "(blob 2 B) ..  c080");
  EXPECT_EQ(BlobSummary("\xE0\x80\x80"), "(blob 3 B) ...  e08080");
  EXPECT_EQ(BlobSummary("\xED\xA0\x80"), "(blob 3 B) ...  eda080");
  EXPECT_EQ(BlobSummary("\xF4\x90\x80\x80"), "(blob 4 B) ....  f4908080");
  EXPECT_EQ(BlobSummary("ab\xE2\x82"), "(blob 4 B) ab..  6162e282");
}

TEST(BlobSummary, Utf16) {
  alignas(2) const char s[] = "H\0i\0";
  EXPECT_EQ(BlobSummary(StrView(s, 4)), "(utf-16 4 B) Hi");
  alignas(2) const char s2[] =
      "\xFF\xFE"
      "a\0"
      "\x42\x01";
  EXPECT_EQ(BlobSummary(StrView(s2, 6)),
            "(utf-16 6 B) \xEF\xBB\xBF"
            "ał");
  EXPECT_EQ(BlobSummary("a\0"
                        "\x3D\xD8\x00\xDE"sv),
            "(utf-16 6 B) a😀");
  EXPECT_EQ(BlobSummary("\xFF\xFE"sv), "(blob 2 B) ..  fffe");
}

TEST(BlobSummary, Utf16NeedsAsciiMajority) {
  EXPECT_EQ(BlobSummary("\x42\x01\x43\x01"sv), "(blob 4 B) B.C.  42014301");
}

TEST(BlobSummary, RejectsBrokenUtf16) {
  EXPECT_EQ(BlobSummary("\x00\xDC\x3D\xD8"sv), "(blob 4 B) ..=.  00dc3dd8");
  EXPECT_EQ(BlobSummary("a\0\x3D\xD8"sv), "(blob 4 B) a.=.  61003dd8");
  EXPECT_EQ(BlobSummary("a\0b"sv), "(blob 3 B) a.b  610062");
  EXPECT_EQ(BlobSummary("a\0\0\0b\0"sv), "(blob 6 B) a...b.  610000006200");
}

TEST(BlobSummary, Hex) {
  EXPECT_EQ(BlobSummary("\x00\x01"
                        "At\xFF"
                        "5n"
                        "\0"sv),
            "(blob 8 B) ..At.5n.  00014174ff356e00");
}

TEST(BlobSummary, TruncatesTextInTheMiddle) {
  Str text(100, 'a');
  text.front() = 'S';
  text.back() = 'E';
  EXPECT_EQ(BlobSummary(text, 26), "(utf-8 100 B) Saaaaa...aaE");
  EXPECT_EQ(BlobSummary("lodźðłózd", 21), "(utf-8 13 B) lodź...d");
  EXPECT_EQ(BlobSummary("a\nb\nc\nd\ne\nf", 21), "(utf-8 11 B) a\\nb...f");
}

TEST(BlobSummary, TruncatesHexInTheMiddle) {
  Str blob(100, '\x01');
  blob.front() = 'S';
  blob.back() = 'E';
  EXPECT_EQ(BlobSummary(blob, 33), "(blob 100 B) S..|.E  530101|0145");
}

TEST(BlobSummary, FitsExactly) {
  EXPECT_EQ(BlobSummary(Str(67, 'a')), "(utf-8 67 B) " + Str(67, 'a'));
  EXPECT_EQ(BlobSummary(Str(68, 'a')), "(utf-8 68 B) " + Str(34, 'a') + "..." + Str(30, 'a'));
}

TEST(BlobSummary, TinyBudgets) {
  EXPECT_EQ(BlobSummary(Str(100, 'a'), 5), "(utf-8 100 B)");
  EXPECT_EQ(BlobSummary(Str(100, '\xff'), 5), "(blob 100 B)");
}
