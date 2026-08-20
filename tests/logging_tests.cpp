#include <doctest/doctest.h>

#include "utils/logging.h"

namespace {

using namespace subtitler;

TEST_CASE("log level parsing") {
  CHECK(ParseLogLevel("error") == LogLevel::kError);
  CHECK(ParseLogLevel("warn") == LogLevel::kWarning);
  CHECK(ParseLogLevel("info") == LogLevel::kInfo);
  CHECK(ParseLogLevel("debug") == LogLevel::kDebug);

  CHECK(!ParseLogLevel("warning"));
  CHECK(!ParseLogLevel("INFO"));
  CHECK(!ParseLogLevel("trace"));
  CHECK(!ParseLogLevel(""));
}

TEST_CASE("log level names") {
  for (const auto* name : {"error", "warn", "info", "debug"}) {
    CHECK(LogLevelName(*ParseLogLevel(name)) == name);
  }
}

TEST_CASE("log config parsing") {
  SUBCASE("empty spec") {
    CHECK(ParseLogConfig("").empty());
  }

  SUBCASE("single entry") {
    const auto config = ParseLogConfig("mjpeg:info");
    REQUIRE(config.size() == 1);
    CHECK(config.at("mjpeg") == LogLevel::kInfo);
  }

  SUBCASE("multiple entries") {
    const auto config = ParseLogConfig("mjpeg:info,stream:debug");
    REQUIRE(config.size() == 2);
    CHECK(config.at("mjpeg") == LogLevel::kInfo);
    CHECK(config.at("stream") == LogLevel::kDebug);
  }

  SUBCASE("malformed entries are ignored") {
    CHECK(ParseLogConfig("mjpeg").empty());
    CHECK(ParseLogConfig("mjpeg:").empty());
    CHECK(ParseLogConfig(":info").empty());
    CHECK(ParseLogConfig("mjpeg:info:extra").empty());
    CHECK(ParseLogConfig("mjpeg:verbose").empty());
    CHECK(ParseLogConfig(",").empty());
  }

  SUBCASE("valid entries survive malformed ones") {
    const auto config = ParseLogConfig("junk,mjpeg:warn,,stream:error");
    REQUIRE(config.size() == 2);
    CHECK(config.at("mjpeg") == LogLevel::kWarning);
    CHECK(config.at("stream") == LogLevel::kError);
  }

  SUBCASE("a repeated label keeps the last entry") {
    const auto config = ParseLogConfig("mjpeg:info,mjpeg:debug");
    REQUIRE(config.size() == 1);
    CHECK(config.at("mjpeg") == LogLevel::kDebug);
  }
}

TEST_CASE("log enablement") {
  const auto config = ParseLogConfig("mjpeg:info");

  SUBCASE("unconfigured label is disabled at every level") {
    for (const auto level :
         {LogLevel::kError, LogLevel::kWarning, LogLevel::kInfo,
          LogLevel::kDebug}) {
      CHECK(!LogEnabled(config, "stream", level));
    }
  }

  SUBCASE("configured label is enabled at its level and below") {
    CHECK(LogEnabled(config, "mjpeg", LogLevel::kError));
    CHECK(LogEnabled(config, "mjpeg", LogLevel::kWarning));
    CHECK(LogEnabled(config, "mjpeg", LogLevel::kInfo));
    CHECK(!LogEnabled(config, "mjpeg", LogLevel::kDebug));
  }
}

TEST_CASE("log catch-all label") {
  SUBCASE("all enables every unconfigured label") {
    const auto config = ParseLogConfig("all:info");
    CHECK(LogEnabled(config, "mjpeg", LogLevel::kInfo));
    CHECK(LogEnabled(config, "stream", LogLevel::kWarning));
    CHECK(!LogEnabled(config, "mjpeg", LogLevel::kDebug));
  }

  SUBCASE("a label's own entry wins over the catch-all") {
    const auto config = ParseLogConfig("all:info,stream:debug");
    CHECK(LogEnabled(config, "stream", LogLevel::kDebug));
    CHECK(!LogEnabled(config, "mjpeg", LogLevel::kDebug));
  }

  SUBCASE("a label's own entry can narrow the catch-all") {
    const auto config = ParseLogConfig("all:debug,stream:error");
    CHECK(LogEnabled(config, "stream", LogLevel::kError));
    CHECK(!LogEnabled(config, "stream", LogLevel::kWarning));
    CHECK(LogEnabled(config, "mjpeg", LogLevel::kDebug));
  }
}

}  // namespace
