#include "debug.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <libavutil/log.h>

int g_verbose_logging = 0;
int AV_DEBUG_LEVEL = 0;
int FFMPEG_DEBUG_LEVEL = 0;
int FFMPEG_SILENT = 1;
int CONTAINER_DEBUG_LEVEL = 0;
int CONTAINER_SILENT = 1;
int LATMENC_DEBUG_LEVEL = 0;
int LATMENC_SILENT = 1;
int AUDIO_MGR_DEBUG_LEVEL = 0;
int AUDIO_MGR_SILENT = 1;
int SUBTITLE_MGR_DEBUG_LEVEL = 0;
int SUBTITLE_MGR_SILENT = 1;
int VIDEO_MGR_DEBUG_LEVEL = 0;
int VIDEO_MGR_SILENT = 1;
int LINUXDVB_DEBUG_LEVEL = 0;
int LINUXDVB_SILENT = 1;
int BUFF_DEBUG_LEVEL = 0;
int BUFF_SILENT = 1;
int OUTPUT_DEBUG_LEVEL = 0;
int OUTPUT_SILENT = 1;
int SUBTITLE_DEBUG_LEVEL = 0;
int SUBTITLE_SILENT = 1;
int WRITER_DEBUG_LEVEL = 0;
int WRITER_SILENT = 1;
int PLAYBACK_DEBUG_LEVEL = 0;
int PLAYBACK_SILENT = 1;
int AAC_DEBUG_LEVEL = 0;
int AAC_SILENT = 1;
int AC3_DEBUG_LEVEL = 0;
int AC3_SILENT = 1;
int AMR_DEBUG_LEVEL = 0;
int AMR_SILENT = 1;
int DIVX_DEBUG_LEVEL = 0;
int DIVX_SILENT = 1;
int DTS_DEBUG_LEVEL = 0;
int DTS_SILENT = 1;
int H263_DEBUG_LEVEL = 0;
int H263_SILENT = 1;
int H264_DEBUG_LEVEL = 0;
int H264_SILENT = 1;
int H265_DEBUG_LEVEL = 0;
int H265_SILENT = 1;
int LPCM_DEBUG_LEVEL = 0;
int LPCM_SILENT = 1;
int MP3_DEBUG_LEVEL = 0;
int MP3_SILENT = 1;
int MPEG2_DEBUG_LEVEL = 0;
int MPEG2_SILENT = 1;
int MPEG4_DEBUG_LEVEL = 0;
int MPEG4_SILENT = 1;
int PCM_DEBUG_LEVEL = 0;
int PCM_SILENT = 1;
int VC1_DEBUG_LEVEL = 0;
int VC1_SILENT = 1;
int VP_DEBUG_LEVEL = 0;
int VP_SILENT = 1;
int WMA_DEBUG_LEVEL = 0;
int WMA_SILENT = 1;
int WMV_DEBUG_LEVEL = 0;
int WMV_SILENT = 1;
int MJPEG_DEBUG_LEVEL = 0;
int MJPEG_SILENT = 1;
int BCMA_DEBUG_LEVEL = 0;
int BCMA_SILENT = 1;
int PLUGIN_DEBUG_LEVEL = 0;
int PLUGIN_SILENT = 1;

struct log_map
{
	const char *name;
	int *level;
} log_mapping[] =
{
	{ "AV_DEBUG_LEVEL", &AV_DEBUG_LEVEL },
	{ "FFMPEG_DEBUG_LEVEL", &FFMPEG_DEBUG_LEVEL },
	{ "FFMPEG_SILENT", &FFMPEG_SILENT },
	{ "CONTAINER_DEBUG_LEVEL", &CONTAINER_DEBUG_LEVEL },
	{ "CONTAINER_SILENT", &CONTAINER_SILENT },
	{ "LATMENC_DEBUG_LEVEL", &LATMENC_DEBUG_LEVEL },
	{ "LATMENC_SILENT", &LATMENC_SILENT },
	{ "AUDIO_MGR_DEBUG_LEVEL", &AUDIO_MGR_DEBUG_LEVEL },
	{ "AUDIO_MGR_SILENT", &AUDIO_MGR_SILENT },
	{ "SUBTITLE_MGR_DEBUG_LEVEL", &SUBTITLE_MGR_DEBUG_LEVEL },
	{ "SUBTITLE_MGR_SILENT", &SUBTITLE_MGR_SILENT },
	{ "VIDEO_MGR_DEBUG_LEVEL", &VIDEO_MGR_DEBUG_LEVEL },
	{ "VIDEO_MGR_SILENT", &VIDEO_MGR_SILENT },
	{ "LINUXDVB_DEBUG_LEVEL", &LINUXDVB_DEBUG_LEVEL },
	{ "LINUXDVB_SILENT", &LINUXDVB_SILENT },
	{ "BUFF_DEBUG_LEVEL", &BUFF_DEBUG_LEVEL },
	{ "BUFF_SILENT", &BUFF_SILENT },
	{ "OUTPUT_DEBUG_LEVEL", &OUTPUT_DEBUG_LEVEL },
	{ "OUTPUT_SILENT", &OUTPUT_SILENT },
	{ "SUBTITLE_DEBUG_LEVEL", &SUBTITLE_DEBUG_LEVEL },
	{ "SUBTITLE_SILENT", &SUBTITLE_SILENT },
	{ "WRITER_DEBUG_LEVEL", &WRITER_DEBUG_LEVEL },
	{ "WRITER_SILENT", &WRITER_SILENT },
	{ "PLAYBACK_DEBUG_LEVEL", &PLAYBACK_DEBUG_LEVEL },
	{ "PLAYBACK_SILENT", &PLAYBACK_SILENT },
	{ "AAC_DEBUG_LEVEL", &AAC_DEBUG_LEVEL },
	{ "AAC_SILENT", &AAC_SILENT },
	{ "AC3_DEBUG_LEVEL", &AC3_DEBUG_LEVEL },
	{ "AC3_SILENT", &AC3_SILENT },
	{ "AMR_DEBUG_LEVEL", &AMR_DEBUG_LEVEL },
	{ "AMR_SILENT", &AMR_SILENT },
	{ "DIVX_DEBUG_LEVEL", &DIVX_DEBUG_LEVEL },
	{ "DIVX_SILENT", &DIVX_SILENT },
	{ "DTS_DEBUG_LEVEL", &DTS_DEBUG_LEVEL },
	{ "DTS_SILENT", &DTS_SILENT },
	{ "H263_DEBUG_LEVEL", &H263_DEBUG_LEVEL },
	{ "H263_SILENT", &H263_SILENT },
	{ "H264_DEBUG_LEVEL", &H264_DEBUG_LEVEL },
	{ "H264_SILENT", &H264_SILENT },
	{ "H265_DEBUG_LEVEL", &H265_DEBUG_LEVEL },
	{ "H265_SILENT", &H265_SILENT },
	{ "LPCM_DEBUG_LEVEL", &LPCM_DEBUG_LEVEL },
	{ "LPCM_SILENT", &LPCM_SILENT },
	{ "MP3_DEBUG_LEVEL", &MP3_DEBUG_LEVEL },
	{ "MP3_SILENT", &MP3_SILENT },
	{ "MPEG2_DEBUG_LEVEL", &MPEG2_DEBUG_LEVEL },
	{ "MPEG2_SILENT", &MPEG2_SILENT },
	{ "MPEG4_DEBUG_LEVEL", &MPEG4_DEBUG_LEVEL },
	{ "MPEG4_SILENT", &MPEG4_SILENT },
	{ "PCM_DEBUG_LEVEL", &PCM_DEBUG_LEVEL },
	{ "PCM_SILENT", &PCM_SILENT },
	{ "VC1_DEBUG_LEVEL", &VC1_DEBUG_LEVEL },
	{ "VC1_SILENT", &VC1_SILENT },
	{ "VP_DEBUG_LEVEL", &VP_DEBUG_LEVEL },
	{ "VP_SILENT", &VP_SILENT },
	{ "WMA_DEBUG_LEVEL", &WMA_DEBUG_LEVEL },
	{ "WMA_SILENT", &WMA_SILENT },
	{ "WMV_DEBUG_LEVEL", &WMV_DEBUG_LEVEL },
	{ "WMV_SILENT", &WMV_SILENT },
	{ "MJPEG_DEBUG_LEVEL", &MJPEG_DEBUG_LEVEL },
	{ "MJPEG_SILENT", &MJPEG_SILENT },
	{ "BCMA_DEBUG_LEVEL", &BCMA_DEBUG_LEVEL },
	{ "BCMA_SILENT", &BCMA_SILENT },
	{ "PLUGIN_DEBUG_LEVEL", &PLUGIN_DEBUG_LEVEL },
	{ "PLUGIN_SILENT", &PLUGIN_SILENT },
	{ NULL, NULL },
};

int set_log_level(const char *level_str)
{
	const char *p;

	printf("Parsing debug level: %s\n", level_str);

	if( (p = strchr(level_str, ':')) == NULL)
	{
		printf("separator not found\n");
		return -1;
	}

	size_t level_name_len = p - level_str;
	p++;
	printf("converting level: \"%s\"\n", p);
	int level = atoi(p);
	struct log_map *m;

	printf("level prefix len: %ld, level: %d\n", (long) level_name_len, level);

	for( m = log_mapping; m->name; m++ )
	{
		printf("comparing %s with %.*s\n", m->name,  level_name_len, level_str );
		if (strlen(m->name) == level_name_len && strncmp(m->name, level_str, level_name_len) == 0)
		{
			printf("match\n");
			*m->level = level;
			break;
		}
	}

	// if (av_log_get_level() != AV_DEBUG_LEVEL)
	// {
	// 	printf("changing av debug level to %d\n", AV_DEBUG_LEVEL);
	// 	if (AV_DEBUG_LEVEL > 0)
	// 	{
	// 		av_log_set_callback( av_log_default_callback );
	// 	}
	// 	av_log_set_level(AV_DEBUG_LEVEL);
	// }

	return 0;
}
