/* DVB API compatibility: AUDIO_GET_PTS / VIDEO_GET_PTS were removed in newer kernel headers */
#ifndef DVB_COMPAT_H
#define DVB_COMPAT_H

#include <sys/ioctl.h>

#ifndef VIDEO_GET_PTS
#define VIDEO_GET_PTS  _IOR('o', 57, __u64)
#endif

#ifndef AUDIO_GET_PTS
#define AUDIO_GET_PTS  _IOR('o', 19, __u64)
#endif

#endif /* DVB_COMPAT_H */
