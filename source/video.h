#ifndef SOLOADER_VIDEO_H
#define SOLOADER_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

// Loads the AVPLAYER sysmodule. Call once, after gl_init() (video_play()'s
// texture allocator maps memory via sceGxmMapMemory, which needs the GXM
// context vitaGL's init brings up). Copied from the DH2 port (vitaGL,
// hardware-tested), which took it from Asphalt 5.
void video_init(void);

void video_shutdown(void);

// Plays name (a path under DATA_PATH, or DATA_PATH"files/") fullscreen,
// blocking the calling thread until the video ends naturally, the user
// skips it (Cross/Start), or it fails to open/init -- always returns,
// never hangs.
void video_play(const char *name);

#ifdef __cplusplus
}
#endif

#endif // SOLOADER_VIDEO_H
