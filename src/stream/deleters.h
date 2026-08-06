#pragma once

#include <gst/app/gstappsink.h>
#include <gst/gstbuffer.h>
#include <gst/gstelement.h>

namespace subtitler {
inline void gst_unref(GstBuffer* buffer) noexcept { gst_buffer_unref(buffer); }

inline void gst_unref(GstElement* element) noexcept {
  gst_object_unref(element);
}

inline void gst_unref(GstBus* bus) noexcept { gst_object_unref(bus); }

inline void gst_unref(GstCaps* caps) noexcept { gst_caps_unref(caps); }

inline void gst_unref(GstAppSink* object) noexcept { gst_object_unref(object); }

inline void gst_unref(GstAppSrc* object) noexcept { gst_object_unref(object); }

inline void gst_unref(GstClock* clock) noexcept { gst_object_unref(clock); }

inline void gst_unref(GstMessage* message) noexcept {
  gst_message_unref(message);
}

inline void gst_unref(GstSample* sample) noexcept { gst_sample_unref(sample); }

inline void gst_unref(GError* error) noexcept { g_error_free(error); }

inline void gst_unref(gchar* chars) noexcept { g_free(chars); }

template <typename T>
concept GstUnreffable = requires(T* object) {
  { gst_unref(object) } noexcept;
};

template <GstUnreffable T>
struct GstDeleter {
  void operator()(T* object) const noexcept {
    if (object) {
      gst_unref(object);
    }
  }
};

// Non-owning view of a Gst object; makes the ownership contract visible at
// the call site.
template <typename T>
using GstView = T*;

struct GstBufferDeleter {
  void operator()(GstBuffer* buffer) const noexcept {
    if (buffer != nullptr) {
      gst_buffer_unref(buffer);
    }
  }
};
}  // namespace subtitler
