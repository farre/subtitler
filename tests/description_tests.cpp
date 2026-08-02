#include "stream/description.h"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

TEST_CASE("capture pipeline description") {
  CHECK(subtitler::capture_pipeline_description("/dev/video0") ==
        "v4l2src "
        "device=\"/dev/video0\" "
        "io-mode=mmap "
        "do-timestamp=true "
        "! video/x-raw,"
        "format=YUY2,"
        "width=1920,"
        "height=1080,"
        "framerate=60/1 "
        "! appsink "
        "name=capture_sink "
        "sync=false "
        "max-buffers=2 "
        "drop=true");
}

TEST_CASE("output pipeline description") {
  CHECK(subtitler::output_pipeline_description(std::nullopt) ==
        "appsrc "
        "name=output_source "
        "is-live=true "
        "format=time "
        "block=true "
        "max-buffers=2 "
        "! kmssink "
        "driver-name=vc4 "
        "force-modesetting=true "
        "sync=true");

  CHECK(subtitler::output_pipeline_description(7) ==
        "appsrc "
        "name=output_source "
        "is-live=true "
        "format=time "
        "block=true "
        "max-buffers=2 "
        "! kmssink "
        "driver-name=vc4 "
        "force-modesetting=true "
        "sync=true "
        "connector-id=7");
}
