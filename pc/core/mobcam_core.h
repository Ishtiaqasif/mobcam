/* MobCam core: phone connection, H.264 decoding, picture processing and the virtual camera.
 * Plain C API so the WPF app (P/Invoke) and the CLI share one engine. Thread-safe. */
#pragma once

#include <stdint.h>
#include <wchar.h>

#if defined(MOBCAM_CORE_STATIC)
#define MC_API
#elif defined(MOBCAM_CORE_EXPORTS)
#define MC_API __declspec(dllexport)
#else
#define MC_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum mc_link {
  MC_LINK_AUTO = 0, /* USB if a phone is plugged in, else Wi-Fi discovery */
  MC_LINK_USB = 1,
  MC_LINK_WIFI = 2, /* discovery, or the given host if not empty */
};

enum mc_state {
  MC_STATE_IDLE = 0,
  MC_STATE_SEARCHING = 1,
  MC_STATE_CONNECTING = 2,
  MC_STATE_STREAMING = 3,
};

enum mc_vcam {
  MC_VCAM_NOT_INSTALLED = 0,
  MC_VCAM_INSTALLED = 1, /* installed and current, not started */
  MC_VCAM_OUTDATED = 2,  /* installed DLL differs from the bundled one */
  MC_VCAM_RUNNING = 3,
  MC_VCAM_FAILED = 4,    /* start failed, see vcam_error */
};

typedef struct mc_status {
  int32_t state;               /* mc_state */
  int32_t link;                /* MC_LINK_USB / MC_LINK_WIFI while connected */
  int32_t width, height;       /* phone stream size */
  int32_t fps;
  int32_t kbps;
  int32_t rotation;            /* rotation applied to the picture */
  int32_t phone_rotation;      /* rotation reported by the phone, -1 unknown */
  int32_t phone_facing;        /* 0 back, 1 front, -1 unknown */
  int32_t phone_target_height; /* 480/720/1080, 0 unknown */
  int32_t camera_in_use;       /* an app is pulling frames from the virtual camera */
  int32_t vcam;                /* mc_vcam */
  int32_t vcam_error;          /* HRESULT of the last failed start */
  int32_t phone_lens;          /* index into mc_get_lenses(), -1 unknown */
  int32_t lens_count;          /* number of lenses the phone reported */
  wchar_t peer[128];           /* phone name / address */
  wchar_t message[256];        /* human readable status or last error */
} mc_status;

typedef struct mc_lens {
  int32_t front; /* 1 = front camera */
  float zoom;    /* 0.67, 1, 3.4 ... */
  wchar_t label[32];
} mc_lens;

typedef struct mc_device {
  int32_t link; /* MC_LINK_USB or MC_LINK_WIFI */
  int32_t port;
  wchar_t name[128];
  wchar_t host[64];
} mc_device;

MC_API int32_t mc_init(void);
MC_API void mc_shutdown(void);

/* Virtual camera */
MC_API int32_t mc_vcam_state(void);
MC_API int32_t mc_vcam_install(void); /* shows a UAC prompt, blocks; 0 on success */
MC_API int32_t mc_vcam_start(void);   /* HRESULT */
MC_API void mc_vcam_stop(void);

/* Connection (keeps reconnecting until mc_disconnect) */
MC_API void mc_connect(int32_t link, const wchar_t* host, int32_t port);
MC_API void mc_disconnect(void);
MC_API void mc_get_status(mc_status* status);
/* Lists phones reachable over USB (adb) and Wi-Fi (broadcast). Blocks up to timeout_ms. */
MC_API int32_t mc_discover(mc_device* devices, int32_t max_devices, int32_t timeout_ms);

/* Picture */
MC_API void mc_set_rotation(int32_t degrees); /* -1 = follow the phone */
MC_API void mc_set_fill(int32_t fill);        /* 0 fit (black bars), 1 fill (crop) */
/* Copies the latest output frame as BGRA scaled to width x height. Returns 1 if a new frame was
 * written (seq updated), 0 if nothing new since *seq. */
MC_API int32_t mc_get_preview(uint8_t* bgra, int32_t width, int32_t height, int32_t stride, int64_t* seq);

/* Lenses of the connected phone (ultra-wide, wide, telephoto, front...). Returns the count. */
MC_API int32_t mc_get_lenses(mc_lens* lenses, int32_t max_lenses);

/* Phone remote control: "SWITCH", "FACING front|back", "LENS <index>", "RES 480|720|1080".
 * Returns 1 if sent. */
MC_API int32_t mc_phone_command(const char* command);

#ifdef __cplusplus
}
#endif
