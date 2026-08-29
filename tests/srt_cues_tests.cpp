#include <doctest/doctest.h>

#include <string>

#include "stream/srt_cues.h"

TEST_CASE("SRT cue parsing") {
  SUBCASE("parses a plain document") {
    const auto cues = subtitler::ParseSrtCues(
        "1\n"
        "00:00:01,000 --> 00:00:02,500\n"
        "Hello there.\n"
        "\n"
        "2\n"
        "00:00:03,000 --> 00:00:04,000\n"
        "General Kenobi.\n");

    REQUIRE(cues.size() == 2);
    CHECK(cues[0].start_ms == 1000);
    CHECK(cues[0].end_ms == 2500);
    CHECK(cues[0].text == "Hello there.");
    CHECK(cues[1].start_ms == 3000);
    CHECK(cues[1].end_ms == 4000);
    CHECK(cues[1].text == "General Kenobi.");
  }

  SUBCASE("handles CRLF") {
    const auto cues = subtitler::ParseSrtCues(
        "1\r\n"
        "00:00:01,000 --> 00:00:02,000\r\n"
        "First\r\n"
        "\r\n"
        "2\r\n"
        "00:00:03,000 --> 00:00:04,000\r\n"
        "Second\r\n");

    REQUIRE(cues.size() == 2);
    CHECK(cues[0].text == "First");
    CHECK(cues[1].text == "Second");
  }

  SUBCASE("joins multiline cues and strips markup") {
    const auto cues = subtitler::ParseSrtCues(
        "1\n"
        "00:00:01,000 --> 00:00:03,000\n"
        "<i>Hello</i>\n"
        "<font color=\"#ff0000\">there</font>, <b>General</b>\n");

    REQUIRE(cues.size() == 1);
    CHECK(cues[0].text == "Hello\nthere, General");
  }

  SUBCASE("tolerates dot decimals, big hours, and position attributes") {
    const auto cues = subtitler::ParseSrtCues(
        "1\n"
        "01:02:03.004 --> 01:02:05.006 X1:100 X2:200\n"
        "Late show.\n");

    REQUIRE(cues.size() == 1);
    CHECK(cues[0].start_ms == 3723004);
    CHECK(cues[1 - 1].end_ms == 3725006);
  }

  SUBCASE("keeps valid cues and skips malformed blocks") {
    const auto cues = subtitler::ParseSrtCues(
        "not a number\n"
        "\n"
        "00:00:01,000 --> nonsense\n"
        "Bad times.\n"
        "\n"
        "3\n"
        "00:00:02,000 --> 00:00:01,000\n"
        "Backwards.\n"
        "\n"
        "4\n"
        "00:00:05,000 --> 00:00:06,000\n"
        "\n"
        "5\n"
        "00:00:07,000 --> 00:00:08,000\n"
        "Keeper.\n");

    REQUIRE(cues.size() == 1);
    CHECK(cues[0].text == "Keeper.");
    CHECK(cues[0].start_ms == 7000);
  }

  SUBCASE("missing sequence numbers are fine") {
    const auto cues = subtitler::ParseSrtCues(
        "00:00:01,000 --> 00:00:02,000\n"
        "No number.\n");

    REQUIRE(cues.size() == 1);
    CHECK(cues[0].text == "No number.");
  }

  SUBCASE("empty and garbage input parses to nothing") {
    CHECK(subtitler::ParseSrtCues("").empty());
    CHECK(subtitler::ParseSrtCues("\n\n\n").empty());
    CHECK(subtitler::ParseSrtCues("this is not an srt file").empty());
  }
}
