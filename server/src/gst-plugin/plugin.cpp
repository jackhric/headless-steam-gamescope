// Both the video and audio elements register from this single plugin_init
// (one GST_PLUGIN_DEFINE per .so).

#include <gst/gst.h>
#include <moonlight/fec.hpp>

extern gboolean rtpmoonlight_register_video(GstPlugin *plugin);
extern gboolean rtpmoonlight_register_audio(GstPlugin *plugin);

static gboolean plugin_init(GstPlugin *plugin) {
  // Must run here: without it the audio element derefs a null reed_solomon_new_fn and crashes.
  moonlight::fec::init();

  if (!rtpmoonlight_register_video(plugin))
    return FALSE;
  if (!rtpmoonlight_register_audio(plugin))
    return FALSE;
  return TRUE;
}

#ifndef VERSION
#define VERSION "0.1.0"
#endif
#ifndef PACKAGE
#define PACKAGE "rtpmoonlightpay"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "steam-stream-rtpmoonlightpay"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/games-on-whales/wolf"
#endif

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR,
                  GST_VERSION_MINOR,
                  rtpmoonlightpay,
                  "Moonlight RTP payloaders (video + audio) lifted from Wolf",
                  plugin_init,
                  VERSION,
                  "LGPL",
                  PACKAGE_NAME,
                  GST_PACKAGE_ORIGIN)
