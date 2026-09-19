#include <doctest/doctest.h>

#include "stream/whisper_demand.h"

TEST_CASE("temporary transcription never becomes continuous implicitly") {
  subtitler::WhisperDemand demand;
  demand.StartSession();
  REQUIRE(demand.Wanted());
  CHECK_FALSE(demand.Continuous());

  // A model-only request omits enabled. Completing or cancelling the
  // session afterwards must leave no demand, including at the next Poll.
  demand.SetContinuous(std::nullopt);
  CHECK_FALSE(demand.Continuous());
  demand.EndSession();
  CHECK_FALSE(demand.Wanted());
}

TEST_CASE("session completion preserves explicit transcription") {
  subtitler::WhisperDemand demand;
  SUBCASE("continuous mode was already enabled") {
    demand.SetContinuous(true);
    demand.StartSession();
    demand.EndSession();
  }
  SUBCASE("continuous mode enabled during the session") {
    demand.StartSession();
    demand.SetContinuous(true);
    demand.EndSession();
  }
  SUBCASE("explicit enable arrives before deferred cleanup") {
    demand.StartSession();
    demand.EndSession();
    demand.SetContinuous(true);
  }
  CHECK(demand.Wanted());
  CHECK(demand.Continuous());
}

TEST_CASE("a new sync session survives cleanup of its predecessor") {
  subtitler::WhisperDemand demand;
  demand.StartSession();
  demand.EndSession();
  demand.StartSession();
  CHECK(demand.Wanted());
  CHECK_FALSE(demand.Continuous());
  demand.EndSession();
  CHECK_FALSE(demand.Wanted());
}
