#include <stdio.h>
#define log_error(silent, fmt, x...) do { if(!(*silent)) printf("[%s:%s] " fmt, __FILE__, __FUNCTION__, ## x); } while (0)
#define log_printf(maxlevel, level, fmt, x...) do { if (*maxlevel >= level) { fprintf(stderr, "[%s:%s] " fmt, __FILE__, __FUNCTION__, ## x); fflush(stderr); } } while (0)
int set_log_level(const char *level_str);

/* Gates the raw, always-on fprintf(stderr,...) diagnostic traces that predate/bypass
 * the per-module *_printf mechanism below (which is already silent by default).
 * Off by default; enabled via the "-g 1" CLI flag, set from serviceapp's "Debug
 * logging" Setup toggle. Never gates the real IPC protocol (E2iSendMsg() in
 * main/exteplayer.c) - only human-readable debug traces. */
extern int g_verbose_logging;
#define RAW_DEBUG_LOG(...) do { if (g_verbose_logging) { fprintf(stderr, __VA_ARGS__); fflush(stderr); } } while (0)

extern int AV_DEBUG_LEVEL;

/*******************************************
 * ffmpeg
 *******************************************/
extern int FFMPEG_DEBUG_LEVEL;
extern int FFMPEG_SILENT;

#define ffmpeg_printf(...) log_printf(&FFMPEG_DEBUG_LEVEL, __VA_ARGS__)
#define ffmpeg_err(...) log_error(&FFMPEG_SILENT, __VA_ARGS__)

/*******************************************
 * container
 *******************************************/
extern int CONTAINER_DEBUG_LEVEL;
extern int CONTAINER_SILENT;

#define container_printf(...) log_printf(&CONTAINER_DEBUG_LEVEL, __VA_ARGS__)
#define container_err(...) log_error(&CONTAINER_SILENT, __VA_ARGS__)

/*******************************************
 * latmenc
 *******************************************/
extern int LATMENC_DEBUG_LEVEL;
extern int LATMENC_SILENT;

#define latmenc_printf(...) log_printf(&LATMENC_DEBUG_LEVEL, __VA_ARGS__)
#define latmenc_err(...) log_error(&LATMENC_SILENT, __VA_ARGS__)

/*******************************************
 * audio_mgr
 *******************************************/
extern int AUDIO_MGR_DEBUG_LEVEL;
extern int AUDIO_MGR_SILENT;

#define audio_mgr_printf(...) log_printf(&AUDIO_MGR_DEBUG_LEVEL, __VA_ARGS__)
#define audio_mgr_err(...) log_error(&AUDIO_MGR_SILENT, __VA_ARGS__)

/*******************************************
 * subtitle_mgr
 *******************************************/
extern int SUBTITLE_MGR_DEBUG_LEVEL;
extern int SUBTITLE_MGR_SILENT;

#define subtitle_mgr_printf(...) log_printf(&SUBTITLE_MGR_DEBUG_LEVEL, __VA_ARGS__)
#define subtitle_mgr_err(...) log_error(&SUBTITLE_MGR_SILENT, __VA_ARGS__)

/*******************************************
 * video_mgr
 *******************************************/
extern int VIDEO_MGR_DEBUG_LEVEL;
extern int VIDEO_MGR_SILENT;

#define video_mgr_printf(...) log_printf(&VIDEO_MGR_DEBUG_LEVEL, __VA_ARGS__)
#define video_mgr_err(...) log_error(&VIDEO_MGR_SILENT, __VA_ARGS__)

/*******************************************
 * linuxdvb
 *******************************************/
extern int LINUXDVB_DEBUG_LEVEL;
extern int LINUXDVB_SILENT;

#define linuxdvb_printf(...) log_printf(&LINUXDVB_DEBUG_LEVEL, __VA_ARGS__)
#define linuxdvb_err(...) log_error(&LINUXDVB_SILENT, __VA_ARGS__)

/*******************************************
 * buff
 *******************************************/
extern int BUFF_DEBUG_LEVEL;
extern int BUFF_SILENT;

#define buff_printf(...) log_printf(&BUFF_DEBUG_LEVEL, __VA_ARGS__)
#define buff_err(...) log_error(&BUFF_SILENT, __VA_ARGS__)

/*******************************************
 * output
 *******************************************/
extern int OUTPUT_DEBUG_LEVEL;
extern int OUTPUT_SILENT;

#define output_printf(...) log_printf(&OUTPUT_DEBUG_LEVEL, __VA_ARGS__)
#define output_err(...) log_error(&OUTPUT_SILENT, __VA_ARGS__)

/*******************************************
 * subtitle
 *******************************************/
extern int SUBTITLE_DEBUG_LEVEL;
extern int SUBTITLE_SILENT;

#define subtitle_printf(...) log_printf(&SUBTITLE_DEBUG_LEVEL, __VA_ARGS__)
#define subtitle_err(...) log_error(&SUBTITLE_SILENT, __VA_ARGS__)

/*******************************************
 * writer
 *******************************************/
extern int WRITER_DEBUG_LEVEL;
extern int WRITER_SILENT;

#define writer_printf(...) log_printf(&WRITER_DEBUG_LEVEL, __VA_ARGS__)
#define writer_err(...) log_error(&WRITER_SILENT, __VA_ARGS__)

/*******************************************
 * playback
 *******************************************/
extern int PLAYBACK_DEBUG_LEVEL;
extern int PLAYBACK_SILENT;

#define playback_printf(...) log_printf(&PLAYBACK_DEBUG_LEVEL, __VA_ARGS__)
#define playback_err(...) log_error(&PLAYBACK_SILENT, __VA_ARGS__)

/*******************************************
 * aac
 *******************************************/
extern int AAC_DEBUG_LEVEL;
extern int AAC_SILENT;

#define aac_printf(...) log_printf(&AAC_DEBUG_LEVEL, __VA_ARGS__)
#define aac_err(...) log_error(&AAC_SILENT, __VA_ARGS__)

/*******************************************
 * ac3
 *******************************************/
extern int AC3_DEBUG_LEVEL;
extern int AC3_SILENT;

#define ac3_printf(...) log_printf(&AC3_DEBUG_LEVEL, __VA_ARGS__)
#define ac3_err(...) log_error(&AC3_SILENT, __VA_ARGS__)

/*******************************************
 * amr
 *******************************************/
extern int AMR_DEBUG_LEVEL;
extern int AMR_SILENT;

#define amr_printf(...) log_printf(&AMR_DEBUG_LEVEL, __VA_ARGS__)
#define amr_err(...) log_error(&AMR_SILENT, __VA_ARGS__)

/*******************************************
 * divx
 *******************************************/
extern int DIVX_DEBUG_LEVEL;
extern int DIVX_SILENT;

#define divx_printf(...) log_printf(&DIVX_DEBUG_LEVEL, __VA_ARGS__)
#define divx_err(...) log_error(&DIVX_SILENT, __VA_ARGS__)

/*******************************************
 * dts
 *******************************************/
extern int DTS_DEBUG_LEVEL;
extern int DTS_SILENT;

#define dts_printf(...) log_printf(&DTS_DEBUG_LEVEL, __VA_ARGS__)
#define dts_err(...) log_error(&DTS_SILENT, __VA_ARGS__)

/*******************************************
 * h263
 *******************************************/
extern int H263_DEBUG_LEVEL;
extern int H263_SILENT;

#define h263_printf(...) log_printf(&H263_DEBUG_LEVEL, __VA_ARGS__)
#define h263_err(...) log_error(&H263_SILENT, __VA_ARGS__)

/*******************************************
 * h264
 *******************************************/
extern int H264_DEBUG_LEVEL;
extern int H264_SILENT;

#define h264_printf(...) log_printf(&H264_DEBUG_LEVEL, __VA_ARGS__)
#define h264_err(...) log_error(&H264_SILENT, __VA_ARGS__)

/*******************************************
 * h265
 *******************************************/
extern int H265_DEBUG_LEVEL;
extern int H265_SILENT;

#define h265_printf(...) log_printf(&H265_DEBUG_LEVEL, __VA_ARGS__)
#define h265_err(...) log_error(&H265_SILENT, __VA_ARGS__)

/*******************************************
 * lpcm
 *******************************************/
extern int LPCM_DEBUG_LEVEL;
extern int LPCM_SILENT;

#define lpcm_printf(...) log_printf(&LPCM_DEBUG_LEVEL, __VA_ARGS__)
#define lpcm_err(...) log_error(&LPCM_SILENT, __VA_ARGS__)

/*******************************************
 * mp3
 *******************************************/
extern int MP3_DEBUG_LEVEL;
extern int MP3_SILENT;

#define mp3_printf(...) log_printf(&MP3_DEBUG_LEVEL, __VA_ARGS__)
#define mp3_err(...) log_error(&MP3_SILENT, __VA_ARGS__)

/*******************************************
 * mpeg2
 *******************************************/
extern int MPEG2_DEBUG_LEVEL;
extern int MPEG2_SILENT;

#define mpeg2_printf(...) log_printf(&MPEG2_DEBUG_LEVEL, __VA_ARGS__)
#define mpeg2_err(...) log_error(&MPEG2_SILENT, __VA_ARGS__)

/*******************************************
 * mpeg4
 *******************************************/
extern int MPEG4_DEBUG_LEVEL;
extern int MPEG4_SILENT;

#define mpeg4_printf(...) log_printf(&MPEG4_DEBUG_LEVEL, __VA_ARGS__)
#define mpeg4_err(...) log_error(&MPEG4_SILENT, __VA_ARGS__)

/*******************************************
 * pcm
 *******************************************/
extern int PCM_DEBUG_LEVEL;
extern int PCM_SILENT;

#define pcm_printf(...) log_printf(&PCM_DEBUG_LEVEL, __VA_ARGS__)
#define pcm_err(...) log_error(&PCM_SILENT, __VA_ARGS__)

/*******************************************
 * vc1
 *******************************************/
extern int VC1_DEBUG_LEVEL;
extern int VC1_SILENT;

#define vc1_printf(...) log_printf(&VC1_DEBUG_LEVEL, __VA_ARGS__)
#define vc1_err(...) log_error(&VC1_SILENT, __VA_ARGS__)

/*******************************************
 * vp
 *******************************************/
extern int VP_DEBUG_LEVEL;
extern int VP_SILENT;

#define vp_printf(...) log_printf(&VP_DEBUG_LEVEL, __VA_ARGS__)
#define vp_err(...) log_error(&VP_SILENT, __VA_ARGS__)

/*******************************************
 * wma
 *******************************************/
extern int WMA_DEBUG_LEVEL;
extern int WMA_SILENT;

#define wma_printf(...) log_printf(&WMA_DEBUG_LEVEL, __VA_ARGS__)
#define wma_err(...) log_error(&WMA_SILENT, __VA_ARGS__)

/*******************************************
 * wmv
 *******************************************/
extern int WMV_DEBUG_LEVEL;
extern int WMV_SILENT;

#define wmv_printf(...) log_printf(&WMV_DEBUG_LEVEL, __VA_ARGS__)
#define wmv_err(...) log_error(&WMV_SILENT, __VA_ARGS__)

/*******************************************
 * mjpeg
 *******************************************/
extern int MJPEG_DEBUG_LEVEL;
extern int MJPEG_SILENT;

#define mjpeg_printf(...) log_printf(&MJPEG_DEBUG_LEVEL, __VA_ARGS__)
#define mjpeg_err(...) log_error(&MJPEG_SILENT, __VA_ARGS__)

/*******************************************
 * bcma
 *******************************************/
extern int BCMA_DEBUG_LEVEL;
extern int BCMA_SILENT;

#define bcma_printf(...) log_printf(&BCMA_DEBUG_LEVEL, __VA_ARGS__)
#define bcma_err(...) log_error(&BCMA_SILENT, __VA_ARGS__)

/*******************************************
 * plugin
 *******************************************/
extern int PLUGIN_DEBUG_LEVEL;
extern int PLUGIN_SILENT;

#define plugin_printf(...) log_printf(&PLUGIN_DEBUG_LEVEL, __VA_ARGS__)
#define plugin_err(...) log_error(&PLUGIN_SILENT, __VA_ARGS__)