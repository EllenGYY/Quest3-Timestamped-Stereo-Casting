#ifndef VIDEO_PREPROCESS_H
#define VIDEO_PREPROCESS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>

// Function to apply video effects to a frame
void apply_video_effects(AVFrame *frame, const char *map_path);

#ifdef __cplusplus
}
#endif

#endif