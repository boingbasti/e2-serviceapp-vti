/*
 * Container handling for all stream's handled by ffmpeg
 * konfetti 2010; based on code from crow
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

 /* Support Large File */
#define _FILE_OFFSET_BITS 64

/* ***************************** */
/* Includes                      */
/* ***************************** */
#include "debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <memory.h>
#include <string.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/poll.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <stdint.h>
#include <signal.h>

#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
//#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 0, 100)
#include <libavcodec/bsf.h>
#include <libavcodec/avcodec.h>
//#endif

#include <ffmpeg/mpeg4audio.h>

#include "common.h"
#include "misc.h"
#include "debug.h"
#include "aac.h"
#include "pcm.h"
#include "ffmpeg_metadata.h"

/* ***************************** */
/* Makros/Constants              */
/* ***************************** */

#define HAVE_CH_LAYOUT (LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 0, 100))

/* Some STB with old kernels have problem with default 
 * read/write functions in ffmpeg which use open/read
 * due to this we set own which use fopen/fread from
 * std library.
 */
#define USE_CUSTOM_IO

/* Error Constants */
#define cERR_CONTAINER_FFMPEG_NO_ERROR        0
#define cERR_CONTAINER_FFMPEG_INIT           -1
#define cERR_CONTAINER_FFMPEG_NOT_SUPPORTED  -2
#define cERR_CONTAINER_FFMPEG_INVALID_FILE   -3
#define cERR_CONTAINER_FFMPEG_RUNNING        -4
#define cERR_CONTAINER_FFMPEG_NOMEM          -5
#define cERR_CONTAINER_FFMPEG_OPEN           -6
#define cERR_CONTAINER_FFMPEG_STREAM         -7
#define cERR_CONTAINER_FFMPEG_NULL           -8
#define cERR_CONTAINER_FFMPEG_ERR            -9
#define cERR_CONTAINER_FFMPEG_END_OF_FILE    -10

#define IPTV_AV_CONTEXT_MAX_NUM 2
/* ***************************** */
/* Types                         */
/* ***************************** */
typedef enum {RTMP_NATIVE, RTMP_LIBRTMP, RTMP_NONE} eRTMPProtoImplType;

/* ***************************** */
/* Varaibles                     */
/* ***************************** */

static pthread_rwlock_t mutex = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t seek_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_t PlayThread;
static int32_t hasPlayThreadStarted = 0;

static AVFormatContext *avContextTab[IPTV_AV_CONTEXT_MAX_NUM] = {NULL, NULL};
static int32_t use_custom_io[IPTV_AV_CONTEXT_MAX_NUM] = {0, 0};
static AVDictionary *g_avio_opts = NULL;

static uint8_t isContainerRunning = 0;

static int64_t latestPts = 0;

static int32_t restart_audio_resampling = 0;

static int64_t g_seek_target_seconds = 0;
static bool g_do_seek_target_seconds = false;
static void *g_stamp;

/* ***************************** */
/* Prototypes                    */
/* ***************************** */
static int32_t container_ffmpeg_seek_bytes(off_t pos);
static int32_t container_ffmpeg_seek(Context_t *context, int64_t sec, uint8_t absolute);
static int32_t container_ffmpeg_get_length(Context_t *context, int64_t *length);
static int64_t calcPts(AVStream *stream, int64_t pts);
static int64_t doCalcPts(const AVRational time_base, int64_t pts);
void LinuxDvbBuffSetStamp(void *stamp);

/* Progressive playback means that we play local file
 * but this local file can grows up, for example 
 * we start playback before download was finished 
 */
static int32_t progressive_playback = 0;
void progressive_playback_set(int32_t val)
{
    progressive_playback = val;
}

#define getMutex(...) getMutex_uni(0, ##__VA_ARGS__)
#define getMutex_wr(...) getMutex_uni(1, ##__VA_ARGS__)

static void getMutex_uni(int type, const char *filename __attribute__((unused)), const char *function __attribute__((unused)), int32_t line)
{
    ffmpeg_printf(100, "::%d requesting mutex\n", line);

    if(type == 0)
    {
        pthread_rwlock_rdlock(&mutex);
    }
    else
    {
        pthread_rwlock_wrlock(&mutex);
    }

    ffmpeg_printf(100, "::%d received mutex\n", line);
}

static void releaseMutex(const char *filename __attribute__((unused)), const const char *function __attribute__((unused)), int32_t line) 
{
    pthread_rwlock_unlock(&mutex);

    ffmpeg_printf(100, "::%d released mutex\n", line);
}

static void getSeekMutex()
{
    pthread_mutex_lock(&seek_mutex);
}

static void releaseSeekMutex() 
{
    pthread_mutex_unlock(&seek_mutex);
}

typedef int32_t (* Write_FN) (void  *, void *);

/* Zeitlupen-Milderung (Phase 2): rein additive Drosselung, KEINE Hardware-
 * Flush/Reset-Kommandos. Diagnose zeigte, dass der Demuxer unter
 * Netzwerklatenz weit vor der tatsaechlichen Hardware-Decoder-Position
 * (OUTPUT_PTS) herauslaufen kann (in Tests bis zu 155s), was sich als
 * "Zeitlupe" aeussert. select()-basierte Drosselung in WriteWithRetry greift
 * erst sehr spaet, weil der Hardware-Puffer offenbar grosszuegig dimensioniert
 * ist. Diese Ergaenzung bremst den Demux-Thread proaktiv, wenn der Abstand
 * zu gross wird. */
#define HLS_LATENCY_THROTTLE_THRESHOLD_90KHZ   (8 * 90000)  /* 8s Soll/Ist-Differenz */
#define HLS_LATENCY_THROTTLE_SLEEP_US          20000        /* 20ms pro betroffenem Write() */
#define HLS_LATENCY_THROTTLE_CHECK_INTERVAL_US 250000       /* PTS-Abfrage hoechstens alle 250ms */
#define HLS_LATENCY_THROTTLE_MIN_CONSECUTIVE   3            /* Entprellung gegen einzelne Ausreisser */

static int32_t Write(Write_FN WriteFun, void *context_, void *privateData, int64_t pts)
{
    /* Because Write is blocking we will release mutex which protect
     * avformat structures, during write time
     */
    int32_t ret = 0;
    Context_t *context = (Context_t *)context_;
    releaseMutex(__FILE__, __FUNCTION__,__LINE__);

    static int64_t lastThrottleCheckUs = 0;
    static int throttleConsecutiveOver = 0;

    if (pts > 0 && pts != INVALID_PTS_VALUE && context && context->playback &&
        context->playback->isPlaying && !context->playback->isSeeking &&
        !context->playback->isPaused && !context->playback->BackWard)
    {
        int64_t nowUs = av_gettime();
        if (nowUs - lastThrottleCheckUs >= HLS_LATENCY_THROTTLE_CHECK_INTERVAL_US)
        {
            lastThrottleCheckUs = nowUs;
            unsigned long long int hwPts = 0;
            if (context->output && context->output->Command &&
                0 == context->output->Command(context, OUTPUT_PTS, &hwPts) && hwPts > 0)
            {
                int64_t diff = pts - (int64_t)hwPts;
                if (diff > HLS_LATENCY_THROTTLE_THRESHOLD_90KHZ)
                {
                    throttleConsecutiveOver++;
                }
                else
                {
                    throttleConsecutiveOver = 0;
                }

                if (throttleConsecutiveOver >= HLS_LATENCY_THROTTLE_MIN_CONSECUTIVE)
                {
                    usleep(HLS_LATENCY_THROTTLE_SLEEP_US);
                }
            }
        }
    }

    ret = WriteFun(context, privateData);
    getMutex(__FILE__, __FUNCTION__,__LINE__);
    return ret;
}


#include "buff_ffmpeg.c"
#include "wrapped_ffmpeg.c"
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56, 34, 100)
#include "mpeg4p2_ffmpeg.c"
#endif

#ifdef HAVE_FLV2MPEG4_CONVERTER
#include "flv2mpeg4_ffmpeg.c"
#endif

/* This is also bad solution 
 * such configuration should passed maybe 
 * via struct 
 */
static int32_t wma_software_decode = 0;
static int32_t aac_software_decode = 0;
#ifdef __sh__
static int32_t aac_latm_software_decode = 1;
#else
static int32_t aac_latm_software_decode = 0;
#endif

static int32_t ac3_software_decode = 0;
static int32_t eac3_software_decode = 0;
static int32_t dts_software_decode = 0;
static int32_t amr_software_decode = 1;
static int32_t vorbis_software_decode = 1;
static int32_t opus_software_decode = 1;

static int32_t pcm_resampling = 1;
static int32_t stereo_software_decoder = 0;
static int32_t insert_pcm_as_lpcm = 0;
static int32_t mp3_software_decode = 0;
static int32_t rtmp_proto_impl = 0; // 0 - auto, 1 - native, 2 - librtmp

static int32_t g_sel_program_id = -1;
static int32_t g_hls_quality_mode = 0; /* 0 = auto (highest bandwidth, same as 2), 1 = lowest bandwidth, 2 = highest bandwidth */
static int32_t g_hls_audio_default_only = 0; /* 1 = keep only the DEFAULT=YES audio rendition per audio group */

#ifdef HAVE_FLV2MPEG4_CONVERTER
static int32_t flv2mpeg4_converter = 1;
#else
static int32_t flv2mpeg4_converter = 0;
#endif

/* ***************************** */
/* MISC Functions                */
/* ***************************** */

static void ffmpeg_silen_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    return;
}

void sel_program_id_set(const int32_t val)
{
    g_sel_program_id = val;
}

void hls_quality_mode_set(const int32_t val)
{
    g_hls_quality_mode = val;
}

void hls_audio_default_only_set(const int32_t val)
{
    g_hls_audio_default_only = val;
}

void verbose_logging_set(const int32_t val)
{
    g_verbose_logging = val;
}

void wma_software_decoder_set(const int32_t val)
{
    wma_software_decode = val;
}

void aac_software_decoder_set(const int32_t val)
{
    aac_software_decode = val;
}

void aac_latm_software_decoder_set(const int32_t val)
{
    aac_latm_software_decode = val;
}

void ac3_software_decoder_set(const int32_t val)
{
    ac3_software_decode = val;
}

void eac3_software_decoder_set(const int32_t val)
{
    eac3_software_decode = val;
}

void dts_software_decoder_set(const int32_t val)
{
    dts_software_decode = val;
}

void amr_software_decoder_set(const int32_t val)
{
    amr_software_decode = val;
}

void vorbis_software_decoder_set(const int32_t val)
{
    vorbis_software_decode = val;
}

void opus_software_decoder_set(const int32_t val)
{
    opus_software_decode = val;
}

void stereo_software_decoder_set(const int32_t val)
{
    stereo_software_decoder = val;
}

void insert_pcm_as_lpcm_set(const int32_t val)
{
    insert_pcm_as_lpcm = val;
}

void pcm_resampling_set(const int32_t val)
{
    pcm_resampling = val;
}

void mp3_software_decoder_set(const int32_t val)
{
    mp3_software_decode = val;
}

/* PCM-Audio-Export: schreibt bereits dekodiertes PCM-Audio zusaetzlich in
 * eine feste Named Pipe, damit Drittanbieter-Plugins (z.B. Live-Untertitel
 * per Spracherkennung) live mitlesen koennen, ohne selbst eine zweite
 * Verbindung zum Stream aufzumachen oder eigene Demux-/Sync-Logik zu bauen.
 * Rein opt-in (Default aus), da echte CPU-Mehrlast durch erzwungene
 * Software-Decodierung (siehe main() in exteplayer.c). Muss die normale
 * Wiedergabe unter allen Umstaenden unbeeinflusst lassen, auch ganz ohne
 * Leser an der Pipe - daher ausschliesslich nicht-blockierende Syscalls
 * im Hot Path, kein zusaetzlicher Thread, keine Locks. */
#define PCM_AUDIO_EXPORT_FIFO_PATH "/tmp/exteplayer3_pcm_audio.fifo"
#define PCM_AUDIO_EXPORT_RECONNECT_INTERVAL_US (2 * 1000 * 1000)
#define PCM_AUDIO_EXPORT_MAGIC 0x504D4341u /* "ACMP" little-endian = "PCMA" */
#define PCM_AUDIO_EXPORT_MAX_FRAME_BYTES (64 * 1024)

static int32_t g_pcm_audio_export_enabled = 0;
static int g_pcm_audio_export_fd = -1;
static int64_t g_pcm_audio_export_last_open_attempt_us = 0;

#pragma pack(push, 1)
struct PcmAudioExportHeader
{
    uint32_t magic;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t payload_len;
};
#pragma pack(pop)

void pcm_audio_export_set(const int32_t val)
{
    g_pcm_audio_export_enabled = val;

    if (val)
    {
        /* Ohne diese Absicherung wuerde ein write() auf die FIFO, sobald der
         * Leser weggeht (EPIPE), den kompletten Wiedergabeprozess per
         * Default-Signal-Disposition terminieren. Nur gesetzt, wenn das
         * Feature tatsaechlich aktiv ist - kein anderer Codepfad in diesem
         * Prozess verlaesst sich auf SIGPIPE-Default-Verhalten. */
        signal(SIGPIPE, SIG_IGN);

        if (mkfifo(PCM_AUDIO_EXPORT_FIFO_PATH, 0666) != 0 && errno != EEXIST)
        {
            RAW_DEBUG_LOG("pcm_audio_export_set: mkfifo(%s) failed: %s\n",
                           PCM_AUDIO_EXPORT_FIFO_PATH, strerror(errno));
        }
    }
}

int32_t pcm_audio_export_get(void)
{
    return g_pcm_audio_export_enabled;
}

static void pcm_audio_export_write(const uint8_t *pcm_data, uint32_t pcm_len, uint32_t sample_rate, uint16_t channels)
{
    if (!g_pcm_audio_export_enabled || pcm_len == 0)
        return;

    if (g_pcm_audio_export_fd < 0)
    {
        int64_t now = av_gettime();
        if (now - g_pcm_audio_export_last_open_attempt_us < PCM_AUDIO_EXPORT_RECONNECT_INTERVAL_US)
            return; /* kuerzlich schon erfolglos versucht, kein Syscall-Sturm ohne Leser */

        g_pcm_audio_export_last_open_attempt_us = now;
        /* O_NONBLOCK auf einer Named Pipe blockiert lt. POSIX fifo(7) nie -
         * ohne Leser schlaegt open() sofort mit ENXIO fehl. */
        g_pcm_audio_export_fd = open(PCM_AUDIO_EXPORT_FIFO_PATH, O_WRONLY | O_NONBLOCK);
        if (g_pcm_audio_export_fd < 0)
            return;
    }

    if (pcm_len > PCM_AUDIO_EXPORT_MAX_FRAME_BYTES - sizeof(struct PcmAudioExportHeader))
        return; /* Frame zu gross fuer den Scratch-Buffer, komplett verwerfen statt es mittendrin abzuschneiden */

    static uint8_t scratch[PCM_AUDIO_EXPORT_MAX_FRAME_BYTES];
    struct PcmAudioExportHeader *hdr = (struct PcmAudioExportHeader *)scratch;
    hdr->magic = PCM_AUDIO_EXPORT_MAGIC;
    hdr->sample_rate = sample_rate;
    hdr->channels = channels;
    hdr->bits_per_sample = 16;
    hdr->payload_len = pcm_len;
    memcpy(scratch + sizeof(struct PcmAudioExportHeader), pcm_data, pcm_len);

    ssize_t written = write(g_pcm_audio_export_fd, scratch, sizeof(struct PcmAudioExportHeader) + pcm_len);
    if (written < 0)
    {
        if (errno == EPIPE)
        {
            /* Leser ist weg - schliessen und sofortigen Reconnect-Versuch
             * erlauben, da sich der Zustand nachweislich geaendert hat. */
            close(g_pcm_audio_export_fd);
            g_pcm_audio_export_fd = -1;
            g_pcm_audio_export_last_open_attempt_us = 0;
        }
        /* EAGAIN/EWOULDBLOCK (Puffer voll, Leser zu langsam): Frame
         * verwerfen, kein Retry-Loop im Hot Path. */
    }
    /* Eine Teilschreibung (0 < written < Gesamtlaenge) wird akzeptiert und
     * NICHT nachgeschrieben - PCM-Frames sind oft groesser als PIPE_BUF,
     * ein zweiter write()-Call im Hot Path waere zusaetzliche Blockier-
     * Flaeche. Der Leser muss laut Dokumentation auf die naechste
     * Magic-Sequenz resynchronisieren koennen. */
}

void rtmp_proto_impl_set(const int32_t val)
{
    rtmp_proto_impl = val;
}

void flv2mpeg4_converter_set(const int32_t val)
{
    flv2mpeg4_converter = val;
}

int32_t ffmpeg_av_dict_set(const char *key, const char *value, int32_t flags)
{
    return av_dict_set(&g_avio_opts, key, value, flags);
}

static char* Codec2Encoding(int32_t codec_id, int32_t media_type, uint8_t *extradata, int extradata_size, int profile, int32_t *version)
{
    ffmpeg_printf(10, "Codec ID: %d (%.8x)\n", codec_id, codec_id);
    switch (codec_id)
    {
    case AV_CODEC_ID_MPEG1VIDEO:
        return "V_MPEG1";
    case AV_CODEC_ID_MPEG2VIDEO:
        return "V_MPEG2";
    case AV_CODEC_ID_MJPEG:
        return "V_MJPEG";
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_H263P:
    case AV_CODEC_ID_H263I:
        return "V_H263";
    case AV_CODEC_ID_FLV1:
        return flv2mpeg4_converter ? "V_MPEG4" : "V_FLV";
    case AV_CODEC_ID_VP5:
    case AV_CODEC_ID_VP6:
    case AV_CODEC_ID_VP6F:
        return "V_VP6";
    case AV_CODEC_ID_VP8:
        return "V_VP8";
#if LIBAVCODEC_VERSION_MAJOR > 54
    case AV_CODEC_ID_VP9:
        return "V_VP9";
#endif
    case AV_CODEC_ID_RV10:
    case AV_CODEC_ID_RV20:
        return "V_RV20";
    case AV_CODEC_ID_RV30:
        return "V_RV30";
    case AV_CODEC_ID_RV40:
        return "V_RV40";
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 21, 100)
    case AV_CODEC_ID_AVS2:
        return "V_AVS2";
#endif
    case AV_CODEC_ID_MPEG4:
        return "V_MPEG4";
#if LIBAVCODEC_VERSION_MAJOR < 53
    case AV_CODEC_ID_XVID:
#endif
    case AV_CODEC_ID_MSMPEG4V1:
    case AV_CODEC_ID_MSMPEG4V2:
    case AV_CODEC_ID_MSMPEG4V3:
        return "V_MPEG4"; //"V_DIVX3";
    case AV_CODEC_ID_WMV1:
        *version = 1;
        return "V_WMV";
    case AV_CODEC_ID_WMV2:
        *version = 2;
        return "V_WMV";
    case AV_CODEC_ID_WMV3:
        *version = 3;
        return "V_WMV";
    case AV_CODEC_ID_VC1:
        return "V_VC1";
    case AV_CODEC_ID_H264:
#if LIBAVCODEC_VERSION_MAJOR < 54
    case AV_CODEC_ID_FFH264:
#endif
        return "V_MPEG4/ISO/AVC";
#if LIBAVCODEC_VERSION_INT > AV_VERSION_INT(55, 92, 100)
    case AV_CODEC_ID_HEVC:
    // case AV_CODEC_ID_H265:
        return "V_HEVC";
#endif
    case AV_CODEC_ID_AVS:
        return "V_AVS";
    case AV_CODEC_ID_MP2:
        return "A_MPEG/L3";
    case AV_CODEC_ID_MP3:
        return (mp3_software_decode) ? "A_IPCM" : "A_MP3";
    case AV_CODEC_ID_AAC:
        if (extradata_size >= 2) {
            MPEG4AudioConfig m4ac;
            int off = avpriv_mpeg4audio_get_config(&m4ac, extradata, extradata_size * 8, 1);
            ffmpeg_printf(1,"aac [%d] off[%d]\n", m4ac.object_type, off);
            if (off < 0) {
                return "A_IPCM";
            }
            else if (0 == m4ac.chan_config && STB_HISILICON != GetSTBType()) {
                // according to https://wiki.multimedia.cx/index.php/ADTS
                // "MPEG-4 Channel Configuration  - in the case of 0, the channel configuration is sent via an inband PCE"	
                // we already have AAC_LATM formatter which will include PCE
                return (aac_latm_software_decode) ? "A_IPCM" : "A_AAC_LATM";
            }
        }
        return (aac_software_decode) ? "A_IPCM" : "A_AAC";
    case AV_CODEC_ID_AAC_LATM:
        return (aac_latm_software_decode) ? "A_IPCM" : "A_AAC_LATM";
    case AV_CODEC_ID_AC3:
        return  (ac3_software_decode) ? "A_IPCM" : "A_AC3";
    case AV_CODEC_ID_EAC3:
        return  (eac3_software_decode) ? "A_IPCM" : "A_EAC3";
    case AV_CODEC_ID_DTS:
        return (dts_software_decode) ? "A_IPCM" : "A_DTS";
    case AV_CODEC_ID_WMAV1:
        return "A_IPCM";
    case AV_CODEC_ID_WMAV2:
            return (wma_software_decode) ? "A_IPCM" : "A_WMA";
    case AV_CODEC_ID_WMAPRO:
        return (wma_software_decode) ? "A_IPCM" : "A_WMA/PRO";
    case AV_CODEC_ID_WMALOSSLESS:
        return "A_IPCM";
    case AV_CODEC_ID_MLP:
        return "A_IPCM";
    case AV_CODEC_ID_RA_144:
        return "A_IPCM";
    case AV_CODEC_ID_RA_288:
        return "A_IPCM";
    case AV_CODEC_ID_FLAC:
        return "A_IPCM";
    case AV_CODEC_ID_PCM_S8:
    case AV_CODEC_ID_PCM_U8:
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_U16LE:
    case AV_CODEC_ID_PCM_U16BE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S24BE:
    case AV_CODEC_ID_PCM_U24LE:
    case AV_CODEC_ID_PCM_U24BE:
    case AV_CODEC_ID_PCM_S32LE:
    case AV_CODEC_ID_PCM_S32BE:
    case AV_CODEC_ID_PCM_U32LE:
    case AV_CODEC_ID_PCM_U32BE:
        return pcm_resampling ? "A_IPCM" : "A_PCM";
    case AV_CODEC_ID_AMR_NB:
    case AV_CODEC_ID_AMR_WB:
        return amr_software_decode ? "A_IPCM" : "A_AMR";
    case AV_CODEC_ID_VORBIS:
        return vorbis_software_decode ? "A_IPCM" : "A_VORBIS";
    case AV_CODEC_ID_OPUS :
        return opus_software_decode ? "A_IPCM" : "A_OPUS";

/* In exteplayer3 embedded text subtitle simple printed
 * to output like other data.  Maybe worth to consider is to use 
 * linux socket or pipe to put
 */

/* subtitle */
    case AV_CODEC_ID_SSA:
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55, 3, 100)
    case AV_CODEC_ID_ASS:
#endif
        return "S_TEXT/ASS"; /* Hellmaster1024: seems to be ASS instead of SSA */
    case AV_CODEC_ID_DVD_SUBTITLE:
    case AV_CODEC_ID_DVB_TELETEXT:
//    case CODEC_ID_DVB_TELETEXT:
//        return "S_TEXT/SRT"; /* fixme */
    case AV_CODEC_ID_MOV_TEXT:
        return "S_TEXT/MOV";
    case AV_CODEC_ID_TEXT: ///< raw UTF-8 text
        return "S_TEXT/UTF-8";
    case AV_CODEC_ID_SRT:
        return "S_TEXT/SRT";
    case AV_CODEC_ID_SUBRIP:
        return "S_TEXT/SUBRIP"; 
    case AV_CODEC_ID_WEBVTT:
        return "S_TEXT/WEBVTT";
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
        return "S_GRAPHIC/PGS";
    case AV_CODEC_ID_DVB_SUBTITLE:
        return "S_GRAPHIC/DVB";
    case AV_CODEC_ID_XSUB:
        return "S_GRAPHIC/XSUB";
    default:
        ffmpeg_err("Codec ID %d (%.8x) not found\n", codec_id, codec_id);
        // Default to injected-pcm for unhandled audio types.
        if (media_type == AVMEDIA_TYPE_AUDIO)
        {
            return "A_IPCM";
        }
        ffmpeg_err("Codec ID %d (%.8x) not found\n", codec_id, codec_id);
    }
    return NULL;
}

/* Frueher wurde hier zusaetzlich der EINMALIG beim Session-Start von FFmpeg
 * gesetzte, globale AVFormatContext->start_time abgezogen. Das normalisierte
 * zwar den allerersten Clip einer Session sauber auf ~0, verglich aber JEDEN
 * spaeteren Clip (jeder Werbespot, jeder Einspieler) fuer immer gegen diese
 * eine fremde Referenz -- ob ein Clip damit einen gueltigen oder negativen
 * (=INVALID_PTS_VALUE) Wert ergab, hing so rein davon ab, was zufaellig
 * zuerst in der Session lief, nicht vom Clip selbst (beobachtet .11
 * 2026-07-26: derselbe Werbespot lief fehlerfrei, wenn er zuerst kam, aber
 * blieb haengen, wenn ein anderer Clip vorher schon die Referenz gesetzt
 * hatte). Die Normalisierung passiert jetzt ausschliesslich ueber den
 * Pointer-basierten Offset-Cache (resolveAndCachePtsOffset/
 * lookupCachedPtsOffset), der jeden Stream-Pointer unabhaengig von den
 * anderen behandelt. */
static int64_t doCalcPts(const AVRational time_base, int64_t pts)
{
    if (time_base.den > 0)
    {
        pts = av_rescale(pts, (int64_t)time_base.num * 90000, time_base.den);
    }

    if (pts & 0x8000000000000000ull)
    {
        pts = INVALID_PTS_VALUE;
    }
    else
    {
        pts &= 0x01FFFFFFFFull; // PES header can handle only 33 bit PTS
    }

    return pts;
}

static int64_t calcPts(AVStream *stream, int64_t pts)
{
    if (!stream || pts == (int64_t)AV_NOPTS_VALUE)
    {
        ffmpeg_err("stream / packet null\n");
        return INVALID_PTS_VALUE;
    }

    return doCalcPts(stream->time_base, pts);
}

static int64_t applyPtsOffset(int64_t rawPts, int64_t offset)
{
    if (rawPts == INVALID_PTS_VALUE)
    {
        return INVALID_PTS_VALUE;
    }

    int64_t corrected = rawPts - offset;

    /* negativ oder > 33-Bit-PES-Grenze ist unplausibel - lieber verwerfen
     * als ein Fantasie-PTS an die Hardware schreiben. */
    if (corrected < 0 || corrected > 0x1FFFFFFFFLL)
    {
        return INVALID_PTS_VALUE;
    }

    return corrected;
}

#define PTS_OFFSET_CACHE_SIZE 4

/* Schwellwert fuer die Sprungerkennung bei gleichbleibendem Stream-Pointer
 * (90kHz-PTS-Takt). Urspruenglich 30s; experimentell auf 3s gesenkt
 * (2026-07-26), da ein beobachtetes Wiederholungs-Muster bei einem
 * Werbe-Uebergang eine interne ~30s-Oszillation zeigte, die exakt an der
 * alten Schwelle vorbeirutschte (30,0s, nicht > 30s). 3s liegt weiterhin
 * weit ueber normalem Frame-zu-Frame-Jitter/B-Frame-Reordering (typischerweise
 * << 1s), aber komfortabel unter jeder bisher beobachteten echten
 * Splice-Groessenordnung. Durch die Cross-Track-Kopplung und die
 * Plausibilitaetspruefung ist ein zu frueh ausgeloester Relearn ungefaehrlich
 * (er verankert sich wieder an der bestehenden Kontinuitaet) -- daher
 * vertretbar, auch ohne abschliessend geklaerten Beweis fuer den exakten
 * Nutzen in diesem einen Fall. */
#define PTS_JUMP_RELEARN_THRESHOLD (3LL * 90000LL)

/* Cross-Track-Kopplung gegen dauerhaften AV-Versatz (beobachtet .11
 * 2026-07-26: Video wechselt auf einen neuen Pointer und lernt einen Offset
 * rein aus seiner eigenen Historie, waehrend Audio unveraendert beim alten
 * Pointer bleibt -- Ergebnis war ein dauerhafter Versatz von 5,83s). Siehe
 * resolveAndCachePtsOffset() fuer die Verwendung. Zwei Absicherungen:
 * (1) knownGoodAVDelta nur bei plausibler Differenz nachziehen, damit ein
 * einseitiger Haenger (eine Spur steht, die andere laeuft normal) das Delta
 * nicht auf einen Fantasiewert hochtreibt; (2) Cross-Anchoring nur, wenn die
 * andere Spur "frisch" ist (kuerzlich noch ein gueltiges Update hatte),
 * sonst wuerde z.B. ein komplett stummer Werbespot die andere Spur an einen
 * minutenalten Wert zurückreissen. */
static int64_t knownGoodAVDelta = 0; /* erwartete audioPts - videoPts Differenz */
#define AV_DELTA_PLAUSIBLE_LIMIT (2LL * 90000LL)
#define OTHER_TRACK_FRESHNESS_LIMIT 200

/* Diagnose (2026-07-26): misst den TATSAECHLICHEN Verlauf von knownGoodAVDelta
 * direkt, statt ihn nur indirekt aus Offset-Werten verschiedener Relearn-
 * Ereignisse zu erschliessen -- Grundlage, um zwischen "driftet durch
 * kontinuierliches Update" und "pendelt sich auf einen stabilen Puffer-Bias
 * ein" zu unterscheiden. Wertbasiert statt zeitbasiert gedrosselt (nur bei
 * Aenderung >= 100ms), keine time_t-Arithmetik in der Drossel-Bedingung --
 * bewusst anders als der fehlerhafte Zeit-basierte Ansatz vom selben Tag. */
static int64_t lastLoggedAVDelta = -999999;
#define AV_DELTA_LOG_STEP (9000LL) /* 100ms */

#define AV_DELTA_EMA_DIVISOR 32 /* Glaettungsfaktor des gleitenden Mittelwerts */

static void updateKnownGoodAVDelta(int64_t currentDelta, bool otherTrackFreshForDelta)
{
    /* Die Plausibilitaetsgrenze (< 2s) allein reicht nicht, wenn eine Spur
     * laenger haengt (z.B. Video mehrere Minuten eingefroren) - die andere
     * Spur laeuft normal weiter, currentDelta waechst dabei SCHRITTWEISE durch
     * den plausiblen
     * Bereich (jeder einzelne Schritt < 2s), bis es beim naechsten echten
     * Sprung mit einem bereits korrumpierten Delta (~1,66s beobachtet)
     * verankert wird. Die bestehende Frische-Pruefung schuetzte bisher nur
     * die Anker-Wahl (own/cross), nicht die Delta-Aktualisierung selbst --
     * daher hier zusaetzlich: keine Aktualisierung, wenn die andere Spur
     * gerade nicht frisch ist. */
    if (!otherTrackFreshForDelta)
    {
        return;
    }
    if (llabs(currentDelta) >= AV_DELTA_PLAUSIBLE_LIMIT)
    {
        return;
    }
    /* Gleitender Mittelwert statt direkter Uebernahme: currentDelta oszilliert
     * durch Demuxer-Interleaving kontinuierlich um +-100-115ms (direkt
     * gemessen, .11 2026-07-26, /tmp/exteplayer3_av_delta.log) -- bleibt dabei
     * ueber mehrere Relearn-Ereignisse hinweg stabil in derselben Bandbreite,
     * driftet nicht weiter aus. Eine direkte Uebernahme wuerde bei jedem
     * Cross-Anchor-Ereignis zufaellig eine der beiden Oszillations-Phasen
     * einfrieren. Der Mittelwert bleibt anders als ein einmaliges Einfrieren
     * weiterhin anpassungsfaehig, falls sich die tatsaechliche Beziehung
     * spaeter legitim aendert (z.B. anders codierte Werbe-Inhalte). */
    knownGoodAVDelta += (currentDelta - knownGoodAVDelta) / AV_DELTA_EMA_DIVISOR;

    if (g_verbose_logging && llabs(knownGoodAVDelta - lastLoggedAVDelta) >= AV_DELTA_LOG_STEP)
    {
        lastLoggedAVDelta = knownGoodAVDelta;
        FILE *dlog = fopen("/tmp/exteplayer3_av_delta.log", "a");
        if (dlog)
        {
            fprintf(dlog, "%ld AV_DELTA_CHANGE knownGoodAVDelta=%lld\n",
                    (long)time(NULL), (long long)knownGoodAVDelta);
            fclose(dlog);
        }
    }
}

/* Der own/cross-Trust-Mechanismus oben schuetzt nur die Anker-KETTE (Serien
 * von anchor=own waehrend die Gegenspur haengt). Er schuetzt NICHT die
 * Qualitaet eines einzelnen, formal frischen UND vertrauenswuerdigen
 * Cross-Deltas selbst. Beobachtet: nach einem laengeren (100s+) Totalausfall
 * beider Spuren gleichzeitig (keine neue Pakete, siehe Kommentar an
 * totalPacketCounter) sprang knownGoodAVDelta
 * trotz durchgehend anchor=cross binnen weniger Sekunden von ~0,3s auf
 * ~1,3s. Grund: OTHER_TRACK_FRESHNESS_LIMIT ist rein paketzaehlerbasiert --
 * waechst totalPacketCounter waehrend des gesamten Freezes kaum (weil BEIDE
 * Spuren gleichzeitig keine neuen Pakete bekommen), sieht der Paketabstand
 * beim gemeinsamen Wiederanlauf trotzdem "frisch" aus, obwohl in echter
 * Wanduhrzeit eine riesige Luecke bestand. Zusaetzlich starten Video und
 * Audio nach so einem Totalausfall nicht zwingend phasengleich wieder
 * (Ad-Stitcher-Neustart, unterschiedliche Puffertiefen) -- der allererste
 * Cross-Wert danach ist daher strukturell unzuverlaessiger als im
 * Normalbetrieb, in dem die Gegenspur durchgehend mitlief. */
#define AV_DELTA_FREEZE_GAP_SECONDS 15
#define AV_DELTA_CONFIRM_COUNT 4
#define AV_DELTA_CONFIRM_MAX_SPREAD (9000LL * 2) /* 200ms erlaubte Streuung zwischen den Bestaetigungswerten */

static int pendingConfirmCount = 0;
static int64_t pendingConfirmValues[AV_DELTA_CONFIRM_COUNT];

/* Wrapper um updateKnownGoodAVDelta(): nach einem erkannten langen Freeze
 * (resumedFromLongFreeze) werden die ersten AV_DELTA_CONFIRM_COUNT Cross-
 * Delta-Werte nur gesammelt statt sofort in die EMA zu uebernehmen. Erst wenn
 * sie untereinander konsistent sind (Streuung < AV_DELTA_CONFIRM_MAX_SPREAD),
 * wird ihr Mittelwert einmalig an updateKnownGoodAVDelta() weitergereicht.
 * Sind sie zu inkonsistent, wird die Bestaetigungsphase verworfen (kein
 * Update) statt einen unsicheren Wert zu uebernehmen -- der naechste erkannte
 * Freeze-Wiederanlauf (oder ein neuer PTS-Sprung im Normalbetrieb) bekommt
 * dann eine neue Chance. Ausserhalb einer Bestaetigungsphase (Normalbetrieb)
 * verhaelt sich der Wrapper transparent wie ein direkter Aufruf. */
static void feedAVDeltaSample(int64_t currentDelta, bool otherTrackFreshForDelta, bool resumedFromLongFreeze)
{
    if (!otherTrackFreshForDelta || llabs(currentDelta) >= AV_DELTA_PLAUSIBLE_LIMIT)
    {
        /* Bereits durch bestehende Fresh/Trust-Pruefung bzw. Plausibilitaets-
         * grenze blockiert -- eine laufende Bestaetigungsphase deswegen nicht
         * abbrechen, ein einzelner ungueltiger Zwischenwert soll sie nicht
         * neu erzwingen muessen. */
        return;
    }

    if (resumedFromLongFreeze)
    {
        pendingConfirmCount = 0; /* neuer Freeze -- alte Bestaetigungswerte verwerfen */
    }

    if (resumedFromLongFreeze || pendingConfirmCount > 0)
    {
        if (pendingConfirmCount < AV_DELTA_CONFIRM_COUNT)
        {
            pendingConfirmValues[pendingConfirmCount] = currentDelta;
            pendingConfirmCount++;
        }
        if (pendingConfirmCount < AV_DELTA_CONFIRM_COUNT)
        {
            return; /* noch nicht genug Bestaetigungswerte gesammelt */
        }

        int64_t minVal = pendingConfirmValues[0];
        int64_t maxVal = pendingConfirmValues[0];
        int64_t sum = pendingConfirmValues[0];
        int i;
        for (i = 1; i < AV_DELTA_CONFIRM_COUNT; i++)
        {
            if (pendingConfirmValues[i] < minVal) minVal = pendingConfirmValues[i];
            if (pendingConfirmValues[i] > maxVal) maxVal = pendingConfirmValues[i];
            sum += pendingConfirmValues[i];
        }
        pendingConfirmCount = 0; /* Bestaetigungsphase abgeschlossen (erfolgreich oder nicht) */

        if ((maxVal - minVal) >= AV_DELTA_CONFIRM_MAX_SPREAD)
        {
            if (g_verbose_logging)
            {
                FILE *dlog = fopen("/tmp/exteplayer3_av_delta.log", "a");
                if (dlog)
                {
                    fprintf(dlog, "%ld AV_DELTA_CONFIRM_REJECTED spread=%lld\n",
                            (long)time(NULL), (long long)(maxVal - minVal));
                    fclose(dlog);
                }
            }
            return;
        }
        currentDelta = sum / AV_DELTA_CONFIRM_COUNT;
    }

    updateKnownGoodAVDelta(currentDelta, true);
}

typedef struct
{
    void   *streamPtr;
    int64_t offset;
    int     learned;
    /* true = Offset wurde zuletzt gegen eine frische Gegenspur gelernt
     * (anchor=cross), false = nur gegen die eigene Historie (anchor=own).
     * Verhindert, dass ein Offset, der waehrend eines Freezes der Gegenspur
     * mehrfach nur gegen sich selbst neu gelernt wurde, per Cross-Anchor an
     * die Gegenspur weitergereicht wird oder in knownGoodAVDelta einsickert
     * (siehe resolveAndCachePtsOffset/updateKnownGoodAVDelta-Aufrufe unten;
     * Fund .11 2026-07-27: Video fror ~4,8s bei einem Mehrfach-Ad-Splice ein,
     * Audio lernte in dieser Zeit mehrfach hintereinander anchor=own und
     * driftete dabei auf einen um ~1,4s verschobenen Offset, der danach von
     * Video per Cross-Anchor uebernommen und von knownGoodAVDelta als neuer
     * "gueltiger" Wert geglaettet wurde). */
    bool    trusted;
} PtsOffsetCacheEntry_t;

/* Lernt (einmalig) oder nutzt einen bereits bekannten Offset fuer einen
 * konkreten AVStream-Pointer. Anders als eine einzelne veraenderliche
 * Offset-Variable wird der Offset pro Pointer dauerhaft eingefroren:
 * springt der Demuxer waehrend eines Ad-Splices ueber mehrere Sekunden
 * zwischen zwei AVStream-Objekten hin und her (beobachtet 2026-07-25/26),
 * wird fuer beide Pointer nur je einmal gelernt statt bei jedem Wechsel neu
 * (und damit potentiell falsch) berechnet zu werden.
 *
 * Zusaetzlich: manche Ad-Splices behalten denselben AVStream-Pointer bei,
 * aber der rohe Zeitstempel macht trotzdem einen riesigen Sprung (beobachtet
 * .11 2026-07-26: Sprung von PTS~33 Mio. auf ~8,58 Mrd., positiv und < 2^33,
 * besteht daher applyPtsOffset()'s Plausibilitaetspruefung unbemerkt). Da
 * hier kein Pointer-Wechsel als Signal existiert, wird zusaetzlich die
 * absolute Differenz zum letzten guten Wert ueberwacht: bei Ueberschreiten
 * von PTS_JUMP_RELEARN_THRESHOLD wird der Offset fuer denselben Pointer
 * sofort neu gelernt statt an einem erkennbar veralteten Wert festzuhalten.
 *
 * otherTrack*-Parameter beziehen sich auf die jeweils ANDERE Spur (Video
 * beim Audio-Aufruf, Audio beim Video-Aufruf) und dienen ausschliesslich dem
 * Cross-Track-Anchoring oben; isAudio steuert das Vorzeichen von
 * knownGoodAVDelta. packetCounterAtLastGoodPts wird bei jedem gueltigen
 * Update der EIGENEN Spur auf totalPacketCounter gesetzt. */
static int64_t resolveAndCachePtsOffset(PtsOffsetCacheEntry_t *cache, int cacheSize,
                                         void *streamPtr, int64_t rawPts,
                                         int64_t *lastGoodPts, int *nextSlot, const char *trackTag,
                                         int isAudio, int64_t otherTrackLastGoodPts,
                                         int64_t otherTrackPacketCounterAtLastGood,
                                         bool otherTrackTrusted,
                                         int64_t totalPacketCounter,
                                         int64_t *packetCounterAtLastGoodPts,
                                         time_t *wallClockAtLastGoodPts,
                                         bool *outTrusted,
                                         int64_t *sessionBasePts)
{
    bool otherTrackFresh = (otherTrackLastGoodPts >= 0) &&
            ((totalPacketCounter - otherTrackPacketCounterAtLastGood) < OTHER_TRACK_FRESHNESS_LIMIT);
    int64_t crossTarget = otherTrackLastGoodPts + (isAudio ? knownGoodAVDelta : -knownGoodAVDelta);

    int i;
    for (i = 0; i < cacheSize; i++)
    {
        if (cache[i].learned && cache[i].streamPtr == streamPtr)
        {
            int64_t corrected = applyPtsOffset(rawPts, cache[i].offset);
            bool needRelearn = false;

            if (corrected == INVALID_PTS_VALUE)
            {
                /* Das rohe PTS war gueltig, aber mit dem gecachten Offset
                 * verrechnet ergibt sich ein negativer/unplausibler Wert (z.B.
                 * ein neuer Werbespot mit deutlich niedrigerer eigener
                 * Zeitbasis als der bisherige Offset erwartet). Ohne diesen
                 * Zweig wuerde die Sprungerkennung unten uebersprungen und
                 * dieser Pointer koennte sich dauerhaft in INVALID_PTS_VALUE
                 * festfahren. (Ursache eines am selben Tag beobachteten
                 * Freezes war laut Paket-Zaehler-Log allerdings ein reines
                 * CDN-Datenloch: null Video-Pakete ueber 141s, nicht falsch
                 * verrechnete Werte.) */
                needRelearn = (rawPts != INVALID_PTS_VALUE && *lastGoodPts >= 0);
            }
            else if (*lastGoodPts >= 0 && llabs(corrected - *lastGoodPts) > PTS_JUMP_RELEARN_THRESHOLD)
            {
                needRelearn = true;
            }

            if (needRelearn)
            {
                /* otherTrackFresh entscheidet bereits oben, ob target der
                 * Gegenspur folgt (cross) oder nur der eigenen Historie (own)
                 * -- trusted spiegelt exakt das wider, damit ein spaeterer
                 * Cross-Anchor der Gegenspur bzw. ein knownGoodAVDelta-Update
                 * nicht auf einem rein selbstreferenziellen Offset aufbaut. */
                int64_t target = otherTrackFresh ? crossTarget : *lastGoodPts;
                int64_t newOffset = rawPts - target;
                cache[i].offset = newOffset;
                cache[i].trusted = otherTrackFresh;
                corrected = applyPtsOffset(rawPts, newOffset);

                if (g_verbose_logging)
                {
                    FILE *plog = fopen("/tmp/exteplayer3_pts_offset.log", "a");
                    if (plog)
                    {
                        fprintf(plog, "%ld %s relearn (jump) stream=%p offset=%lld anchor=%s\n",
                                (long)time(NULL), trackTag, streamPtr, (long long)newOffset,
                                otherTrackFresh ? "cross" : "own");
                        fclose(plog);
                    }
                }
            }
            if (corrected != INVALID_PTS_VALUE)
            {
                *lastGoodPts = corrected;
                *packetCounterAtLastGoodPts = totalPacketCounter;
                time_t now = time(NULL);
                bool resumedFromLongFreeze = (*wallClockAtLastGoodPts > 0) &&
                        ((now - *wallClockAtLastGoodPts) > AV_DELTA_FREEZE_GAP_SECONDS);
                *wallClockAtLastGoodPts = now;
                if (outTrusted)
                {
                    *outTrusted = cache[i].trusted;
                }
                if (otherTrackLastGoodPts >= 0)
                {
                    int64_t currentDelta = isAudio ? (corrected - otherTrackLastGoodPts)
                                                    : (otherTrackLastGoodPts - corrected);
                    /* Nur vertrauen, wenn die Gegenspur sowohl frisch ist ALS
                     * AUCH selbst zuletzt cross-verankert wurde -- sonst
                     * koennte ein waehrend eines Freezes der eigenen Spur nur
                     * gegen sich selbst gelernter Wert der Gegenspur unbemerkt
                     * in knownGoodAVDelta einsickern (siehe Kommentar an
                     * PtsOffsetCacheEntry_t.trusted). resumedFromLongFreeze
                     * faengt zusaetzlich den Fall ab, dass beide Spuren
                     * gleichzeitig lange standen (siehe feedAVDeltaSample). */
                    feedAVDeltaSample(currentDelta, otherTrackFresh && otherTrackTrusted, resumedFromLongFreeze);
                }
            }
            return corrected;
        }
    }

    if (rawPts == INVALID_PTS_VALUE)
    {
        return INVALID_PTS_VALUE;
    }

    int64_t offset;
    if (*lastGoodPts >= 0)
    {
        int64_t target = otherTrackFresh ? crossTarget : *lastGoodPts;
        offset = rawPts - target;
    }
    else
    {
        /* Erstes jemals gelerntes PTS dieser Spur in dieser Session (noch keine
         * eigene Kontinuitaet vorhanden) -- hier NIE auf die Gegenspur cross-
         * ankern, selbst wenn sie schon "frisch" ist. knownGoodAVDelta steht zu
         * Sessionbeginn noch auf dem Default 0, ein Cross-Anchor wuerde also
         * den tatsaechlichen, im Container gewollten Audio/Video-Versatz sofort
         * auf 0 zwingen und diesen Fehlwert ueber feedAVDeltaSample dauerhaft
         * festschreiben -- Stream lief danach permanent asynchron (beobachtet
         * bei einem Stream mit Referer-Header ueber StreamAnything, 2026-07-29).
         * Beide Spuren muessen ihren initialen Offset unabhaengig voneinander
         * aus dem eigenen rohen PTS lernen, um den nativen Versatz des
         * Containers zu erhalten. Der Cross-Anchor-Mechanismus bleibt fuer
         * seinen eigentlichen Zweck (Spur bekommt MITTEN im Stream einen neuen
         * Pointer, waehrend sie bereits Kontinuitaet hat) unveraendert aktiv --
         * das laeuft ueber den *lastGoodPts>=0-Zweig oben, nicht hier.
         *
         * Trotzdem soll die resultierende Position nicht den vollen rohen,
         * bei manchen Live-Streams wallclock-artigen PTS-Wert zeigen (z.B.
         * "Minuten seit dem letzten Reset des Webcam-Encoders"). Die zuerst
         * startende Spur setzt daher sessionBasePts einmalig auf ihren
         * eigenen rohen PTS, danach ziehen BEIDE Spuren bei ihrem jeweils
         * ersten Lernen denselben gemeinsamen Nullpunkt ab -- der natuerliche
         * A/V-Versatz bleibt dabei erhalten, nur die absolute Position
         * startet nahe 0 statt bei einem verwirrenden Fantasiewert. */
        if (sessionBasePts && *sessionBasePts < 0)
        {
            *sessionBasePts = rawPts;
        }
        offset = sessionBasePts ? *sessionBasePts : 0;
    }
    int slot = *nextSlot;
    cache[slot].streamPtr = streamPtr;
    cache[slot].offset = offset;
    cache[slot].learned = 1;
    /* otherTrackFresh bestimmt oben in beiden Zweigen (target=crossTarget
     * bzw. offset=rawPts-crossTarget) einheitlich, ob cross oder own/0
     * verwendet wurde -- siehe Kommentar an PtsOffsetCacheEntry_t.trusted. */
    cache[slot].trusted = otherTrackFresh;
    *nextSlot = (slot + 1) % cacheSize;

    if (g_verbose_logging)
    {
        FILE *plog = fopen("/tmp/exteplayer3_pts_offset.log", "a");
        if (plog)
        {
            fprintf(plog, "%ld %s learn stream=%p offset=%lld anchor=%s\n",
                    (long)time(NULL), trackTag, streamPtr, (long long)offset,
                    (*lastGoodPts < 0 && otherTrackFresh) ? "cross" : "own");
            fclose(plog);
        }
    }

    int64_t corrected = applyPtsOffset(rawPts, offset);
    if (corrected != INVALID_PTS_VALUE)
    {
        *lastGoodPts = corrected;
        *packetCounterAtLastGoodPts = totalPacketCounter;
        time_t now = time(NULL);
        bool resumedFromLongFreeze = (*wallClockAtLastGoodPts > 0) &&
                ((now - *wallClockAtLastGoodPts) > AV_DELTA_FREEZE_GAP_SECONDS);
        *wallClockAtLastGoodPts = now;
        if (outTrusted)
        {
            *outTrusted = cache[slot].trusted;
        }
        if (otherTrackLastGoodPts >= 0)
        {
            int64_t currentDelta = isAudio ? (corrected - otherTrackLastGoodPts)
                                            : (otherTrackLastGoodPts - corrected);
            feedAVDeltaSample(currentDelta, otherTrackFresh && otherTrackTrusted, resumedFromLongFreeze);
        }
    }
    return corrected;
}

/* Reine Anwendung eines bereits gelernten Offsets, ohne selbst zu lernen -
 * fuer DTS und abgeleitete Zeitstempel, die denselben Offset wie der
 * zugehoerige PTS derselben Spur/desselben Pointers verwenden muessen. */
static int64_t lookupCachedPtsOffset(PtsOffsetCacheEntry_t *cache, int cacheSize, void *streamPtr, int64_t rawPts)
{
    int i;
    for (i = 0; i < cacheSize; i++)
    {
        if (cache[i].learned && cache[i].streamPtr == streamPtr)
        {
            return applyPtsOffset(rawPts, cache[i].offset);
        }
    }
    return INVALID_PTS_VALUE;
}

/* search for metatdata in context and stream
 * and map it to our metadata.
 */

static char* searchMeta(void * metadata, char* ourTag)
{
#if LIBAVCODEC_VERSION_MAJOR < 54
   AVMetadataTag *tag = NULL;
#else
   AVDictionaryEntry *tag = NULL;
#endif
   int i = 0;

   while (metadata_map[i] != NULL)
   {
      if (strcmp(ourTag, metadata_map[i]) == 0)
      {
#if LIBAVCODEC_VERSION_MAJOR < 54
          while ((tag = av_metadata_get(metadata, "", tag, AV_METADATA_IGNORE_SUFFIX)))
#else
          while ((tag = av_dict_get(metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
#endif
          {
              if (strcmp(tag->key, metadata_map[ i + 1 ]) == 0)
              {
                  return tag->value;
              }
          }
      }
      i++;
   }

   return NULL;
}

/* **************************** */
/* Worker Thread                */
/* **************************** */

static void FFMPEGThread(Context_t *context) 
{
    char threadname[17];
    strncpy(threadname, __func__, sizeof(threadname));
    threadname[16] = 0;
    prctl (PR_SET_NAME, (unsigned long)&threadname);
    AVPacket   packet;
    off_t   lastSeek = -1;
    int64_t lastPts = -1;
    int64_t currentVideoPts = -1;
    int64_t currentAudioPts = -1;
    
    /* lastVideoDts and lastAudioDts 
     * used in isTSLiveMode
     */
    int64_t lastVideoDts = -1;
    int64_t lastAudioDts = -1;

    /* Pointer-basierte PTS-Kontinuitaets-Korrektur an HLS-Splice-Grenzen, mit
     * Offset-Cache pro AVStream-Pointer statt einer einzelnen veraenderlichen
     * Offset-Variable (siehe resolveAndCachePtsOffset/lookupCachedPtsOffset):
     * springt der Demuxer waehrend eines Splices mehrfach zwischen zwei
     * Stream-Objekten hin und her, wird fuer jeden Pointer nur einmal
     * gelernt statt bei jedem Wechsel neu berechnet. */
    int64_t lastGoodVideoPts = -1;
    int64_t lastGoodAudioPts = -1;
    /* Wird von der zuerst startenden Spur (Audio oder Video, je nachdem was
     * zuerst ein Paket liefert) einmalig auf deren allerersten rohen PTS
     * gesetzt und danach von BEIDEN Spuren beim eigenen ersten Lernen als
     * gemeinsamer Nullpunkt abgezogen (siehe resolveAndCachePtsOffset).
     * Ohne das behaelt eine Spur ohne fremde Laengenangabe (z.B. eine reine
     * Live-Webcam-URL) ihren vollen rohen, oft wallclock-artigen PTS-Wert als
     * Wiedergabeposition - fuer den Nutzer sichtbar als ein Player, der beim
     * Start nicht bei 0:00 sondern bei einem beliebigen, verwirrenden Wert
     * (z.B. "seit dem letzten Reset des Webcam-Encoders") losläuft, obwohl
     * die eigentliche Zeitanzeige inzwischen korrekt mitzaehlt (Laengen-
     * Overflow bereits separat gefixt). Der natuerliche A/V-Versatz bleibt
     * erhalten, da beide Spuren denselben Nullpunkt abziehen. */
    int64_t sessionBasePts = -1;
    /* Spiegelt PtsOffsetCacheEntry_t.trusted der jeweils zuletzt verwendeten
     * Cache-Zeile -- wird der Gegenspur beim naechsten Aufruf als
     * otherTrackTrusted mitgegeben (siehe resolveAndCachePtsOffset). */
    bool lastGoodVideoPtsTrusted = false;
    bool lastGoodAudioPtsTrusted = false;
    /* Paket-Zaehler-Zeitstempel des jeweils letzten gueltigen lastGoodPts-
     * Updates - Grundlage fuer die Cross-Track-Frische-Pruefung in
     * resolveAndCachePtsOffset() (siehe dort). */
    int64_t packetCounterAtLastGoodVideoPts = 0;
    int64_t packetCounterAtLastGoodAudioPts = 0;
    /* Wanduhr-Gegenstueck zu packetCounterAtLastGoodPts -- deckt den Fall ab,
     * dass waehrend eines langen Freezes BEIDE Spuren gleichzeitig kaum neue
     * Pakete bekommen und der Paketzaehler dadurch faelschlich "frisch"
     * anzeigt (siehe feedAVDeltaSample). 0 = noch nie gesetzt. */
    time_t lastGoodVideoPtsWallClock = 0;
    time_t lastGoodAudioPtsWallClock = 0;
    PtsOffsetCacheEntry_t videoPtsOffsetCache[PTS_OFFSET_CACHE_SIZE];
    PtsOffsetCacheEntry_t audioPtsOffsetCache[PTS_OFFSET_CACHE_SIZE];
    memset(videoPtsOffsetCache, 0, sizeof(videoPtsOffsetCache));
    memset(audioPtsOffsetCache, 0, sizeof(audioPtsOffsetCache));
    int videoCacheNextSlot = 0;
    int audioCacheNextSlot = 0;

    int64_t totalPacketCounter = 0;

    int64_t multiContextLastPts[IPTV_AV_CONTEXT_MAX_NUM] = {INVALID_PTS_VALUE, INVALID_PTS_VALUE};
    
    int64_t showtime = 0;
    int64_t bofcount = 0;
    int32_t       err = 0;
    AudioVideoOut_t avOut;
    
    g_context = context;

    SwrContext *swr = NULL;
    AVFrame *decoded_frame = NULL;
    int32_t out_sample_rate = 44100;
#if HAVE_CH_LAYOUT
    AVChannelLayout out_channel_layout;
    av_channel_layout_default(&out_channel_layout, 2);
#else
    uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
#endif
    int32_t out_channels = 2;
    uint32_t cAVIdx = 0;

    // for seek
    int64_t seek_target_seconds = 0;
    bool do_seek_target_seconds = false;

    int64_t seek_target_bytes = 0;
    bool do_seek_target_bytes = false;
    int64_t prev_seek_time_sec = -1;
    void *stamp;

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56, 34, 100)
#ifdef __sh__
    Mpeg4P2Context *mpeg4p2_context = NULL;
#else
    Mpeg4P2Context *mpeg4p2_context = mpeg4p2_context_open();
#endif
#endif
#ifdef HAVE_FLV2MPEG4_CONVERTER
    Flv2Mpeg4Context flv2mpeg4_context;
    memset(&flv2mpeg4_context, 0, sizeof(Flv2Mpeg4Context));
#endif
    ffmpeg_printf(10, "\n");
    while ( context->playback->isCreationPhase )
    {
        ffmpeg_printf(10, "Thread waiting for end of init phase...\n");
        usleep(1000);
    }
    ffmpeg_printf(10, "Running!\n");

    uint32_t bufferSize = 0;
    context->output->Command(context, OUTPUT_GET_BUFFER_SIZE, &bufferSize);
    ffmpeg_printf(10, "bufferSize [%u]\n", bufferSize);

    int8_t isWaitingForFinish = 0;
    while ( context && context->playback && context->playback->isPlaying ) 
    {
        /* When user press PAUSE we call pause on AUDIO and VIDEO decoders, 
         * we will not wait here because we can still fill 
         * DVB drivers buffers at PAUSE time
         * 
         */
#ifdef __sh__
        /* ST DVB drivers skip data if they are written during pause 
         * so, we must wait here if there is not buffering queue
         */
        if (0 == bufferSize && context->playback->isPaused) 
        {
            ffmpeg_printf(20, "paused\n");
            reset_finish_timeout();
            usleep(10000);
            continue;
        }
#endif

        if (context->playback->isSeeking) 
        {
            ffmpeg_printf(10, "seeking\n");
            reset_finish_timeout();
            usleep(10000);
            continue;
        }

        getMutex(__FILE__, __FUNCTION__,__LINE__);

        if (!context->playback || !context->playback->isPlaying)
        {
            releaseMutex(__FILE__, __FUNCTION__,__LINE__);
            if(!isWaitingForFinish)
            {
                reset_finish_timeout();
            }
            continue;
        }

        getSeekMutex();
        if (g_do_seek_target_seconds) 
        {
            do_seek_target_seconds = g_do_seek_target_seconds;
            seek_target_seconds = g_seek_target_seconds;
            stamp = g_stamp;
            g_do_seek_target_seconds = false;
        }
        releaseSeekMutex();

        if (do_seek_target_seconds || do_seek_target_bytes) 
        {
            isWaitingForFinish = 0;
            if (do_seek_target_seconds)
            {
                ffmpeg_printf(10, "seek_target_seconds[%"PRId64"]\n", seek_target_seconds);
                uint32_t i = 0;
                for(; i<IPTV_AV_CONTEXT_MAX_NUM; i+=1)
                {
                    multiContextLastPts[i] = INVALID_PTS_VALUE;
                    if(NULL != avContextTab[i])
                    {
                        if (i == 1)
                        {
                            prev_seek_time_sec = seek_target_seconds;
                        }
                        if (avContextTab[i]->start_time != AV_NOPTS_VALUE)
                        {
                            seek_target_seconds += avContextTab[i]->start_time;
                        }
                        /* Ein Seek kann den Stream von einem bereits erreichten EOF
                         * wegbewegen (z.B. zurueckspringen nach Vorspulen ans Ende) -
                         * ohne diesen Reset wuerde der naechste, an sich harmlose
                         * Lesefehler an der neuen Position faelschlich sofortiges
                         * Schliessen statt der Netzwerk-Gnadenfrist ausloesen. */
                        isEOFReached[i] = 0;
                        //av_seek_frame(avContextTab[i], -1, seek_target_seconds, 0);
                        avformat_seek_file(avContextTab[i], -1, INT64_MIN, seek_target_seconds, INT64_MAX, 0);
                    }
                    else
                    {
                        break;
                    }
                }
                /* resolveAndCachePtsOffset() wurde fuer Live-Stream-Diskontinuitaeten
                 * (z.B. Werbe-Splices) gebaut und kann einen legitimen Seek nicht von
                 * einer ungewollten Diskontinuitaet unterscheiden - ein PTS-Sprung ueber
                 * PTS_JUMP_RELEARN_THRESHOLD loest "relearn" aus, das den Offset genau so
                 * waehlt, dass corrected == lastGoodPts bleibt (siehe dort), der Sprung
                 * wird also aktiv herausgerechnet und die Wiedergabe "kriecht" scheinbar
                 * an der alten Position weiter, obwohl der Demuxer bereits korrekt an der
                 * neuen Position liest. Reset erzwingt den "noch nie gelernt"-Zweig
                 * (offset = sessionBasePts statt Fortsetzung von lastGoodPts);
                 * sessionBasePts bleibt bewusst unangetastet (Datei-Start-Nullpunkt,
                 * nicht seek-abhaengig). Betrifft nur diesen Seek-Codepfad, die
                 * Werbesprung-Erkennung bei Live-Streams laeuft ueber denselben
                 * Mechanismus ausserhalb eines Seek-Befehls und bleibt unberuehrt. */
                lastGoodVideoPts = -1;
                lastGoodAudioPts = -1;
                memset(videoPtsOffsetCache, 0, sizeof(videoPtsOffsetCache));
                memset(audioPtsOffsetCache, 0, sizeof(audioPtsOffsetCache));
                reset_finish_timeout();
                /*
                if (bufferSize > 0)
                {
                    context->output->Command(context, OUTPUT_CLEAR, NULL);
                }
                */
            } 
            else
            {
                container_ffmpeg_seek_bytes(seek_target_bytes);
            }
            do_seek_target_seconds = false;
            do_seek_target_bytes = false;
            
            restart_audio_resampling = 1;
            currentVideoPts = -1;
            currentAudioPts = -1;
            latestPts = 0;

            // flush streams
            uint32_t i = 0;
            for(i=0; i<IPTV_AV_CONTEXT_MAX_NUM; i+=1)
            {
                if(NULL != avContextTab[i])
                {
                    if (i != 1)
                    {
                        wrapped_avcodec_flush_buffers(i);
                    }
                }
                else
                {
                    break;
                }
            }
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56, 34, 100)
            mpeg4p2_context_reset(mpeg4p2_context);
#endif
#ifdef HAVE_FLV2MPEG4_CONVERTER
            flv2mpeg4_context_reset(&flv2mpeg4_context);
#endif
        }

        int ffmpegStatus = 0;
        if(!isWaitingForFinish)
        {
            if(NULL != avContextTab[1])
            {
                if (prev_seek_time_sec >= 0)
                {
                    if (multiContextLastPts[0] != INVALID_PTS_VALUE) {
                        int64_t target = av_rescale(multiContextLastPts[0], AV_TIME_BASE, 90000);
                        avformat_seek_file(avContextTab[1], -1, INT64_MIN, target, INT64_MAX, 0);
                        prev_seek_time_sec = -1;
                        wrapped_avcodec_flush_buffers(1);
                        cAVIdx = 1;
                    } else {
                        cAVIdx = 0;
                    }
                } else {
                    if (multiContextLastPts[0] != INVALID_PTS_VALUE && multiContextLastPts[1] != INVALID_PTS_VALUE) {
                        cAVIdx = multiContextLastPts[0] < multiContextLastPts[1] ? 0 : 1;
                    } else {
                        cAVIdx = !cAVIdx;
                    }
                }
            }
            else
            {
                cAVIdx = 0;
            }
        }

        if (bufferSize > 0)
        {
            LinuxDvbBuffSetStamp(stamp);
        }

        if( !isWaitingForFinish )
        {
            ffmpegStatus = av_read_frame(avContextTab[cAVIdx], &packet);
        }

        if (!isWaitingForFinish && (ffmpegStatus == 0) )
        {
            int64_t pts = 0;
            int64_t dts = 0;
            Track_t *videoTrack = NULL;
            Track_t *audioTrack = NULL;
            Track_t *subtitleTrack = NULL;

            int32_t pid = avContextTab[cAVIdx]->streams[packet.stream_index]->id;
            /* Der Stream des AKTUELLEN Pakets, direkt aus dem Demuxer. Bewusst
             * nicht videoTrack->stream/audioTrack->stream verwenden: die werden
             * nur aufgefrischt, wenn sich die (bei HLS pro Variante konstante,
             * nicht pro Segment eindeutige) Id aendert - bleibt sie bei einem
             * Ad-Splice zufaellig gleich, zeigen Track->stream weiter auf ein
             * altes, gueltiges aber falsches AVStream-Objekt mit abweichender
             * time_base (beobachtet .11 2026-07-26: Werbespot-zu-Werbespot-
             * Uebergaenge liefen deswegen minutenlang in einer PTS-Schleife). */
            AVStream *pktStream = avContextTab[cAVIdx]->streams[packet.stream_index];

            multiContextLastPts[cAVIdx] = calcPts(pktStream, packet.pts);
            ffmpeg_printf(200, "Ctx %d PTS: %"PRId64" PTS[1] %"PRId64"\n", cAVIdx, multiContextLastPts[cAVIdx], multiContextLastPts[1]);

            /* Zaehlt JEDES von av_read_frame() erfolgreich gelieferte Paket,
             * unabhaengig von Typ/Routing/Discard -- Grundlage fuer die
             * paketzaehlerbasierte Cross-Track-Frische-Pruefung in
             * resolveAndCachePtsOffset() (siehe OTHER_TRACK_FRESHNESS_LIMIT). */
            totalPacketCounter++;

            reset_finish_timeout();
            if(avContextTab[cAVIdx]->streams[packet.stream_index]->discard != AVDISCARD_ALL)
            {
                if (context->manager->video->Command(context, MANAGER_GET_TRACK, &videoTrack) < 0)
                {
                    ffmpeg_err("error getting video track\n");
                }
                
                if (context->manager->audio->Command(context, MANAGER_GET_TRACK, &audioTrack) < 0)
                {
                    ffmpeg_err("error getting audio track\n");
                }
                
                if (context->manager->subtitle->Command(context, MANAGER_GET_TRACK, &subtitleTrack) < 0)
                {
                    ffmpeg_err("error getting subtitle track\n");
                }
            }
            else
            {
                ffmpeg_printf(1, "SKIP DISCARDED PACKET packed_size[%d] stream_index[%d] pid[%d]\n", packet.size, (int)packet.stream_index, pid);
            }

            /* Legt der HLS-/MPEGTS-Demuxer bei einem Segment- oder PMT-Uebergang
             * (z.B. Ad-Splice) ein neues AVStream-Objekt fuer dieselbe logische
             * Spur an, bekommt es eine neue eigene ->id. videoTrack->Id/
             * audioTrack->Id werden aber nur einmal beim initialen Track-Setup
             * gesetzt. Ohne Auffrischung schlaegt der Identitaetsabgleich
             * (Id == pid) ab diesem Punkt dauerhaft fehl -- Pakete kommen zwar
             * weiter an, werden aber nie mehr an den Decoder geschrieben (Bild/
             * Ton bleiben aus). Bei Typ-Uebereinstimmung aber Id-Mismatch daher
             * den Track auf den neuen Stream ummelden. */
            {
                if (videoTrack && videoTrack->AVIdx == cAVIdx && videoTrack->Id != pid &&
                    get_codecpar(pktStream)->codec_type == AVMEDIA_TYPE_VIDEO)
                {
                    bool haveValidDims = get_codecpar(pktStream)->width > 0 && get_codecpar(pktStream)->height > 0;

                    /* Frueher wurde die Id-/Stream-Ummeldung erst nach bis zu 20
                     * Paketen (oder sobald gueltige Dimensionen da waren) committet.
                     * In der Zwischenzeit passte weder Video- noch Audio-Id zum
                     * Paket, es wurde unbeobachtet verworfen - nachweislich echte
                     * Videopakete (siehe DROPPED-Log), was zu sichtbaren
                     * Standbildern fuehren kann. Jetzt: Id/Stream sofort umschalten
                     * (wie beim Audio-Pfad), Dimensionen aber erst uebernehmen,
                     * sobald sie tatsaechlich verfuegbar sind - bis dahin gelten die
                     * alten Werte als beste Vermutung weiter, aber es wird kein
                     * einziges Paket mehr verworfen. */
                    videoTrack->Id = pid;
                    videoTrack->stream = (void*)pktStream;
                    pktStream->discard = AVDISCARD_DEFAULT; /* sicherstellen, dass der neue Stream nicht als "nicht benoetigt" gilt */
                    if (haveValidDims)
                    {
                        videoTrack->width = get_codecpar(pktStream)->width;
                        videoTrack->height = get_codecpar(pktStream)->height;
                        videoTrack->aspect_ratio_num = pktStream->sample_aspect_ratio.num;
                        videoTrack->aspect_ratio_den = pktStream->sample_aspect_ratio.den;
                        if (0 == videoTrack->aspect_ratio_num || 0 == videoTrack->aspect_ratio_den)
                        {
                            videoTrack->aspect_ratio_num = get_codecpar(pktStream)->sample_aspect_ratio.num;
                            videoTrack->aspect_ratio_den = get_codecpar(pktStream)->sample_aspect_ratio.den;
                        }
                        {
                            AVRational rateRational = get_frame_rate(pktStream);
                            if (rateRational.den != 0)
                            {
                                videoTrack->frame_rate = (uint32_t)(1000 * (int64_t)(rateRational.num) / (int64_t)(rateRational.den));
                            }
                        }
                        videoTrack->TimeScale = (videoTrack->frame_rate < 23970) ? 1001 : 1000;
                    }
                    /* sonst: alte width/height/frame_rate/TimeScale/extraData behalten (beste Vermutung) */
                    if (get_codecpar(pktStream)->extradata_size > 0)
                    {
                        videoTrack->extraData = get_codecpar(pktStream)->extradata;
                        videoTrack->extraSize = get_codecpar(pktStream)->extradata_size;
                    }
                }
                else if (videoTrack && videoTrack->AVIdx == cAVIdx && videoTrack->Id == pid &&
                         get_codecpar(pktStream)->codec_type == AVMEDIA_TYPE_VIDEO &&
                         (0 == videoTrack->width || 0 == videoTrack->height) &&
                         get_codecpar(pktStream)->width > 0 && get_codecpar(pktStream)->height > 0)
                {
                    /* Dimensionen kamen erst ein paar Pakete NACH der sofortigen
                     * Id-Umschaltung an - jetzt nachtragen. */
                    videoTrack->width = get_codecpar(pktStream)->width;
                    videoTrack->height = get_codecpar(pktStream)->height;
                    videoTrack->aspect_ratio_num = pktStream->sample_aspect_ratio.num;
                    videoTrack->aspect_ratio_den = pktStream->sample_aspect_ratio.den;
                    if (0 == videoTrack->aspect_ratio_num || 0 == videoTrack->aspect_ratio_den)
                    {
                        videoTrack->aspect_ratio_num = get_codecpar(pktStream)->sample_aspect_ratio.num;
                        videoTrack->aspect_ratio_den = get_codecpar(pktStream)->sample_aspect_ratio.den;
                    }
                    {
                        AVRational rateRational = get_frame_rate(pktStream);
                        if (rateRational.den != 0)
                        {
                            videoTrack->frame_rate = (uint32_t)(1000 * (int64_t)(rateRational.num) / (int64_t)(rateRational.den));
                        }
                    }
                    videoTrack->TimeScale = (videoTrack->frame_rate < 23970) ? 1001 : 1000;
                }
                if (audioTrack && audioTrack->AVIdx == cAVIdx && audioTrack->Id != pid &&
                    get_codecpar(pktStream)->codec_type == AVMEDIA_TYPE_AUDIO)
                {
                    int32_t oldId = audioTrack->Id;
                    int32_t oldCodecId = audioTrack->stream ? (int32_t)get_codecpar((AVStream*)audioTrack->stream)->codec_id : -1;

                    audioTrack->Id = pid;
                    audioTrack->stream = (void*)pktStream;
                    pktStream->discard = AVDISCARD_DEFAULT;

                    /* Der Software-Decode-Pfad (inject_as_pcm, aktiv bei -a/-d/-l) haelt
                     * einen langlebigen AVCodecContext, der nur einmal beim initialen
                     * Track-Setup gegen den damaligen AVStream geoeffnet wurde. Ohne
                     * Neuoeffnung gegen den hier frisch zugewiesenen Stream werden
                     * Pakete mit ggf. geaenderter Codec-Config (z.B. LATM/LOAS-Extradata
                     * bei einem Ad-Splice) durch einen dafuer nicht mehr passenden
                     * Decoder-Kontext geschickt - Ton bleibt fuer mehrere Sekunden aus
                     * oder verzerrt, ohne dass ein Fehler geloggt wird. */
                    if (g_verbose_logging)
                    {
                        FILE *arlog = fopen("/tmp/exteplayer3_audio_reinit.log", "a");
                        if (arlog)
                        {
                            fprintf(arlog, "%ld AUDIO_ID_SWITCH oldId=%d newId=%d oldCodecId=%d newCodecId=%d inject_as_pcm=%d\n",
                                    (long)time(NULL), oldId, pid, oldCodecId,
                                    (int32_t)get_codecpar(pktStream)->codec_id, audioTrack->inject_as_pcm);
                            fclose(arlog);
                        }
                    }

                    if (audioTrack->inject_as_pcm == 1)
                    {
                        AVCodecContext *newCodecCtx = wrapped_avcodec_get_context(cAVIdx, pktStream);
                        const AVCodec *codec = newCodecCtx ? avcodec_find_decoder(get_codecpar(pktStream)->codec_id) : NULL;
                        int openRet = -1;

                        if (newCodecCtx && codec)
                        {
                            openRet = avcodec_open2(newCodecCtx, codec, NULL);
                            if (0 == openRet)
                            {
                                audioTrack->avCodecCtx = newCodecCtx;
                                restart_audio_resampling = 1;
                            }
                        }

                        if (g_verbose_logging)
                        {
                            FILE *arlog = fopen("/tmp/exteplayer3_audio_reinit.log", "a");
                            if (arlog)
                            {
                                fprintf(arlog, "%ld AUDIO_CODEC_REINIT newCodecCtx=%p codec=%p codecName=%s openRet=%d restart_audio_resampling=%d\n",
                                        (long)time(NULL), (void*)newCodecCtx, (void*)codec,
                                        codec ? codec->name : "-", openRet, restart_audio_resampling);
                                fclose(arlog);
                            }
                        }
                    }
                }
            }

            ffmpeg_printf(200, "packet.size %d - index %d\n", packet.size, pid);

            /* Bei einem Segment-/Discontinuity-Uebergang kann der Demuxer einem neuen
             * Stream zufaellig dieselbe ->id wie ein zuvor bekannter Track vergeben
             * (z.B. Video-id == alte Audio-id). Ohne diese Typ-Pruefung wuerden dann
             * Video- oder Metadaten-Pakete faelschlich als Audio (oder umgekehrt)
             * geroutet und an den falschen Decoder geschickt - dort werden sie als
             * ungueltige Daten abgelehnt (AVERROR_INVALIDDATA), Ton/Bild setzen aus. */
            if (videoTrack && (videoTrack->AVIdx == cAVIdx) && (videoTrack->Id == pid) &&
                get_codecpar(avContextTab[cAVIdx]->streams[packet.stream_index])->codec_type == AVMEDIA_TYPE_VIDEO)
            {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56, 34, 100)
                AVCodecContext *codec_context = videoTrack->avCodecCtx;
                if (codec_context && codec_context->codec_id == AV_CODEC_ID_MPEG4 && NULL != mpeg4p2_context)
                {
                    mpeg4p2_write_packet(context, mpeg4p2_context, videoTrack, &currentVideoPts, &latestPts, &packet);
                    update_max_injected_pts(latestPts);
                }
                else
#endif
#ifdef HAVE_FLV2MPEG4_CONVERTER
                if (get_codecpar(avContextTab[cAVIdx]->streams[packet.stream_index])->codec_id == AV_CODEC_ID_FLV1 &&
                    0 == memcmp(videoTrack->Encoding, "V_MPEG4", 7) )
                {
                    flv2mpeg4_write_packet(context, &flv2mpeg4_context, videoTrack, &currentVideoPts, &latestPts, &packet);
                    update_max_injected_pts(latestPts);
                }
                else
#endif
                {
                    bool skipPacket = false;
                    int64_t rawVideoPts = calcPts(pktStream, packet.pts);
                    currentVideoPts = videoTrack->pts = pts = resolveAndCachePtsOffset(
                            videoPtsOffsetCache, PTS_OFFSET_CACHE_SIZE, (void*)pktStream,
                            rawVideoPts, &lastGoodVideoPts, &videoCacheNextSlot, "VIDEO",
                            0, lastGoodAudioPts, packetCounterAtLastGoodAudioPts,
                            lastGoodAudioPtsTrusted,
                            totalPacketCounter, &packetCounterAtLastGoodVideoPts,
                            &lastGoodVideoPtsWallClock,
                            &lastGoodVideoPtsTrusted,
                            &sessionBasePts);
                    videoTrack->dts = dts = lookupCachedPtsOffset(
                            videoPtsOffsetCache, PTS_OFFSET_CACHE_SIZE, (void*)pktStream,
                            calcPts(pktStream, packet.dts));

                    if ((currentVideoPts != INVALID_PTS_VALUE) && (currentVideoPts > latestPts))
                    {
                        latestPts = currentVideoPts;
                        update_max_injected_pts(latestPts);
                    }
                    
                    if (context->playback->isTSLiveMode)
                    {
                        if (dts != INVALID_PTS_VALUE)
                        {   
                            if (dts > lastVideoDts)
                            {
                                lastVideoDts = dts;
                            }
                            else
                            {
                                // skip already injected VIDEO packet
                                ffmpeg_printf(200, "skip already injected VIDEO packet\n");
                                skipPacket = true;
                            }
                        }
                        else
                        {
                            // skip VIDEO packet with unknown DTS
                            ffmpeg_printf(200, "skip VIDEO packet with unknown DTS\n");
                            skipPacket = true;
                        }
                    }
                    
                    if (skipPacket)
                    {   
                        wrapped_packet_unref(&packet);
                        releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                        continue;
                    }
                    
                    ffmpeg_printf(200, "VideoTrack index = %d %"PRId64"\n",pid, currentVideoPts);

                    avOut.data       = packet.data;
                    avOut.len        = packet.size;
                    avOut.pts        = pts;
                    avOut.dts        = dts;
                    avOut.extradata  = videoTrack->extraData;
                    avOut.extralen   = videoTrack->extraSize;
                    avOut.frameRate  = videoTrack->frame_rate;
                    avOut.timeScale  = videoTrack->TimeScale;
                    avOut.width      = videoTrack->width;
                    avOut.height     = videoTrack->height;
                    avOut.type       = "video";
                    avOut.infoFlags  = 0;
                    
                    if (avContextTab[cAVIdx]->iformat->flags & AVFMT_TS_DISCONT)
                    {
                        avOut.infoFlags = 1; // TS container
                    }

                    /* INVALID_PTS_VALUE (0x200000000, ein Bit ueber der 33-Bit-PES-
                     * PTS-Grenze) NIE an die Hardware schreiben: wird der Wert beim
                     * Aufbau des PES-Headers auf 33 Bit gekuerzt, wird daraus PTS=0
                     * - ein scheinbar gueltiger, aber komplett falscher Zeitstempel,
                     * der die AV-Sync-Engine mit einem riesigen Ruecksprung
                     * konfrontiert (fuehrt zu Standbild). Paket lieber
                     * ueberspringen als mit Fantasie-PTS schreiben. */
                    if (pts != INVALID_PTS_VALUE)
                    {
                        if (Write(context->output->video->Write, context, &avOut, pts) < 0)
                        {
                            ffmpeg_err("writing data to video device failed\n");
                        }
                    }
                }
            }
            else if (audioTrack && (audioTrack->AVIdx == cAVIdx) && (audioTrack->Id == pid) &&
                     get_codecpar(avContextTab[cAVIdx]->streams[packet.stream_index])->codec_type == AVMEDIA_TYPE_AUDIO)
            {
                uint8_t skipPacket = 0;
                int64_t rawAudioPts = calcPts(pktStream, packet.pts);
                currentAudioPts = audioTrack->pts = pts = resolveAndCachePtsOffset(
                        audioPtsOffsetCache, PTS_OFFSET_CACHE_SIZE, (void*)pktStream,
                        rawAudioPts, &lastGoodAudioPts, &audioCacheNextSlot, "AUDIO",
                        1, lastGoodVideoPts, packetCounterAtLastGoodVideoPts,
                        lastGoodVideoPtsTrusted,
                        totalPacketCounter, &packetCounterAtLastGoodAudioPts,
                        &lastGoodAudioPtsWallClock,
                        &lastGoodAudioPtsTrusted,
                        &sessionBasePts);
                dts = lookupCachedPtsOffset(
                        audioPtsOffsetCache, PTS_OFFSET_CACHE_SIZE, (void*)pktStream,
                        calcPts(pktStream, packet.dts));

                if ((currentAudioPts != INVALID_PTS_VALUE) && (currentAudioPts > latestPts) && (!videoTrack))
                {
                    latestPts = currentAudioPts;
                    update_max_injected_pts(latestPts);
                }
                
                if (context->playback->isTSLiveMode)
                {
                    if (dts != INVALID_PTS_VALUE)
                    {
                        if (dts > lastAudioDts)
                        {
                            lastAudioDts = dts;
                        }
                        else
                        {
                            // skip already injected AUDIO packet
                            ffmpeg_printf(200, "skip already injected AUDIO packet\n");
                            skipPacket = 1;
                        }
                    }
                    else
                    {
                        // skip AUDIO packet with unknown PTS
                        ffmpeg_printf(200, "skip AUDIO packet with unknown PTS\n");
                        skipPacket = 1;
                    }
                }
                
                if (skipPacket)
                {
                    wrapped_packet_unref(&packet);
                    releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                    continue;
                }
                
                pcmPrivateData_t pcmExtradata;
#if HAVE_CH_LAYOUT
                pcmExtradata.channels              = get_codecpar(audioTrack->stream)->ch_layout.nb_channels;
#else
                pcmExtradata.channels              = get_codecpar(audioTrack->stream)->channels;
#endif
                pcmExtradata.bits_per_coded_sample = get_codecpar(audioTrack->stream)->bits_per_coded_sample;
                pcmExtradata.sample_rate           = get_codecpar(audioTrack->stream)->sample_rate;
                pcmExtradata.bit_rate              = get_codecpar(audioTrack->stream)->bit_rate;
                pcmExtradata.block_align           = get_codecpar(audioTrack->stream)->block_align;
                pcmExtradata.frame_size            = get_codecpar(audioTrack->stream)->frame_size;

                pcmExtradata.codec_id              = get_codecpar(audioTrack->stream)->codec_id;
                pcmExtradata.bResampling           = restart_audio_resampling;
                
                uint8_t *pAudioExtradata    = get_codecpar(audioTrack->stream)->extradata;
                uint32_t audioExtradataSize = get_codecpar(audioTrack->stream)->extradata_size;
                    
                ffmpeg_printf(200, "AudioTrack index = %d\n",pid);
                if (audioTrack->inject_raw_pcm == 1)
                {
                    ffmpeg_printf(200,"write audio raw pcm\n");
                    restart_audio_resampling = 0;

                    avOut.data       = packet.data;
                    avOut.len        = packet.size;
                    avOut.pts        = pts;
                    avOut.extradata  = (uint8_t *) &pcmExtradata;
                    avOut.extralen   = sizeof(pcmExtradata);
                    avOut.frameRate  = 0;
                    avOut.timeScale  = 0;
                    avOut.width      = 0;
                    avOut.height     = 0;
                    avOut.type       = "audio";

                    /* Anders als Video darf ein fehlender/ungueltiger PTS bei Audio
                     * NICHT zum Verwerfen des ganzen Pakets fuehren: bei MPEG-TS/HLS
                     * traegt oft nur das erste Audio-Frame eines PES-Pakets einen
                     * echten Zeitstempel, alle folgenden Frames im selben Paket haben
                     * keinen eigenen (AV_NOPTS_VALUE -> INVALID_PTS_VALUE). Die
                     * Hardware ignoriert einen ungueltigen PTS ohnehin und spielt
                     * einfach fortlaufend weiter - wurden diese Pakete verworfen statt
                     * nur mit unklarem Zeitstempel geschrieben, entstanden haeufige
                     * Aussetzer im Audiostrom (hoerbar als durchgehend verzerrter/
                     * "knatternder" Ton, u.a. bei AAC-Streams ueber HLS beobachtet). */
                    if (Write(context->output->audio->Write, context, &avOut, pts) < 0)
                    {
                        ffmpeg_err("(raw pcm) writing data to audio device failed\n");
                    }
                }
                else if (audioTrack->inject_as_pcm == 1 && audioTrack->avCodecCtx)
                {
                    AVCodecContext *c = audioTrack->avCodecCtx;

                    if (restart_audio_resampling)
                    {
                        restart_audio_resampling = 0;
                        if (swr)
                        {
                            swr_free(&swr);
                            swr = NULL;
                        }
                        if (decoded_frame)
                        {
                            wrapped_frame_free(&decoded_frame);
                            decoded_frame = NULL;
                        }
                    }
#if (LIBAVFORMAT_VERSION_MAJOR > 57) || ((LIBAVFORMAT_VERSION_MAJOR == 57) && (LIBAVFORMAT_VERSION_MINOR > 32))
                    while (packet.size > 0 || (!packet.size && !packet.data)) 
#else
                    while(packet.size > 0)
#endif
                    {
                        if(do_seek_target_seconds || do_seek_target_bytes)
                        {
                            break;
                        }
                        
                        if (!decoded_frame)
                        {
                            decoded_frame = wrapped_frame_alloc();
                            if (!decoded_frame)
                            {
                                ffmpeg_err("out of memory\n");
                                exit(1);
                            }
                        } 
                        else
                        {
                            wrapped_frame_unref(decoded_frame);
                        }
#if (LIBAVFORMAT_VERSION_MAJOR > 57) || ((LIBAVFORMAT_VERSION_MAJOR == 57) && (LIBAVFORMAT_VERSION_MINOR > 32))
                        int ret = avcodec_send_packet(c, &packet);
                        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                        {
                            if (g_verbose_logging)
                            {
                                FILE *arlog = fopen("/tmp/exteplayer3_audio_reinit.log", "a");
                                if (arlog)
                                {
                                    fprintf(arlog,
                                        "%ld AUDIO_SEND_PACKET_ERR ret=%d pktSize=%d ctxSampleRate=%d ctxChannels=%d "
                                        "ctxExtradataSize=%d dataHead=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                                        (long)time(NULL), ret, packet.size,
                                        c->sample_rate, c->channels, c->extradata_size,
                                        packet.size > 0 ? packet.data[0] : 0,
                                        packet.size > 1 ? packet.data[1] : 0,
                                        packet.size > 2 ? packet.data[2] : 0,
                                        packet.size > 3 ? packet.data[3] : 0,
                                        packet.size > 4 ? packet.data[4] : 0,
                                        packet.size > 5 ? packet.data[5] : 0,
                                        packet.size > 6 ? packet.data[6] : 0,
                                        packet.size > 7 ? packet.data[7] : 0);
                                    fclose(arlog);
                                }
                            }
                            restart_audio_resampling = 1;
                            break;
                        }

                        if (ret >= 0)
                        {
                            packet.size = 0;
                        }

                        ret = avcodec_receive_frame(c, decoded_frame);
                        if (ret < 0)
                        {
                            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                            {
                                if (g_verbose_logging)
                                {
                                    FILE *arlog = fopen("/tmp/exteplayer3_audio_reinit.log", "a");
                                    if (arlog)
                                    {
                                        fprintf(arlog, "%ld AUDIO_RECEIVE_FRAME_ERR ret=%d\n", (long)time(NULL), ret);
                                        fclose(arlog);
                                    }
                                }
                                restart_audio_resampling = 1;
                                break;
                            }
                            else
                            {
                                continue;
                            }
                        }
#else
                        int32_t got_frame = 0;
                        int32_t len = avcodec_decode_audio4(c, decoded_frame, &got_frame, &packet);
                        if (len < 0) 
                        {
                            ffmpeg_err("avcodec_decode_audio4: %d\n", len);
                            break;
                        }

                        packet.data += len;
                        packet.size -= len;
                        
                        if (!got_frame)
                        {
                            continue;
                        }
#endif
                        int32_t e = 0;
                        if (!swr) 
                        {
                            if(insert_pcm_as_lpcm)
                            {
                                out_sample_rate = 48000;
                            }
                            else
                            {
                                int32_t rates[] = { 48000, 96000, 192000, 44100, 88200, 176400, 0 };
                                int32_t *rate = rates;
                                int32_t in_rate = c->sample_rate;
                                while (*rate && ((*rate / in_rate) * in_rate != *rate) && (in_rate / *rate) * *rate != in_rate)
                                {
                                    rate++;
                                }
                                out_sample_rate = *rate ? *rate : 44100;
                            }

                            swr = swr_alloc();
#if HAVE_CH_LAYOUT
                            out_channels = c->ch_layout.nb_channels;
                            if( !av_channel_layout_check(&c->ch_layout) )
                            {
                                av_channel_layout_default( &c->ch_layout, out_channels );
                            }

                            av_channel_layout_copy(&out_channel_layout, &c->ch_layout);
#else
                            out_channels = c->channels;

                            if (c->channel_layout == 0)
                            {
                                c->channel_layout = av_get_default_channel_layout( c->channels );
                            }
                            out_channel_layout = c->channel_layout;
#endif

                            uint8_t downmix = stereo_software_decoder && out_channels > 2 ? 1 : 0;
#ifdef __sh__
                            // player2 won't play mono
                            if (out_channel_layout == AV_CH_LAYOUT_MONO)
                            {
                                downmix = 1;
                            }
#endif

#if HAVE_CH_LAYOUT
                            if(downmix)
                            {
                                av_channel_layout_default(&out_channel_layout, 2);
                                out_channels = 2;
                            }

                            av_opt_set_chlayout(swr, "in_chlayout", &c->ch_layout,	0);
                            av_opt_set_chlayout(swr, "out_chlayout", &out_channel_layout,	0);
#else
                            if(downmix)
                            {
                                out_channel_layout = AV_CH_LAYOUT_STEREO_DOWNMIX;
                                out_channels = 2;
                            }

                            av_opt_set_int(swr, "in_channel_layout",    c->channel_layout,	0);
                            av_opt_set_int(swr, "out_channel_layout",   out_channel_layout,	0);
#endif
                            av_opt_set_int(swr, "in_sample_rate",		c->sample_rate,		0);
                            av_opt_set_int(swr, "out_sample_rate",		out_sample_rate,	0);
                            av_opt_set_int(swr, "in_sample_fmt",		c->sample_fmt,		0);
                            av_opt_set_int(swr, "out_sample_fmt",		AV_SAMPLE_FMT_S16,	0);
        

                            e = swr_init(swr);
                            if (e < 0) 
                            {
#if HAVE_CH_LAYOUT
                                char icl[128];
                                char ocl[128];
                                av_channel_layout_describe(&c->ch_layout, icl, sizeof(icl-1));
                                av_channel_layout_describe(&out_channel_layout, ocl, sizeof(ocl-1));
                                ffmpeg_err("swr_init: %d (icl=%s ocl=%s isr=%d osr=%d isf=%d osf=%d\n",
                                    -e, icl, ocl, c->sample_rate, out_sample_rate, c->sample_fmt, AV_SAMPLE_FMT_S16);
#else
                                ffmpeg_err("swr_init: %d (icl=%d ocl=%d isr=%d osr=%d isf=%d osf=%d\n",
                                    -e, (int32_t)c->channel_layout, (int32_t)out_channel_layout, c->sample_rate, out_sample_rate, c->sample_fmt, AV_SAMPLE_FMT_S16);
#endif
                                swr_free(&swr);
                                swr = NULL;
                            }
                        }
                        
                        uint8_t *output[8] = {NULL};
                        int32_t in_samples = decoded_frame->nb_samples;
                        int32_t out_samples = av_rescale_rnd(swr_get_delay(swr, c->sample_rate) + in_samples, out_sample_rate, c->sample_rate, AV_ROUND_UP);
                        e = av_samples_alloc(&output[0], NULL, out_channels, out_samples, AV_SAMPLE_FMT_S16, 1);
                        if (e < 0) 
                        {
                            ffmpeg_err("av_samples_alloc: %d\n", -e);
                            continue;
                        }
                        int64_t next_in_pts = av_rescale(wrapped_frame_get_best_effort_timestamp(decoded_frame),
                                         pktStream->time_base.num * (int64_t)out_sample_rate * c->sample_rate,
                                         pktStream->time_base.den);
                        int64_t next_out_pts = av_rescale(swr_next_pts(swr, next_in_pts),
                                         pktStream->time_base.den,
                                         pktStream->time_base.num * (int64_t)out_sample_rate * c->sample_rate);

                        currentAudioPts = audioTrack->pts = pts = resolveAndCachePtsOffset(
                                audioPtsOffsetCache, PTS_OFFSET_CACHE_SIZE, (void*)pktStream,
                                calcPts(pktStream, next_out_pts),
                                &lastGoodAudioPts, &audioCacheNextSlot, "AUDIO",
                                1, lastGoodVideoPts, packetCounterAtLastGoodVideoPts,
                                lastGoodVideoPtsTrusted,
                                totalPacketCounter, &packetCounterAtLastGoodAudioPts,
                                &lastGoodAudioPtsWallClock,
                                &lastGoodAudioPtsTrusted,
                                &sessionBasePts);
                        out_samples = swr_convert(swr, &output[0], out_samples, (const uint8_t **) &decoded_frame->data[0], in_samples);
                        
                        //////////////////////////////////////////////////////////////////////
                        // Update pcmExtradata according to decode parameters
#if HAVE_CH_LAYOUT
                        pcmExtradata.channels              = out_channel_layout.nb_channels;
#else
                        pcmExtradata.channels              = av_get_channel_layout_nb_channels(out_channel_layout);
#endif
                        pcmExtradata.bits_per_coded_sample = 16;
                        pcmExtradata.sample_rate           = out_sample_rate;
                        // The data described by the sample format is always in native-endian order
#ifdef WORDS_BIGENDIAN
                        pcmExtradata.codec_id       = AV_CODEC_ID_PCM_S16BE;
#else
                        pcmExtradata.codec_id       = AV_CODEC_ID_PCM_S16LE;
#endif

                        //////////////////////////////////////////////////////////////////////

                        avOut.data       = output[0];
                        avOut.len        = out_samples * sizeof(int16_t) * out_channels;

                        avOut.pts        = pts;
                        avOut.extradata  = (unsigned char *) &pcmExtradata;
                        avOut.extralen   = sizeof(pcmExtradata);
                        avOut.frameRate  = 0;
                        avOut.timeScale  = 0;
                        avOut.width      = 0;
                        avOut.height     = 0;
                        avOut.type       = "audio";

                        if (!context->playback->BackWard && Write(context->output->audio->Write, context, &avOut, pts) < 0)
                        {
                            ffmpeg_err("writing data to audio device failed\n");
                        }
                        pcm_audio_export_write((const uint8_t *) output[0], (uint32_t) avOut.len,
                                               (uint32_t) pcmExtradata.sample_rate, (uint16_t) pcmExtradata.channels);
                        av_freep(&output[0]);
                    }
                }
                else if (audioTrack->have_aacheader == 1)
                {
                    ffmpeg_printf(200, "write audio aac\n");
                    ffmpeg_printf(200, "> %hhx %hhx %hhx %hhx %x %hhx %hhx\n", packet.data[0], packet.data[1], packet.data[2], packet.data[3], packet.data[4], packet.data[5], packet.data[6]);

                    avOut.data       = packet.data;
                    avOut.len        = packet.size;
                    avOut.pts        = pts;
                    avOut.extradata  = audioTrack->aacbuf;
                    avOut.extralen   = audioTrack->aacbuflen;
                    avOut.frameRate  = 0;
                    avOut.timeScale  = 0;
                    avOut.width      = 0;
                    avOut.height     = 0;
                    avOut.type       = "audio";

                    if (!context->playback->BackWard && Write(context->output->audio->Write, context, &avOut, pts) < 0)
                    {
                        ffmpeg_err("(aac) writing data to audio device failed\n");
                    }
                }
                else if (pcmExtradata.codec_id == AV_CODEC_ID_VORBIS || pcmExtradata.codec_id == AV_CODEC_ID_OPUS ||
                         pcmExtradata.codec_id == AV_CODEC_ID_WMAV1 || pcmExtradata.codec_id == AV_CODEC_ID_WMAV2 ||
                         pcmExtradata.codec_id == AV_CODEC_ID_WMAPRO || pcmExtradata.codec_id == AV_CODEC_ID_WMALOSSLESS) {
                    avOut.data       = packet.data;
                    avOut.len        = packet.size;
                    avOut.pts        = pts;
                    avOut.extradata  = (uint8_t *) &pcmExtradata;
                    avOut.extralen   = sizeof(pcmExtradata);
                    avOut.frameRate  = 0;
                    avOut.timeScale  = 0;
                    avOut.width      = 0;
                    avOut.height     = 0;
                    avOut.type       = "audio";

                    pcmExtradata.private_data = pAudioExtradata;
                    pcmExtradata.private_size = audioExtradataSize;

                    if (!context->playback->BackWard && Write(context->output->audio->Write, context, &avOut, pts) < 0) {
                        ffmpeg_err("writing data to audio device failed\n");
                    }
                }
                else
                {
                    avOut.data       = packet.data;
                    avOut.len        = packet.size;
                    avOut.pts        = pts;
                    avOut.extradata  = pAudioExtradata;
                    avOut.extralen   = audioExtradataSize;
                    avOut.frameRate  = 0;
                    avOut.timeScale  = 0;
                    avOut.width      = 0;
                    avOut.height     = 0;
                    avOut.type       = "audio";

                    if (!context->playback->BackWard && Write(context->output->audio->Write, context, &avOut, pts) < 0)
                    {
                        ffmpeg_err("writing data to audio device failed\n");
                    }
                }
            }
            else if (subtitleTrack && (subtitleTrack->Id == pid))
            {
                int64_t duration = -1;
                int64_t pts = calcPts(subtitleTrack->stream, packet.pts);
                AVStream *stream = subtitleTrack->stream;
                
                if (packet.duration != 0)
                {
                    // duration in milliseconds
                    duration = (int64_t)av_rescale(packet.duration, (int64_t)stream->time_base.num * 1000, stream->time_base.den); 
                }
                else if(get_packet_duration(&packet) != 0 && get_packet_duration(&packet) != AV_NOPTS_VALUE )
                {
                    // duration in milliseconds
                    duration = (int64_t)av_rescale(get_packet_duration(&packet), (int64_t)stream->time_base.num * 1000, stream->time_base.den); 
                }

                if (duration > 0 || duration == -1)
                {
                    SubtitleOut_t subOut;
                    memset(&subOut, 0, sizeof(subOut));
                    subOut.trackId = pid;
                    subOut.data = (uint8_t *)packet.data;
                    subOut.len = packet.size;
                    subOut.pts = pts;
                    subOut.durationMS = duration;
                    subOut.extradata = get_codecpar(stream)->extradata;
                    subOut.extralen  = get_codecpar(stream)->extradata_size;
                    subOut.width     = get_codecpar(stream)->width;;
                    subOut.height    = get_codecpar(stream)->height;;
                    if (Write(context->output->subtitle->Write, context, &subOut, pts) < 0)
                    {
                        ffmpeg_err("writing data to teletext fifo failed\n");
                    }
                }
            }
            else
            {
                /* Weder Video- noch Audio- noch Untertitel-Branch hat das Paket
                 * genommen. Fuer echte Timed-Metadata/ID3-Pakete ist das normal
                 * und wird nicht geloggt. Wenn das Paket laut Demuxer aber
                 * TATSAECHLICH Video oder Audio ist, ist das ein Problem: entweder
                 * der Typ-Guard von heute hat es bei einer Id-Kollision abgelehnt
                 * (frueher waere es fehlgeroutet worden), oder videoTrack->Id/
                 * audioTrack->Id wurden nach einem Stream-Wechsel nie aufgefrischt.
                 * Beides kann dazu fuehren, dass ein Track fuer den Rest der
                 * Session gar nicht mehr beschrieben wird (Standbild). */
                int32_t realType = get_codecpar(avContextTab[cAVIdx]->streams[packet.stream_index])->codec_type;
                if (g_verbose_logging && (realType == AVMEDIA_TYPE_VIDEO || realType == AVMEDIA_TYPE_AUDIO))
                {
                    FILE *dlog = fopen("/tmp/exteplayer3_dropped_packet.log", "a");
                    if (dlog)
                    {
                        fprintf(dlog, "%ld DROPPED pid=%d videoTrackId=%d audioTrackId=%d realCodecType=%d pktSize=%d\n",
                                (long)time(NULL), pid,
                                videoTrack ? videoTrack->Id : -1,
                                audioTrack ? audioTrack->Id : -1,
                                realType, packet.size);
                        fclose(dlog);
                    }
                }
            }
        }
        else
        {
            if( 0 != ffmpegStatus )
            {
                static char errbuf[256];

                if (ffmpegStatus == AVERROR_EOF)
                {
                    isEOFReached[cAVIdx] = 1;
                }

                if( 0 == av_strerror(ffmpegStatus, errbuf, sizeof(errbuf)) )
                {
                    /* In this way we inform user about error within the core
                     */
                    printf("{\"log\":\"Frame read error: '%s'\"}\n", errbuf);
                }
                
                /*
                if( ffmpegStatus == AVERROR(EAGAIN) )
                {
                    continue;
                }
                */
                ffmpegStatus = 0;
            }
            
            if(!is_finish_timeout(cAVIdx) && !context->playback->isTSLiveMode)
            {
                isWaitingForFinish = 1;
                update_finish_timeout(cAVIdx);
                releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                usleep(100000);
                continue;
            }
            else
            {
                uint8_t bEndProcess = 1;
                if (context->playback->isTSLiveMode)
                {
                    seek_target_bytes = 0;
                    do_seek_target_bytes = true;
                    bEndProcess = 0;
                }
                else if( 1 == context->playback->isLoopMode )
                {
                    int64_t tmpLength = 0;
                    if( 0 == container_ffmpeg_get_length(context, &tmpLength) && tmpLength > 0 && get_play_pts() > 0)
                    {
                        seek_target_seconds = 0;
                        do_seek_target_seconds = 1;

                        bEndProcess = 0;
                        context->output->Command(context, OUTPUT_CLEAR, NULL);
                        context->output->Command(context, OUTPUT_PLAY, NULL);
                        printf("{\"log\":\"Loop mode: jump to the start.\"}\n");
                    }
                }
                
                // av_read_frame failed
                ffmpeg_err("no data ->end of file reached ? \n");
                wrapped_packet_unref(&packet);
                releaseMutex(__FILE__, __FUNCTION__,__LINE__);
                if( bEndProcess )
                {
                    break; // while
                }
                else
                {
                    continue;
                }
            }
        }
        wrapped_packet_unref(&packet);
        releaseMutex(__FILE__, __FUNCTION__,__LINE__);
    } /* while */

    if (swr)
    {
        swr_free(&swr);
    }
    
    if (decoded_frame)
    {
        wrapped_frame_free(&decoded_frame);
    }
    
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56, 34, 100)
    mpeg4p2_context_close(mpeg4p2_context);
#endif

    hasPlayThreadStarted = 0;
    context->playback->isPlaying = 0;
    PlaybackDieNow(1);
    ffmpeg_printf(10, "terminating\n");
}

/* **************************** */
/* Container part for ffmpeg    */
/* **************************** */

static int32_t terminating = 0;

static int32_t interrupt_cb(void *ctx)
{
    PlaybackHandler_t *p = (PlaybackHandler_t *)ctx;
    return p->abortRequested || PlaybackDieNow(0);
}

#ifdef USE_CUSTOM_IO
typedef struct CustomIOCtx_t 
{ 
    FILE *pFile;
    FILE *pMoovFile;
    int64_t iOffset;
    
    char *szFile;
    uint64_t iFileSize;
    char *szMoovAtomFile;
    uint64_t iMoovAtomOffset;
} CustomIOCtx_t;

CustomIOCtx_t* custom_io_tab[IPTV_AV_CONTEXT_MAX_NUM] = {NULL, NULL};

int SAM_ReadFunc(void *ptr, uint8_t *buffer, int lSize)
{
    CustomIOCtx_t *io = (CustomIOCtx_t *)ptr;
    int ret = 0;
    
    if (!io->pMoovFile)
    {
        ret = (int)fread( (void *) buffer, (size_t) 1, (size_t) lSize, io->pFile );
    }
    else
    {
        if (io->iOffset < io->iMoovAtomOffset)
        {
            ret = (int)fread( (void *) buffer, (size_t) 1, (size_t) lSize, io->pFile );
            buffer += ret;
            lSize -= ret;
        }
        
        if (io->iOffset + ret >= io->iMoovAtomOffset)
        {
            if (ret)
            {
                if (fseeko(io->pMoovFile, io->iOffset + ret - io->iMoovAtomOffset, SEEK_SET))
                {
                    // something goes wrong
                    ffmpeg_err("fseeko on moov atom file fail \n");
                    lSize = 0;
                }
            }
            
            ret += (int)fread( (void *) buffer, (size_t) 1, (size_t) lSize, io->pMoovFile );
        }
        
        io->iOffset += ret;
    }
    return ret;
}

// whence: SEEK_SET, SEEK_CUR, SEEK_END (like fseek) and AVSEEK_SIZE
int64_t SAM_SeekFunc(void *ptr, int64_t pos, int whence)
{   
    CustomIOCtx_t *io = (CustomIOCtx_t *)ptr;
    int64_t ret = -1;
    if (!io->pMoovFile)
    {
        if( AVSEEK_SIZE != whence )
        {
            ret = (int64_t)fseeko(io->pFile, (off_t)pos, whence);
            if(0 == ret)
            {
                ret = (int64_t)ftello(io->pFile);
            }
        }
    }
    else
    {
        switch(whence)
        {
            case SEEK_SET:
                ret = pos;
                break;
            case SEEK_CUR:
                ret += pos;
                break;
            case SEEK_END:
                ret = io->iFileSize + pos;
                break;
            case AVSEEK_SIZE:
                return io->iFileSize;
            default:
                return -1;
        }
        
        if (ret >= 0 && ret <= io->iFileSize)
        {
            if (ret < io->iMoovAtomOffset)
            {
                if(!fseeko(io->pFile, (off_t)ret, SEEK_SET))
                    io->iOffset = ret;
                else
                    ret = -1;
            }
            else
            {
                if(!fseeko(io->pMoovFile, (off_t)(ret - io->iMoovAtomOffset), SEEK_SET))
                    io->iOffset = ret;
                else
                    ret = -1;
            }
        }
        else
        {
            ret = -1;
        }
    }
    return ret;
}

AVIOContext* container_ffmpeg_get_avio_context(CustomIOCtx_t *custom_io, size_t avio_ctx_buffer_size)
{
    if(strstr(custom_io->szFile, "file://") == custom_io->szFile)
        custom_io->szFile += 7;
    
    custom_io->pFile = fopen(custom_io->szFile, "rb");
    if(NULL == custom_io->pFile)
    {
        return NULL;
    }
    
    if (custom_io->szMoovAtomFile && custom_io->szMoovAtomFile[0] != '\0')
    {
        if(strstr(custom_io->szMoovAtomFile, "file://") == custom_io->szMoovAtomFile)
            custom_io->szMoovAtomFile += 7;
        
        custom_io->pMoovFile = fopen(custom_io->szMoovAtomFile, "rb");
        if(NULL == custom_io->pMoovFile)
        {
            fclose(custom_io->pFile);
            return NULL;
        }
    }
    
    AVIOContext *avio_ctx = NULL;
    uint8_t *avio_ctx_buffer = NULL;
    
    avio_ctx_buffer = av_malloc(avio_ctx_buffer_size);
    if (!avio_ctx_buffer) 
    {
        return NULL;
    }
    avio_ctx = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size, 0, custom_io, &SAM_ReadFunc, NULL, &SAM_SeekFunc);
    if (!avio_ctx) 
    {
        return NULL;
    }
    return avio_ctx;
}
#endif

int32_t container_ffmpeg_init_av_context(Context_t *context, char *filename, uint64_t fileSize, char *moovAtomFile, uint64_t moovAtomOffset, int32_t AVIdx)
{
    int32_t err = 0;
    AVInputFormat *fmt = NULL;
    if (AVIdx >= 0 && AVIdx < 2)
    {
        isEOFReached[AVIdx] = 0; // Reset EOF flag on start
    }
    avContextTab[AVIdx] = avformat_alloc_context();
    avContextTab[AVIdx]->interrupt_callback.callback = interrupt_cb;
    avContextTab[AVIdx]->interrupt_callback.opaque = context->playback;

#ifdef USE_CUSTOM_IO
    if(0 == strstr(filename, "://") || 
       0 == strncmp(filename, "file://", 7))
    {
        AVIOContext *avio_ctx = NULL;
        custom_io_tab[AVIdx] = malloc(sizeof(CustomIOCtx_t));
        sizeof(custom_io_tab[AVIdx], 0x00, sizeof(CustomIOCtx_t));
        
        custom_io_tab[AVIdx]->szFile = filename;
        custom_io_tab[AVIdx]->iFileSize = fileSize;
        custom_io_tab[AVIdx]->szMoovAtomFile = moovAtomFile;
        custom_io_tab[AVIdx]->iMoovAtomOffset = moovAtomOffset;
        
        avio_ctx = container_ffmpeg_get_avio_context(custom_io_tab[AVIdx], 4096);
        if(avio_ctx)
        {
            avContextTab[AVIdx]->pb = avio_ctx;
            use_custom_io[AVIdx] = 1;
        }
        else
        {
            free(custom_io_tab[AVIdx]);
            custom_io_tab[AVIdx] = NULL;
            return cERR_CONTAINER_FFMPEG_OPEN;
        }
    }
#endif

    AVDictionary *avio_opts = NULL;
    AVDictionary **pavio_opts = NULL;
    av_dict_copy(&avio_opts, g_avio_opts, 0);

    eRTMPProtoImplType rtmpProtoImplType = RTMP_NONE;
    uint8_t numOfRTMPImpl = 0;
    if (0 == strncmp(filename, "ffrtmp", 6))
    {
        filename = filename + 2;
        rtmpProtoImplType = RTMP_NATIVE;
    }
    
    if (1 == rtmp_proto_impl)
    {
        rtmpProtoImplType = RTMP_NATIVE;
    } 
    else if (2 == rtmp_proto_impl)
    {
        rtmpProtoImplType = RTMP_LIBRTMP;
    }

    if (0 == strncmp(filename, "rtmp://", 7) ||
        0 == strncmp(filename, "rtmpe://", 8) ||
        0 == strncmp(filename, "rtmps://", 8) ||
        0 == strncmp(filename, "rtmpt://", 8) ||
        0 == strncmp(filename, "rtmpte://", 9) ||
        0 == strncmp(filename, "rtmpts://", 9))
    {
        /* At first we need to check which protocol 
         * implementations we have
         */
        void *opaque = NULL;
        const char *protoName = NULL;
        uint8_t haveNativeProto = 0;
        
        while (protoName = avio_enum_protocols(&opaque, 1))
        {
            if (0 == strcmp("rtmp", protoName))
            {
                ++numOfRTMPImpl;
            }
            else if (0 == strcmp("ffrtmp", protoName))
            {
                // ffmpeg has patch to have both native and librtmp implementations
                ++numOfRTMPImpl;
                haveNativeProto = 2;
            }
            else if (0 == strncmp("rtmpts", protoName, 6))
            {
                // rtmpts is only available in native implementation
                // rtmpts is listed after rtmp
                haveNativeProto = 1;
            }
        }
        
        if (haveNativeProto > 0)
        {
            if (numOfRTMPImpl > 1) // we have both
            {
                if (rtmpProtoImplType == RTMP_NONE)
                {
                    /* if we have both impl, we will prefer native
                     * unless uri contain param wich can be understandable
                     * only by librtmp
                     */
                    if (strstr(filename, " token=") || 
                        strstr(filename, " jtv="))
                    {
                        rtmpProtoImplType = RTMP_LIBRTMP;
                    }
                    else
                    {
                        rtmpProtoImplType = RTMP_NATIVE;
                    }
                }
            }
            else
            {
                rtmpProtoImplType = RTMP_NATIVE;
            }
        }
        else
        {
            rtmpProtoImplType = RTMP_LIBRTMP;
        }
        
        if (RTMP_NATIVE == rtmpProtoImplType)
        {
            char *baseUri = strdup(filename);
            char *token  = NULL;
            
            // check if uri have additional params
            if ((token = strtok(baseUri, " ")) != NULL )
            {
                char *conn = malloc(strlen(filename));
                char *swfUrl = malloc(strlen(filename));
                char *swfVfy = malloc(strlen(filename));
                char *poseq, *key, *value;
                conn[0] = '\0';
                swfUrl[0] = '\0';
                swfVfy[0] = '\0';
                token = NULL;
                while((token = strtok(token, " ")) != NULL)
                {
                    if ((poseq = strchr(token, '=')) != NULL)
                    {
                        *poseq = '\0';
                        key = token;
                        value = poseq + 1;
                        ffmpeg_printf(20, "rtmp_key = \"%s\", rtmp_value = \"%s\"\n", key, value);
                        /* translate librtmp connection parameters to ffmpeg ones routin provided by @mx3L
                         *
                         * librtmp parameters     - https://rtmpdump.mplayerhq.hu/librtmp.3.html
                         * ffmpeg rtmp parameters - https://ffmpeg.org/ffmpeg-protocols.html#rtmp
                         */
                        if (!strcasecmp(key, "app"))
                        {
                            av_dict_set(&avio_opts, "rtmp_app", value, 0);
                        }
                        else if (!strcasecmp(key, "conn"))
                        {
                            if (conn[0] != '\0')
                            {
                                strcat(conn, " ");
                            }
                            strcat(conn, value);
                        }
                        else if (!strcasecmp(key, "buffer"))
                        {
                            av_dict_set(&avio_opts, "rtmp_buffer", value, 0);
                        }
                        else if (!strcasecmp(key, "flashVer"))
                        {
                            av_dict_set(&avio_opts, "rtmp_flashver", value, 0);
                        }
                        else if (!strcasecmp(key, "live"))
                        {
                            av_dict_set(&avio_opts, "rtmp_live", value, 0);
                        }
                        else if (!strcasecmp(key, "pageUrl"))
                        {
                            av_dict_set(&avio_opts, "rtmp_pageurl", value, 0);
                        }
                        else if (!strcasecmp(key, "playpath"))
                        {
                            av_dict_set(&avio_opts, "rtmp_playpath", value, 0);
                        }
                        else if (!strcasecmp(key, "subscribe"))
                        {
                            av_dict_set(&avio_opts, "rtmp_subscribe", value, 0);
                        }
                        else if (!strcasecmp(key, "swfUrl"))
                        {
                            strcpy(swfUrl, value);
                        }
                        // ffmpeg expects this value to contain url to player swf
                        // not a 1|0|TRUE like librtmp
                        else if (!strcasecmp(key, "swfVfy"))
                        {
                            strcpy(swfVfy, value);
                        }
                        else if (!strcasecmp(key, "tcUrl"))
                        {
                            av_dict_set(&avio_opts, "rtmp_tcurl", value, 0);
                        }
                        // timeout is ment for incoming connections
                        else if (!strcasecmp(key, "timeout"))
                        {
                        }
                        else
                        {
                            // threat as direct options
                            // for example rtmp_swfhash, rtmp_swfsize
                            av_dict_set(&avio_opts, key, value, 0);
                        }
                    }
                    token = NULL;
                }
                
                if (conn[0] != '\0')
                {
                    av_dict_set(&avio_opts, "rtmp_conn", conn, 0);
                }
                free(conn);

                if (swfUrl[0] != '\0')
                {
                    if (swfVfy[0] == '1' || !strncasecmp(swfVfy, "true", 4))
                    {
                        av_dict_set(&avio_opts, "rtmp_swfverify", swfUrl, 0);
                    }
                    else
                    {
                        av_dict_set(&avio_opts, "rtmp_swfurl", swfUrl, 0);
                    }
                }
                free(swfUrl);
                free(swfVfy);
            }
            
            if (2 == haveNativeProto)
            {
                filename = malloc(strlen(baseUri) + 2 + 1);
                strncpy(filename, "ff", 2);
                strcpy(filename+2, baseUri);
                free(baseUri);
                // memory leak, only once, so does not matter
            }
            else
            {
                filename = baseUri;
                // memory leak, only once, so does not matter
            }
        }
    }
    else if(0 == strncmp(filename, "http://", 7) || 
            0 == strncmp(filename, "https://", 8))
    {
    	char num[16];

    	sprintf( num, "%u000", context->playback->httpTimeout );
        av_dict_set(&avio_opts, "timeout", num, 0); // default is 10s
        /* "timeout" (oben) wirkt nur auf die initial per avformat_open_input()
         * geoeffnete Verbindung -- ffio_copy_url_options() (aviobuf.c) kopiert
         * fuer alle intern von hls.c nachgeladenen Segmente/Sub-Playlists
         * explizit "rw_timeout" (nicht "timeout"), das bisher nie gesetzt
         * wurde und defaultmaessig 0 (=deaktiviert) ist. Dadurch griff bei
         * einem haengenden Read innerhalb eines HLS-Segment-/Playlist-Abrufs
         * kein Timeout, egal was bei "timeout" stand. */
        av_dict_set(&avio_opts, "rw_timeout", num, 0);
        av_dict_set(&avio_opts, "reconnect", "1", 0);
        /* Manche HLS-Origins (z.B. ARD) liefern deutlich kleinere Playlists,
         * wenn Kompression angeboten wird -- FFmpeg kann gzip/deflate zwar
         * dekodieren (zlib ist eingebunden), fordert es aber nie selbst an.
         * Bei sehr grossen Live-Playlists kann das den Download unter
         * Netzwerklast von mehreren Sekunden auf Bruchteile davon senken. */
        av_dict_set(&avio_opts, "headers", "Accept-Encoding: gzip\r\n", AV_DICT_APPEND);

        if (context->playback->isTSLiveMode) // special mode for live TS stream with skip packet
        {
            av_dict_set(&avio_opts, "seekable", "0", 0);
            av_dict_set(&avio_opts, "reconnect_at_eof", "1", 0);
            av_dict_set(&avio_opts, "reconnect_streamed", "1", 0);
        }

        if( strncmp(filename, "http://127.0.0.1", 16) == 0 )
        {
            /* when using with ArchivCZSK, then indicate DRM support */
            av_dict_set(&avio_opts, "headers", "X-DRM-Api-Level: 1\r\n", AV_DICT_APPEND);
        }
    }

    if (g_hls_quality_mode != 0) {
        char num[16];
        sprintf(num, "%d", g_hls_quality_mode);
        av_dict_set(&avio_opts, "hls_quality_mode", num, 0);
    }
    if (g_hls_audio_default_only) {
        av_dict_set(&avio_opts, "hls_audio_default_only", "1", 0);
    }

    pavio_opts = &avio_opts;
    
    if ((err = avformat_open_input(&avContextTab[AVIdx], filename, fmt, pavio_opts)) != 0)
    {
        if (rtmp_proto_impl == 0 && //err == AVERROR_UNKNOWN && 
            rtmpProtoImplType == RTMP_NATIVE &&
            numOfRTMPImpl > 1)
        {
            // retry with librtmp
            err = avformat_open_input(&avContextTab[AVIdx], filename+2, fmt, pavio_opts);
            // filename2 - another memory leak, and also only once, so does not matter 
        }
        
        if (err != 0)
        {
            char error[512];

            ffmpeg_err("avformat_open_input failed %d (%s)\n", err, filename);
            av_strerror(err, error, 512);
            E2iSendMsg("{\"FF_ERROR\":{\"msg\":\"%s\",\"code\":%i}}\n", error, err);

            if(avio_opts != NULL)
            {
                av_dict_free(&avio_opts);
            }
            releaseMutex(__FILE__, __FUNCTION__,__LINE__);
            return cERR_CONTAINER_FFMPEG_OPEN;
        }
    }

    //ffmpeg5: error: assignment of member 'flags' in read-only object -> so commented out next line
    //avContextTab[AVIdx]->iformat->flags |= AVFMT_SEEK_TO_PTS;
    /* AVFMT_FLAG_DISCARD_CORRUPT: Live-Mitschnitt mit -L AV_DEBUG_LEVEL:24
     * zeigte bei Werbe-Uebergaengen mancher HLS-Live-Streams wiederholten
     * TS-Sync-Verlust (mpegts.c wechselt wild zwischen 188/192/204 Byte
     * Paketgroesse), der zu "Packet corrupt"- und "Invalid timestamps"-
     * Meldungen fuehrt - ff_parse_pes_pts() validiert keine Marker-Bits und
     * interpretiert bei falschem Byte-Offset Zufallsbytes als PTS/DTS nahe
     * 2^33 (deckt sich mit dem separat gefundenen ~8,58-Mrd.-Sprung, siehe
     * resolveAndCachePtsOffset()). Dieses Flag laesst FFmpeg als korrupt
     * erkannte Pakete selbst verwerfen, bevor sie die eigene PTS-Logik
     * erreichen. Wird laut hls.c:2367 (pls->ctx->flags |= s->flags &
     * ~AVFMT_FLAG_CUSTOM_IO) zuverlaessig an die inneren HLS-Sub-Kontexte
     * weitergereicht. */
    avContextTab[AVIdx]->flags = AVFMT_FLAG_GENPTS | AVFMT_FLAG_DISCARD_CORRUPT;

    /* Preselection is now handled natively in FFmpeg's hls.c using dictionary options */

    if (context->playback->noprobe)
    {
        wrapped_set_max_analyze_duration(avContextTab[AVIdx], 1);
    }

    ffmpeg_printf(20, "find_streaminfo\n");

    if (avformat_find_stream_info(avContextTab[AVIdx], NULL) < 0) 
    {
        ffmpeg_err("Error avformat_find_stream_info\n");
    }

//for buffered io
    if(avContextTab[AVIdx] != NULL && avContextTab[AVIdx]->pb != NULL && !context->playback->isTSLiveMode)
    {
        ffmpeg_real_read_org = avContextTab[AVIdx]->pb->read_packet;
        
        if(0 ==AVIdx && strstr(filename, "://") != 0 && 
           strncmp(filename, "file://", 7) != 0)
        {
            if(ffmpeg_buf_size > 0 && ffmpeg_buf_size > FILLBUFDIFF + FILLBUFPAKET)
            {
                if(avContextTab[AVIdx] != NULL && avContextTab[AVIdx]->pb != NULL)
                {
                    ffmpeg_buf = av_malloc(ffmpeg_buf_size);

                    if(ffmpeg_buf != NULL)
                    {
                        ffmpeg_printf(10, "buffer size=%d\n", ffmpeg_buf_size);
                        
                        ffmpeg_read_org = avContextTab[AVIdx]->pb->read_packet;
                        avContextTab[AVIdx]->pb->read_packet = ffmpeg_read;
                        ffmpeg_seek_org = avContextTab[AVIdx]->pb->seek;
                        avContextTab[AVIdx]->pb->seek = ffmpeg_seek;
                        ffmpeg_buf_read = ffmpeg_buf;
                        ffmpeg_buf_write = ffmpeg_buf;

                        //fill buffer
                        ffmpeg_filler(context, -1, NULL, 0);
                        ffmpeg_start_fillerTHREAD(context);
                    }
                }
            }
        }
        else if (progressive_playback)
        {
            avContextTab[AVIdx]->pb->read_packet = ffmpeg_read_wrapper;
        }
    }
//for buffered io (end)
    
    return 0;
}

int32_t container_ffmpeg_init(Context_t *context, PlayFiles_t *playFilesNames)
{
    int32_t err = 0;

    ffmpeg_printf(10, ">\n");

    if (playFilesNames == NULL) 
    {
        ffmpeg_err("playFilesNames NULL\n");
        return cERR_CONTAINER_FFMPEG_NULL;
    }
    
    if (playFilesNames->szFirstFile == NULL) 
    {
        ffmpeg_err("playFilesNames->szFirstFile NULL\n");
        return cERR_CONTAINER_FFMPEG_NULL;
    }

    if (context == NULL) 
    {
        ffmpeg_err("context NULL\n");
        return cERR_CONTAINER_FFMPEG_NULL;
    }

    ffmpeg_printf(10, "filename %s\n", playFilesNames->szFirstFile);
    if(playFilesNames->szSecondFile)
    {
        ffmpeg_printf(10, "second filename %s\n", playFilesNames->szSecondFile);
    }

    if (isContainerRunning) 
    {
        ffmpeg_err("ups already running?\n");
        releaseMutex(__FILE__, __FUNCTION__,__LINE__);
        return cERR_CONTAINER_FFMPEG_RUNNING;
    }

    /* initialize ffmpeg */
    wrapped_register_all();
    avformat_network_init();

    if(AV_DEBUG_LEVEL > 0)
    {
        av_log_set_level( AV_DEBUG_LEVEL );
    }
    else
    {
        av_log_set_callback( ffmpeg_silen_callback );
    }

    context->playback->abortRequested = 0;
    int32_t res = container_ffmpeg_init_av_context(context, playFilesNames->szFirstFile, playFilesNames->iFirstFileSize, \
                                                   playFilesNames->szFirstMoovAtomFile, playFilesNames->iFirstMoovAtomOffset, 0);
    if(0 != res)
    {
        return res;
    }

    if(playFilesNames->szSecondFile && playFilesNames->szSecondFile[0] != '\0')
    {
        res = container_ffmpeg_init_av_context(context, playFilesNames->szSecondFile, playFilesNames->iSecondFileSize, \
                                               playFilesNames->szSecondMoovAtomFile, playFilesNames->iSecondMoovAtomOffset, 1);
    }
    
    if(0 != res)
    {
        return res;
    }
    
    terminating = 0;
    latestPts = 0;
    isContainerRunning = 1;
    res = container_ffmpeg_update_tracks(context, playFilesNames->szFirstFile, 1);
    return res;
}

int32_t container_ffmpeg_update_tracks(Context_t *context, char *filename, int32_t initial)
{
    Track_t *currAudioTrack = NULL;
    Track_t *currSubtitleTrack = NULL;
    uint32_t addedVideoTracksCount = 0;
    
    if (terminating)
    {
        return cERR_CONTAINER_FFMPEG_NO_ERROR;
    }
    
    getMutex_wr(__FILE__, __FUNCTION__,__LINE__);
    
    if (initial && context->manager->subtitle)
    {
        context->manager->subtitle->Command(context, MANAGER_GET_TRACK, &currSubtitleTrack);
    }

    if (context->manager->audio)
    {
        context->manager->audio->Command(context, MANAGER_GET_TRACK, &currAudioTrack);
    }

    if (context->manager->video)
    {
        context->manager->video->Command(context, MANAGER_INIT_UPDATE, NULL);
    }
    
    if (context->manager->audio)
    {
        context->manager->audio->Command(context, MANAGER_INIT_UPDATE, NULL);
    }
    
#if 0
    if (context->manager->subtitle)
    {
        context->manager->subtitle->Command(context, MANAGER_INIT_UPDATE, NULL);
    }
#endif

    ffmpeg_printf(20, "dump format\n");
    av_dump_format(avContextTab[0], 0, filename, 0);
    
    
    uint32_t cAVIdx = 0;
    for(cAVIdx=0; cAVIdx<IPTV_AV_CONTEXT_MAX_NUM; cAVIdx+=1)
    {
        if(NULL == avContextTab[cAVIdx])
        {
            break;
        }
        AVFormatContext *avContext = avContextTab[cAVIdx];
        uint32_t *stream_index = NULL;
        uint32_t nb_stream_indexes = 0;
        
        ffmpeg_printf(1, "cAVIdx[%d]: number of streams: %d\n", cAVIdx, avContext->nb_streams);
        
        if (avContext->nb_programs > 0)
        {
            uint32_t n = 0;
            ffmpeg_printf(1, "cAVIdx[%d]: stream with multi programs: num of programs %d\n", cAVIdx, avContext->nb_programs);

            if (g_sel_program_id <= 0)
            {
                /* Pick the variant with the lowest/highest advertised HLS
                 * bandwidth instead of just the first one with a video stream.
                 * Mode 0 (auto, no explicit -Q from the caller) is treated the
                 * same as mode 2 (highest) below: a caller that never asked for
                 * a specific quality almost certainly did not intend "whatever
                 * happens to be listed first in the playlist" either - that was
                 * only ever an implementation accident, not a deliberate choice,
                 * and it silently picked the lowest quality for every caller
                 * that invokes exteplayer3 directly without going through
                 * serviceapp's own hls_quality_mode setting. */
                AVProgram *best = NULL;
                int64_t best_bandwidth = -1;

                for (n = 0; n < avContext->nb_programs; n++)
                {
                    AVProgram *p = avContext->programs[n];
                    uint32_t m = 0;
                    int hasVideo = 0;
                    AVDictionaryEntry *bw_entry;
                    int64_t bandwidth;

                    if (!p->nb_stream_indexes)
                        continue;

                    for (m = 0; m < p->nb_stream_indexes; m++)
                    {
                        AVStream *s = avContext->streams[p->stream_index[m]];
                        if (get_codecpar(s)->codec_type == AVMEDIA_TYPE_VIDEO && get_codecpar(s)->width > 0)
                        {
                            hasVideo = 1;
                            break;
                        }
                    }
                    if (!hasVideo)
                        continue;

                    bw_entry = av_dict_get(p->metadata, "variant_bitrate", NULL, 0);
                    bandwidth = bw_entry ? strtoll(bw_entry->value, NULL, 10) : 0;

                    ffmpeg_printf(1, "cAVIdx[%d]: PROGRAM ID: %d, bandwidth [%"PRId64"]\n", cAVIdx, (int32_t)p->id, bandwidth);

                    if (best == NULL ||
                        (g_hls_quality_mode != 1 && bandwidth > best_bandwidth) ||
                        (g_hls_quality_mode == 1 && bandwidth < best_bandwidth))
                    {
                        best = p;
                        best_bandwidth = bandwidth;
                    }
                }

                if (best != NULL)
                {
                    stream_index = best->stream_index;
                    nb_stream_indexes = best->nb_stream_indexes;
                    ffmpeg_printf(1, "cAVIdx[%d]: select PROGRAM ID: %d (hls_quality_mode=%d, bandwidth=%"PRId64")\n",
                                  cAVIdx, (int32_t)best->id, g_hls_quality_mode, best_bandwidth);
                }
            }
            else
            {
                /* g_sel_program_id > 0 here (the smart bandwidth-based branch
                 * above handles every g_sel_program_id <= 0 case now, auto
                 * included) - an explicit caller-selected program always wins. */
                for (n = 0; n < avContext->nb_programs && (0 == nb_stream_indexes || stream_index == NULL); n++)
                {
                    AVProgram *p = avContext->programs[n];
                    if (p->nb_stream_indexes && g_sel_program_id == p->id)
                    {
                        stream_index = p->stream_index;
                        nb_stream_indexes = p->nb_stream_indexes;
                        ffmpeg_printf(1, "cAVIdx[%d]: select PROGRAM ID: %d\n", cAVIdx, (int32_t)p->id);
                        break;
                    }
                }
            }
        }

        /* DASH represents its adaptive bitrate variants as multiple VIDEO
         * streams within a single program (unlike HLS's per-variant
         * AVProgram grouping handled above), so this needs its own pass
         * over avContext->nb_streams rather than nb_programs. Reuses the
         * same g_hls_quality_mode setting/-Q flag as HLS above - one
         * quality preference covers every adaptive format, see plugin.py's
         * "HLS/DASH start quality" setup label. Mode 0 (auto, e.g. a
         * caller like archivCZSK/e2iplayer that never passes -Q) is
         * treated the same as mode 2 (highest), matching the HLS behavior.
         *
         * Hardware-decoder protection: this box's H.264 decoder is capped
         * at 1080p60 (Level 4.2) - 4K is only decodable via HEVC. A DASH
         * manifest offering a UHD H.264 representation must never be
         * selected, even under "highest quality", or the recording/
         * playback ends up with a valid but undecodable video track
         * (black screen, audio still plays). HEVC representations are not
         * resolution-capped here since UHD HEVC decode does work.
         *
         * Only the index is determined here - the actual discard flag is
         * set together with every other stream a few dozen lines below
         * (the per-stream loop starting at "int32_t n = 0;"), since that
         * loop's own "first video track wins" logic would otherwise
         * unconditionally overwrite whatever we set here. */
        int isDashCtx = (avContext->iformat && !strcmp(avContext->iformat->name, "dash"));
        int32_t dashBestVideoIdx = -1;
        if (isDashCtx)
        {
            int32_t n2;
            int64_t best_bandwidth = -1;
            int64_t best_res = -1;

            for (n2 = 0; n2 < avContext->nb_streams; n2++)
            {
                AVStream *st = avContext->streams[n2];
                AVCodecParameters *par = get_codecpar(st);
                AVDictionaryEntry *bw_entry;
                int64_t bandwidth;
                int64_t res;

                if (par->codec_type != AVMEDIA_TYPE_VIDEO || par->width <= 0)
                    continue;

                if (par->codec_id == AV_CODEC_ID_H264 && (par->width > 1920 || par->height > 1080))
                {
                    ffmpeg_printf(1, "cAVIdx[%d]: DASH stream %d (%dx%d H.264) exceeds hardware decoder limits, skipping\n",
                                  cAVIdx, n2, par->width, par->height);
                    continue;
                }

                bw_entry = av_dict_get(st->metadata, "variant_bitrate", NULL, 0);
                bandwidth = bw_entry ? strtoll(bw_entry->value, NULL, 10) : 0;
                res = (int64_t)par->width * par->height;

                if (dashBestVideoIdx < 0 ||
                    (g_hls_quality_mode != 1 && (bandwidth > best_bandwidth || (bandwidth == best_bandwidth && res > best_res))) ||
                    (g_hls_quality_mode == 1 && (bandwidth < best_bandwidth || (bandwidth == best_bandwidth && res < best_res))))
                {
                    dashBestVideoIdx = n2;
                    best_bandwidth = bandwidth;
                    best_res = res;
                }
            }

            if (dashBestVideoIdx >= 0)
            {
                ffmpeg_printf(1, "cAVIdx[%d]: DASH select video stream %d (hls_quality_mode=%d, bandwidth=%"PRId64", res=%"PRId64")\n",
                              cAVIdx, dashBestVideoIdx, g_hls_quality_mode, best_bandwidth, best_res);
            }
        }

        int hlsHasDefaultAudio = 0;
        if (g_hls_audio_default_only && avContext->iformat && !strcmp(avContext->iformat->name, "hls"))
        {
            int32_t chk;
            for (chk = 0; chk < avContext->nb_streams; chk++)
            {
                AVStream *s = avContext->streams[chk];
                if (get_codecpar(s)->codec_type != AVMEDIA_TYPE_AUDIO)
                    continue;
                if (nb_stream_indexes > 0 && stream_index != NULL)
                {
                    uint32_t m2 = 0;
                    int inProg = 0;
                    for (m2 = 0; m2 < nb_stream_indexes; m2++)
                    {
                        if ((uint32_t)chk == stream_index[m2]) { inProg = 1; break; }
                    }
                    if (!inProg)
                        continue;
                }
                if (s->disposition & AV_DISPOSITION_DEFAULT)
                {
                    hlsHasDefaultAudio = 1;
                    break;
                }
            }
        }

        int32_t n = 0;
        for (n = 0; n < avContext->nb_streams; n++)
        {
            Track_t track;
            AVStream *stream = avContext->streams[n];
            int32_t version = 0;
            char *encoding = NULL;

            if (nb_stream_indexes > 0 && stream_index != NULL)
            {
                uint32_t isStreamFromSelProg = 0;
                uint32_t m = 0;
                for (m = 0; m < nb_stream_indexes; m++)
                {
                    if (n == stream_index[m])
                    {
                        isStreamFromSelProg = 1;
                        break;
                    }
                }

                if (!isStreamFromSelProg) {
                    stream->discard = AVDISCARD_ALL;
                    ffmpeg_printf(1, "cAVIdx[%d]: add DISCARD flag to  stream index[%d]\n", cAVIdx, stream->index);
                    continue; // skip this stream
                }
            }

            if (hlsHasDefaultAudio &&
                get_codecpar(stream)->codec_type == AVMEDIA_TYPE_AUDIO &&
                !(stream->disposition & AV_DISPOSITION_DEFAULT))
            {
                stream->discard = AVDISCARD_ALL;
                ffmpeg_printf(1, "cAVIdx[%d]: hls_audio_default_only: discard non-default audio stream index[%d]\n", cAVIdx, stream->index);
                continue; // skip non-default HLS audio rendition
            }

            if (isDashCtx &&
                get_codecpar(stream)->codec_type == AVMEDIA_TYPE_VIDEO &&
                n != dashBestVideoIdx)
            {
                stream->discard = AVDISCARD_ALL;
                ffmpeg_printf(1, "cAVIdx[%d]: DASH: discard non-selected video stream index[%d]\n", cAVIdx, stream->index);
                continue; // skip every DASH video representation except the one chosen above
            }

            encoding = Codec2Encoding((int32_t)get_codecpar(stream)->codec_id, (int32_t)get_codecpar(stream)->codec_type, \
                                      (uint8_t *)get_codecpar(stream)->extradata, \
                                      (int)get_codecpar(stream)->extradata_size, \
                                      (int)get_codecpar(stream)->profile, &version);
            
            if(encoding != NULL && !strncmp(encoding, "A_IPCM", 6) && insert_pcm_as_lpcm)
            {
                encoding = "A_LPCM";
            }

            if (encoding != NULL)
            {
               ffmpeg_printf(1, "%d. encoding = %s - version %d\n", n, encoding, version);
            }

            if (!stream->id)
            {
                stream->id = n;
            }

            /* some values in track are unset and therefor copyTrack segfaults.
             * so set it by default to NULL!
             */
            memset(&track, 0, sizeof(track));
            track.AVIdx = cAVIdx;

            switch (get_codecpar(stream)->codec_type)
            {
            case AVMEDIA_TYPE_VIDEO:
                ffmpeg_printf(10, "CODEC_TYPE_VIDEO %d\n", get_codecpar(stream)->codec_type);
                // do not discard any stream from second context
                stream->discard = 0 == cAVIdx ? AVDISCARD_ALL : AVDISCARD_DEFAULT; /* by default we discard all video streams */
                if (encoding != NULL) 
                {
                    track.type           = eTypeES;
                    track.version        = version;

                    track.width          = get_codecpar(stream)->width;
                    track.height         = get_codecpar(stream)->height;
                    
                    /* We will return here PAR (Pixel Aspect Ratio) client need to calculate DAR(Display Aspect Ratio)
                     * example: PAR 64:45 DAR 16:9
                     *          Resolution 720x576
                     * Display aspect ratio = (720*64)/(576*45) = 16/9
                     * 0:1 is the value for invalid/unset aspect ratio -> https://trac.ffmpeg.org/ticket/3798
                     */
                    track.aspect_ratio_num = stream->sample_aspect_ratio.num;
                    track.aspect_ratio_den = stream->sample_aspect_ratio.den;
                    if (0 == track.aspect_ratio_num  || 0 == track.aspect_ratio_den)
                    {
                        track.aspect_ratio_num = get_codecpar(stream)->sample_aspect_ratio.num;
                        track.aspect_ratio_den = get_codecpar(stream)->sample_aspect_ratio.den;
                    }

                    track.extraData      = get_codecpar(stream)->extradata;
                    track.extraSize      = get_codecpar(stream)->extradata_size;

                    track.aacbuf         = 0;
                    track.have_aacheader = -1;
                    
                    AVRational rateRational = get_frame_rate(stream);
                    if (rateRational.den!=0)
                    {
                        track.frame_rate = (uint32_t)(1000 * (int64_t)(rateRational.num) / (int64_t)(rateRational.den));
                    }
                    
                    /* fixme: revise this */
                    if (track.frame_rate < 23970)
                    {
                        track.TimeScale = 1001;
                    }
                    else
                    {
                        track.TimeScale = 1000;
                    }
                    
                    ffmpeg_printf(10, "bit_rate       [%"PRId64"]\n", get_codecpar(stream)->bit_rate);
                    ffmpeg_printf(10, "time_base.den  [%d]\n", stream->time_base.den);
                    ffmpeg_printf(10, "time_base.num  [%d]\n", stream->time_base.num);
                    ffmpeg_printf(10, "width          [%d]\n", get_codecpar(stream)->width);
                    ffmpeg_printf(10, "height         [%d]\n", get_codecpar(stream)->height);
                    ffmpeg_printf(10, "frame_rate num [%d]\n", rateRational.num);
                    ffmpeg_printf(10, "frame_rate den [%d]\n", rateRational.den);

                    ffmpeg_printf(10, "frame_rate     [%u]\n", track.frame_rate);
                    ffmpeg_printf(10, "TimeScale      [%d]\n", track.TimeScale);

                    track.Name      = "und";
                    track.Encoding  = encoding;
                    track.stream    = stream;
                    track.Id        = ((AVStream *) (track.stream))->id;

                    track.duration = (int64_t)av_rescale(stream->duration, (int64_t)stream->time_base.num * 1000, stream->time_base.den);
                    if(stream->duration == AV_NOPTS_VALUE || 0 == strncmp(avContext->iformat->name, "dash", 4))
                    {
                        ffmpeg_printf(10, "Stream has no duration so we take the duration from context\n");
                        /* Ist auch avContext->duration unbekannt (echte Live-Streams ohne
                         * jede Laengenangabe, z.B. eine reine Webcam-URL), ist das selbst
                         * AV_NOPTS_VALUE (INT64_MIN). Ohne diese Absicherung wird daraus
                         * durch die Division ein riesiger, aber nicht mehr als ungueltig
                         * erkennbarer negativer Wert (in container_ffmpeg_get_length()
                         * nochmal durch 1000 geteilt), der als PLAYBACK_LENGTH an
                         * serviceapp/Enigma2 durchgereicht wird -- dort blieb dadurch
                         * die Zeitanzeige trotz laufender Wiedergabe auf 0:00 stehen. */
                        track.duration = (AV_NOPTS_VALUE == avContext->duration) ? 0 : (int64_t) avContext->duration / 1000;
                    }
                    
                    if (context->manager->video)
                    {
                        if (get_codecpar(stream)->codec_id == AV_CODEC_ID_MPEG4)
                        {
                            track.avCodecCtx = wrapped_avcodec_get_context(cAVIdx, stream);
                        }
                        ffmpeg_printf(1, "cAVIdx[%d]: MANAGER_ADD track VIDEO\n", cAVIdx);
                        if( context->manager->video->Command(context, MANAGER_ADD, &track) < 0) 
                        {
                            /* konfetti: fixme: is this a reason to return with error? */
                            ffmpeg_err("failed to add track %d\n", n);
                        }
                        else
                        {
                            if (addedVideoTracksCount == 0) /* at now we can handle only first video track */
                            {
                                stream->discard = AVDISCARD_DEFAULT;
                            }
                            addedVideoTracksCount += 1;
                        }
                    }
                }
                else 
                {
                    ffmpeg_err("codec type video but codec unknown %d\n", get_codecpar(stream)->codec_id);
                }
                break;
            case AVMEDIA_TYPE_AUDIO:
                ffmpeg_printf(10, "CODEC_TYPE_AUDIO %d\n",get_codecpar(stream)->codec_type);
                // do not discard any stream from second context
                stream->discard = 0 == cAVIdx ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
                if (encoding != NULL)
                {
                    AVDictionaryEntry *lang;
                    track.type = eTypeES;

                    lang = av_dict_get(stream->metadata, "language", NULL, 0);

                    track.Name = lang ? lang->value : "und";

                    ffmpeg_printf(10, "Language %s\n", track.Name);

                    track.Encoding       = encoding;
                    track.stream         = stream;
                    track.Id             = ((AVStream *) (track.stream))->id;
                    track.aacbuf         = 0;
                    track.have_aacheader = -1;

                    track.duration       = (int64_t)av_rescale(stream->duration, (int64_t)stream->time_base.num * 1000, stream->time_base.den);
                    if(stream->duration == AV_NOPTS_VALUE) 
                    {
                        ffmpeg_printf(10, "Stream has no duration so we take the duration from context\n");
                        track.duration = (AV_NOPTS_VALUE == avContext->duration) ? 0 : (int64_t) avContext->duration / 1000;
                    }
                    
                    if(!strncmp(encoding, "A_IPCM", 6) || !strncmp(encoding, "A_LPCM", 6))
                    {
                        track.inject_as_pcm = 1;

                        /* container_ffmpeg_update_tracks() wird nicht nur einmal beim
                         * Start aufgerufen, sondern bei JEDER GUI-Trackabfrage erneut
                         * (MANAGER_LIST -> hier mit initial=0), z.B. durch
                         * getCurrentTrack/getNumberOfTracks. ManagerAdd() kopiert das
                         * Ergebnis dann per freeTrack()+copyTrack() in die AKTIVE, vom
                         * Demux-Thread gerade benutzte Track_t-Struktur. Ein erneutes
                         * avcodec_open2() hier tauscht den Decoder-Kontext also mitten
                         * im laufenden Stream aus, ohne den Downstream-Resampler-
                         * Zustand (swr/decoded_frame in FFMPEGThread) zurueckzusetzen
                         * - das fuehrt zu Tonaussetzern/Async, voellig unabhaengig von
                         * Ad-Splices. Ist bereits ein Track mit derselben Id aktiv,
                         * dessen Decoder-Kontext wiederverwenden statt neu zu oeffnen. */
                        if (!initial && currAudioTrack && currAudioTrack->Id == track.Id && currAudioTrack->avCodecCtx)
                        {
                            track.avCodecCtx = currAudioTrack->avCodecCtx;
                        }
                        else
                        {
                            track.avCodecCtx = wrapped_avcodec_get_context(cAVIdx, stream);
                            if (track.avCodecCtx)
                            {
                                ffmpeg_printf(10, " Handle inject_as_pcm = %d\n", track.inject_as_pcm);

                                const AVCodec *codec = avcodec_find_decoder(get_codecpar(stream)->codec_id);

                                int errorCode = avcodec_open2(track.avCodecCtx, codec, NULL);
                                if(codec != NULL && !errorCode)
                                {
                                   ffmpeg_printf(10, "AVCODEC__INIT__SUCCESS\n");
                                }
                                else
                                {
                                   ffmpeg_printf(10, "AVCODEC__INIT__FAILED error[%d]\n", errorCode);
                                }

                                if (g_verbose_logging)
                                {
                                    FILE *arlog = fopen("/tmp/exteplayer3_audio_reinit.log", "a");
                                    if (arlog)
                                    {
                                        uint8_t *ed = get_codecpar(stream)->extradata;
                                        int edsize = get_codecpar(stream)->extradata_size;
                                        fprintf(arlog,
                                            "%ld AUDIO_INITIAL_OPEN codec=%s codecId=%d sampleRate=%d channels=%d "
                                            "profile=%d extradataSize=%d extradataHead=%02x%02x%02x%02x errorCode=%d initial=%d\n",
                                            (long)time(NULL),
                                            codec ? codec->name : "-",
                                            (int32_t)get_codecpar(stream)->codec_id,
                                            get_codecpar(stream)->sample_rate,
                                            get_codecpar(stream)->channels,
                                            get_codecpar(stream)->profile,
                                            edsize,
                                            edsize > 0 ? ed[0] : 0,
                                            edsize > 1 ? ed[1] : 0,
                                            edsize > 2 ? ed[2] : 0,
                                            edsize > 3 ? ed[3] : 0,
                                            errorCode, initial);
                                        fclose(arlog);
                                    }
                                }
                            }
                        }
                    }
                    else if(!strncmp(encoding, "A_PCM", 5))
                    {
                        track.inject_raw_pcm = 1;
                    }
                    else if (get_codecpar(stream)->codec_id == AV_CODEC_ID_AAC_LATM)
                    {
                        const char marker[] = "LATM";
                        track.aacbuflen = sizeof(marker)/sizeof(char);
                        track.aacbuf = malloc(track.aacbuflen);
                        memcpy(track.aacbuf, marker, track.aacbuflen);
                        
                        ffmpeg_printf(10, "AV_CODEC_ID_AAC_LATM no extradata ACC header should be available in each frame\n");
                        track.have_aacheader = 1;
                    }
                    else if(!strncmp(encoding, "A_AAC_LATM", 10))
                    {
                        ffmpeg_printf(10, "AV_CODEC_ID_AAC_LATM extradata will be used in aac writter\n");
                    }
                    else if (get_codecpar(stream)->codec_id == AV_CODEC_ID_AAC)
                    {
                        if( 0 == strncmp(avContext->iformat->name, "mpegts", 6) || 
                            0 == strncmp(avContext->iformat->name, "hls,", 4) )
                        {
                            const char marker[] = "ADTS";
                            track.aacbuflen = sizeof(marker)/sizeof(char);
                            track.aacbuf = malloc(track.aacbuflen);
                            memcpy(track.aacbuf, marker, track.aacbuflen);
                            
                            ffmpeg_printf(10, "AV_CODEC_ID_AAC no extradata ACC header should be available in each frame\n");
                            track.have_aacheader = 1;
                        }
                        else
                        {
                            ffmpeg_printf(10, "Create AAC ExtraData\n");
                            ffmpeg_printf(10, "get_codecpar(stream)->extradata_size %d\n", get_codecpar(stream)->extradata_size);
                            //Hexdump(get_codecpar(stream)->extradata, get_codecpar(stream)->extradata_size);

                            /*  extradata:
                                13 10 56 e5 9d 48 00 (anderen cops)
                                object_type: 00010 2 = LC
                                sample_rate: 011 0 6 = 24000
                                chan_config: 0010 2 = Stereo
                                000 0
                                1010110 111 = 0x2b7
                                00101 = SBR
                                1
                                0011 = 48000
                                101 01001000 = 0x548
                                ps = 0
                                0000000
                            */

                            int32_t object_type = 2; // LC
                            int32_t sample_index = aac_get_sample_rate_index(get_codecpar(stream)->sample_rate);
#if HAVE_CH_LAYOUT
                            int32_t chan_config = get_chan_config(get_codecpar(stream)->ch_layout.nb_channels);
#else
                            int32_t chan_config = get_chan_config(get_codecpar(stream)->channels);
#endif
                            ffmpeg_printf(1,"aac object_type %d\n", object_type);
                            ffmpeg_printf(1,"aac sample_index %d\n", sample_index);
                            ffmpeg_printf(1,"aac chan_config %d\n", chan_config);
                            
                            int off = -1;
                            if (get_codecpar(stream)->extradata_size >= 2)
                            {
                                MPEG4AudioConfig m4ac;
                                off = avpriv_mpeg4audio_get_config(&m4ac, get_codecpar(stream)->extradata, get_codecpar(stream)->extradata_size * 8, 1);
                                if (off >= 0)
                                {
                                    object_type  = m4ac.object_type;
                                    sample_index = m4ac.sampling_index;
                                    if (sample_index == 0x0f)
                                    {
                                        sample_index = aac_get_sample_rate_index(m4ac.sample_rate);
                                    }
                                    chan_config  = m4ac.chan_config;
                                }
                            }
                            
                            ffmpeg_printf(1,"aac object_type %d\n", object_type);
                            ffmpeg_printf(1,"aac sample_index %d\n", sample_index);
                            ffmpeg_printf(1,"aac chan_config %d\n", chan_config);

                            if (off >= 0 && chan_config == 0) { // channel config must be send in the inband PCE
                                track.aacbuf = malloc(AAC_HEADER_LENGTH + MAX_PCE_SIZE);

                                GetBitContext gb;
                                PutBitContext pb;
                                init_put_bits(&pb, track.aacbuf + AAC_HEADER_LENGTH, MAX_PCE_SIZE);
                                init_get_bits8(&gb, get_codecpar(stream)->extradata, get_codecpar(stream)->extradata_size);
                                skip_bits_long(&gb, off + 3);

                                put_bits(&pb, 3, 5); //ID_PCE
                                track.aacbuflen = AAC_HEADER_LENGTH + (avpriv_copy_pce_data(&pb, &gb) + 3) / 8;
                                flush_put_bits(&pb);
                            }
                            else {
                                track.aacbuflen = AAC_HEADER_LENGTH;
                                track.aacbuf = malloc(AAC_HEADER_LENGTH+1);
                            }

                            // https://wiki.multimedia.cx/index.php/ADTS
                            object_type -= 1; //ADTS - profile, the MPEG-4 Audio Object Type minus 1

                            track.aacbuf[0] = 0xFF;
                            track.aacbuf[1] = 0xF1;
                            //track.aacbuf[1] |=0x8;
                            track.aacbuf[2] = ((object_type & 0x03) << 6)  | (sample_index << 2) | ((chan_config >> 2) & 0x01);
                            track.aacbuf[3] = (chan_config & 0x03) << 6;
                            //track.aacbuf[3]|= 0x10;
                            track.aacbuf[4] = 0x00;
                            track.aacbuf[5] = 0x1F;
                            track.aacbuf[6] = 0xFC;

                            //printf("AAC_HEADER -> ");
                            //Hexdump(track.aacbuf,7);
                            track.have_aacheader = 1;
                        }
                        /*
                        else
                        {
                            ffmpeg_err("AV_CODEC_ID_AAC extradata not available\n");
                        }
                        */

                    }
#ifdef __sh__
                    else if(get_codecpar(stream)->codec_id == AV_CODEC_ID_WMAV1
                        || get_codecpar(stream)->codec_id == AV_CODEC_ID_WMAV2
                        || get_codecpar(stream)->codec_id == AV_CODEC_ID_WMAPRO
                        || get_codecpar(stream)->codec_id == AV_CODEC_ID_WMALOSSLESS) //if (get_codecpar(stream)->extradata_size > 0)
                    {
                        ffmpeg_printf(10,"Create WMA ExtraData\n");

                        // type_specific_data
                        uint16_t codec_id = 0;
                        switch(get_codecpar(stream)->codec_id)
                        {
                            //TODO: What code for lossless ?
                            case AV_CODEC_ID_WMALOSSLESS:
                                codec_id = 0x163; // WMA_LOSSLESS
                                break;
                            case AV_CODEC_ID_WMAPRO:
                                codec_id = 0x162; // WMA_VERSION_9_PRO
                                break;
                            case AV_CODEC_ID_WMAV2:
                                codec_id = 0x161; // WMA_VERSION_2_9
                                break;
                            case AV_CODEC_ID_WMAV1:
                            default:
                                codec_id = 0x160; // WMA_VERSION_1
                                break;
                        }
                        
                        track.aacbuflen = 104 + get_codecpar(stream)->extradata_size;
                        track.aacbuf = malloc(track.aacbuflen);
                        memset (track.aacbuf, 0, track.aacbuflen);
                        
                        uint8_t ASF_Stream_Properties_Object[16] =
                        {0x91,0x07,0xDC,0xB7,0xB7,0xA9,0xCF,0x11,0x8E,0xE6,0x00,0xC0,0x0C,0x20,0x53,0x65};
                        
                        memcpy(track.aacbuf + 0, ASF_Stream_Properties_Object, 16); // ASF_Stream_Properties_Object
                        memcpy(track.aacbuf + 16, &track.aacbuflen, 4); //FrameDateLength

                        uint32_t sizehi = 0;
                        memcpy(track.aacbuf + 20, &sizehi, 4); // sizehi (not used)

                        uint8_t ASF_Audio_Media[16] =
                        {0x40,0x9E,0x69,0xF8,0x4D,0x5B,0xCF,0x11,0xA8,0xFD,0x00,0x80,0x5F,0x5C,0x44,0x2B};
                        
                        memcpy(track.aacbuf + 24, ASF_Audio_Media, 16); //ASF_Audio_Media

                        uint8_t ASF_Audio_Spread[16] =
                        {0x50,0xCD,0xC3,0xBF,0x8F,0x61,0xCF,0x11,0x8B,0xB2,0x00,0xAA,0x00,0xB4,0xE2,0x20};
                        
                        memcpy(track.aacbuf + 40, ASF_Audio_Spread, 16); //ASF_Audio_Spread

                        memset(track.aacbuf + 56, 0, 4); // time_offset (not used)
                        memset(track.aacbuf + 60, 0, 4); // time_offset_hi (not used)

                        uint8_t type_specific_data_length = 18 + get_codecpar(stream)->extradata_size;
                        memcpy(track.aacbuf + 64, &type_specific_data_length, 4); //type_specific_data_length

                        uint8_t error_correction_data_length = 8;
                        memcpy(track.aacbuf + 68, &error_correction_data_length, 4); //error_correction_data_length

                        uint16_t flags = 1; // stream_number
                        memcpy(track.aacbuf + 72, &flags, 2); //flags

                        uint32_t reserved = 0;
                        memcpy(track.aacbuf + 74, &reserved, 4); // reserved

                        memcpy(track.aacbuf + 78, &codec_id, 2); //codec_id

                        uint16_t number_of_channels = get_codecpar(stream)->channels;
                        memcpy(track.aacbuf + 80, &number_of_channels, 2); //number_of_channels

                        uint32_t samples_per_second = get_codecpar(stream)->sample_rate;
                        ffmpeg_printf(1, "samples_per_second = %d\n", samples_per_second);
                        memcpy(track.aacbuf + 82, &samples_per_second, 4); //samples_per_second

                        uint32_t average_number_of_bytes_per_second = get_codecpar(stream)->bit_rate / 8;
                        ffmpeg_printf(1, "average_number_of_bytes_per_second = %d\n", average_number_of_bytes_per_second);
                        memcpy(track.aacbuf + 86, &average_number_of_bytes_per_second, 4); //average_number_of_bytes_per_second

                        uint16_t block_alignment = get_codecpar(stream)->block_align;
                        ffmpeg_printf(1, "block_alignment = %d\n", block_alignment);
                        memcpy(track.aacbuf + 90, &block_alignment, 2); //block_alignment

#if (LIBAVFORMAT_VERSION_MAJOR > 57) || ((LIBAVFORMAT_VERSION_MAJOR == 57) && (LIBAVFORMAT_VERSION_MINOR > 32))
                        enum AVSampleFormat sample_fmt = get_codecpar(stream)->format;
#else
                        enum AVSampleFormat sample_fmt = get_codecpar(stream)->sample_fmt;
#endif
                        uint16_t bits_per_sample = sample_fmt>=0 ? (sample_fmt+1)*8 : 8;
                        ffmpeg_printf(1, "bits_per_sample = %d (%d)\n", bits_per_sample, sample_fmt);
                        memcpy(track.aacbuf + 92, &bits_per_sample, 2); //bits_per_sample

                        memcpy(track.aacbuf + 94, &get_codecpar(stream)->extradata_size, 2); //bits_per_sample

                        memcpy(track.aacbuf + 96, get_codecpar(stream)->extradata, get_codecpar(stream)->extradata_size);

                        ffmpeg_printf(1, "aacbuf:\n");
                        track.have_aacheader = 1;
                    }
#endif
                    if (context->manager->audio)
                    {
                        ffmpeg_printf(1, "cAVIdx[%d]: MANAGER_ADD track AUDIO\n", cAVIdx);
                        if (context->manager->audio->Command(context, MANAGER_ADD, &track) < 0) 
                        {
                            /* konfetti: fixme: is this a reason to return with error? */
                            ffmpeg_err("failed to add track %d\n", n);
                        }
                    }
                }
                else //encoding != NULL
                {
                    ffmpeg_err("codec type audio but codec unknown %d\n", get_codecpar(stream)->codec_id);
                }
                break;
            case AVMEDIA_TYPE_SUBTITLE:
            {
                if (get_codecpar(stream)->codec_id != AV_CODEC_ID_SSA &&
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55, 3, 100)
                    get_codecpar(stream)->codec_id != AV_CODEC_ID_ASS &&
#endif
                    get_codecpar(stream)->codec_id != AV_CODEC_ID_SUBRIP && 
                    get_codecpar(stream)->codec_id != AV_CODEC_ID_TEXT &&
                    get_codecpar(stream)->codec_id != AV_CODEC_ID_SRT &&
                    get_codecpar(stream)->codec_id != AV_CODEC_ID_WEBVTT &&
                    get_codecpar(stream)->codec_id != AV_CODEC_ID_MOV_TEXT &&
                    ((get_codecpar(stream)->codec_id != AV_CODEC_ID_HDMV_PGS_SUBTITLE &&
                      get_codecpar(stream)->codec_id != AV_CODEC_ID_DVB_SUBTITLE &&
                      get_codecpar(stream)->codec_id != AV_CODEC_ID_XSUB) ||
                     !GetGraphicSubPath() || !GetGraphicSubPath()[0]))
                {
                    ffmpeg_printf(10, "subtitle with not supported codec codec_id[%u]\n", (uint32_t)get_codecpar(stream)->codec_id);
                }
                else if (initial && context->manager->subtitle)
                {
                    AVDictionaryEntry *lang = NULL;
                    memset(&track, 0, sizeof(track));

                    ffmpeg_printf(10, "CODEC_TYPE_SUBTITLE %d\n", get_codecpar(stream)->codec_type);
                    
                    lang = av_dict_get(stream->metadata, "language", NULL, 0);
                    track.Name = lang ? lang->value : "und";
                    ffmpeg_printf(10, "Language %s\n", track.Name);

                    track.Encoding       = encoding;
                    track.stream         = stream;
                    track.Id             = ((AVStream *) (track.stream))->id;
                    track.duration       = (int64_t)av_rescale(stream->duration, (int64_t)stream->time_base.num * 1000, stream->time_base.den);
                    
                    if(stream->duration == AV_NOPTS_VALUE) 
                    {
                        ffmpeg_printf(10, "Stream has no duration so we take the duration from context\n");
                        track.duration = (AV_NOPTS_VALUE == avContext->duration) ? 0 : (int64_t) avContext->duration / 1000;
                    }

                    ffmpeg_printf(1, "subtitle codec %d\n", get_codecpar(stream)->codec_id);
                    ffmpeg_printf(1, "subtitle width %d\n", get_codecpar(stream)->width);
                    ffmpeg_printf(1, "subtitle height %d\n", get_codecpar(stream)->height);
                    ffmpeg_printf(1, "subtitle stream %p\n", stream);

                    ffmpeg_printf(10, "FOUND SUBTITLE %s\n", track.Name);
                    
                    if (context->manager->subtitle->Command(context, MANAGER_ADD, &track) < 0)
                    {
                        ffmpeg_err("failed to add subtitle track %d\n", n);
                    }
                }
                break;
            }
            case AVMEDIA_TYPE_UNKNOWN:
            case AVMEDIA_TYPE_DATA:
            case AVMEDIA_TYPE_ATTACHMENT:
            case AVMEDIA_TYPE_NB:
            default:
                if (get_codecpar(stream)->codec_id == AV_CODEC_ID_NONE)
                {
                    /* Noch nicht klassifiziert (private/proprietaere MPEG-TS
                     * stream_type-Codes im Bereich 0x80-0xFF, wie sie manche
                     * Live-Streams bei Werbe-Segmenten verwenden) - NICHT
                     * verwerfen, damit FFmpegs eigener Auto-Erkennungs-
                     * mechanismus (mpegts.c:1219, request_probe beim naechsten
                     * PES-Paket) ueberhaupt eine Chance bekommt zu laufen.
                     * mpegts.c:1187 ueberspringt Pakete mit
                     * discard==AVDISCARD_ALL VOR der request_probe-Zuweisung -
                     * sofortiges Verwerfen hat die eingebaute Bitstream-
                     * Erkennung bisher strukturell verhindert. */
                }
                else
                {
                    stream->discard = AVDISCARD_ALL;
                    ffmpeg_err("not handled or unknown codec_type %d\n", get_codecpar(stream)->codec_type);
                }
             break;
            }
        } /* for */
    
    }
    
    if (context->manager->audio)
    {
        Track_t *Tracks = NULL;
        int32_t TrackCount = 0;
        int32_t selTrackIdx = -1;
        
        context->manager->audio->Command(context, MANAGER_REF_LIST, &Tracks);
        context->manager->audio->Command(context, MANAGER_REF_LIST_SIZE, &TrackCount);
        if (Tracks && TrackCount)
        {
            int32_t i;
            for (i=0; i < TrackCount; ++i)
            {
                if (Tracks[i].pending || Tracks[i].Id < 0)
                    continue;
                
                if (selTrackIdx == -1)
                    selTrackIdx = i;
                    
                if (currAudioTrack && currAudioTrack->Id == Tracks[i].Id)
                {
                    selTrackIdx = i;
                    break;
                }
            }
            
            if (selTrackIdx > -1)
            {
                ((AVStream*)Tracks[selTrackIdx].stream)->discard = AVDISCARD_DEFAULT;
                if (!currAudioTrack || currAudioTrack->Id != Tracks[selTrackIdx].Id )
                {
                    context->manager->audio->Command(context, MANAGER_SET, &Tracks[selTrackIdx].Id);
                }
            }
        }
    }
    
    releaseMutex(__FILE__, __FUNCTION__,__LINE__);
    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}

static int32_t container_ffmpeg_play(Context_t *context)
{
    int32_t error = 0;
    int32_t ret = 0;
    pthread_attr_t attr;

    ffmpeg_printf(10, "\n");

    if ( context && context->playback && context->playback->isPlaying ) 
    {
        ffmpeg_printf(10, "is Playing\n");
    }
    else 
    {
        ffmpeg_printf(10, "is NOT Playing\n");
    }

    if (hasPlayThreadStarted == 0) 
    {
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        if((error = pthread_create(&PlayThread, &attr, (void *)&FFMPEGThread, context)) != 0) 
        {
            ffmpeg_printf(10, "Error creating thread, error:%d:%s\n", error,strerror(error));

            hasPlayThreadStarted = 0;
            ret = cERR_CONTAINER_FFMPEG_ERR;
        }
        else 
        {
            ffmpeg_printf(10, "Created thread\n");

            hasPlayThreadStarted = 1;
        }
    }
    else 
    {
        ffmpeg_printf(10, "A thread already exists!\n");
        ret = cERR_CONTAINER_FFMPEG_ERR;
    }

    ffmpeg_printf(10, "exiting with value %d\n", ret);
    return ret;
}

static int32_t container_ffmpeg_stop(Context_t *context) 
{
    int32_t ret = cERR_CONTAINER_FFMPEG_NO_ERROR;
    int32_t wait_time = 10; // we give 1s to close otherwise we will force close

    RAW_DEBUG_LOG("[exteplayer3] container_ffmpeg_stop: enter, hasPlayThreadStarted=%d\n", hasPlayThreadStarted);

    if (!isContainerRunning)
    {
        ffmpeg_err("Container not running\n");
        return cERR_CONTAINER_FFMPEG_ERR;
    }
    
    if (context->playback)
    {
        context->playback->isPlaying = 0;
    }

    int32_t wait_iterations = 200; // 200 * 5ms = 1000ms

    while ( (hasPlayThreadStarted != 0) && (--wait_iterations) > 0 ) 
    {
        RAW_DEBUG_LOG("[exteplayer3] container_ffmpeg_stop: waiting for play thread, wait_iterations=%d\n", wait_iterations);
        usleep(5000); // 5ms
    }

    if (wait_iterations == 0) 
    {
        /* force close */
        RAW_DEBUG_LOG("[exteplayer3] container_ffmpeg_stop: Timeout waiting for play thread!\n");
        ret = cERR_CONTAINER_FFMPEG_ERR;
        /* to speed up close - we are in separate process for the moment this process will 
         * be closed and whole resources will be free by the system  
         */ 
        return ret;
    }
    else
    {
        RAW_DEBUG_LOG("[exteplayer3] container_ffmpeg_stop: play thread terminated cleanly\n");
    }

    if (g_pcm_audio_export_fd >= 0)
    {
        /* Sauberes EOF fuer einen etwaigen Leser bei regulaerem Stream-Ende,
         * statt dass er das erst beim Prozessende implizit sieht. */
        close(g_pcm_audio_export_fd);
        g_pcm_audio_export_fd = -1;
        g_pcm_audio_export_last_open_attempt_us = 0;
    }

    hasPlayThreadStarted = 0;
    terminating = 1;

    getMutex_wr(__FILE__, __FUNCTION__,__LINE__);
    
    free_all_stored_avcodec_context();
    
    uint32_t i = 0;
    for(i=0; i<IPTV_AV_CONTEXT_MAX_NUM; i+=1)
    {
        if(NULL != avContextTab[i])
        {
            if(0 != use_custom_io[i])
            {
                /*
                 * Free custom IO independently to avoid segfault/bus error 
                 * avformat_close_input do not expect custom io, so it try
                 * to release incorrectly
                 */
                av_freep(&(avContextTab[i]->pb->buffer));
                av_freep(&(avContextTab[i]->pb));
                use_custom_io[i] = 0;
            }
            avformat_close_input(&avContextTab[i]);
            avContextTab[i] = NULL;
        }
        else
        {
            break;
        }
    }

    if(g_avio_opts != NULL)
    {
        av_dict_free(&g_avio_opts);
    }

    isContainerRunning = 0;
    avformat_network_deinit();
    ffmpeg_buf_free();

    releaseMutex(__FILE__, __FUNCTION__,__LINE__);

    ffmpeg_printf(10, "ret %d\n", ret);
    return ret;
}

static int32_t container_ffmpeg_seek_bytes(off_t pos) 
{
    int32_t flag = AVSEEK_FLAG_BYTE;
    off_t current_pos = avio_tell(avContextTab[0]->pb);

    ffmpeg_printf(20, "seeking to position %"PRId64" (bytes)\n", pos);

    if (current_pos > pos)
    {
        flag |= AVSEEK_FLAG_BACKWARD;
    }

    if (avformat_seek_file(avContextTab[0], -1, INT64_MIN, pos, INT64_MAX, flag) < 0)
    {
        ffmpeg_err( "Error seeking\n");
        return cERR_CONTAINER_FFMPEG_ERR;
    }

    ffmpeg_printf(30, "current_pos after seek %"PRId64"\n", avio_tell(avContextTab[0]->pb));

    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}

static int32_t container_ffmpeg_seek(Context_t *context, int64_t sec, uint8_t absolute)
{
    Track_t *videoTrack = NULL;
    Track_t *audioTrack = NULL;
    Track_t *current = NULL;

    if (!absolute) 
    {
        ffmpeg_printf(10, "seeking %"PRId64" sec\n", sec);

        int64_t length = 0;

        context->playback->Command(context, PLAYBACK_LENGTH, (void*)&length);

        int64_t currPts = -1;
        int ret = context->playback->Command(context, PLAYBACK_PTS, &currPts);
        if (ret != 0)
        {
            ffmpeg_err("fail to get current PTS\n");
            return cERR_CONTAINER_FFMPEG_ERR;
        }
        int64_t cur_sec = currPts / 90000;

//        ffmpeg_err("cur_sec = %lld\n", (long long) cur_sec);
//        ffmpeg_err("length = %lld\n", (long long) length);

        /* 
         * - 0s relative seek is used when switching subtitle or audio tracks to properly flush/reload queues
         * - in some types of live streams this can result in hang or seek behind the end of the stream
         * - try to detect such streams - mostly they have incorrect (negative) length or current position is behind the end
         * - in these cases just ignore seek command
         */

        if (sec == 0 && cur_sec > length)
        {
            ffmpeg_err("sec = 0 ignoring\n");
            return cERR_CONTAINER_FFMPEG_ERR;
        }

        sec += cur_sec;
    }
    
    ffmpeg_printf(10, "goto %"PRId64" sec\n", sec);
    if (sec < 0)
    {
        sec = 0;
    }
    
    context->manager->video->Command(context, MANAGER_GET_TRACK, &videoTrack);
    context->manager->audio->Command(context, MANAGER_GET_TRACK, &audioTrack);

    if (videoTrack != NULL)
    {
        current = videoTrack;
    }
    else if (audioTrack != NULL)
    {
        current = audioTrack;
    }
    else
    {
        ffmpeg_err( "no track available to seek\n");
        return cERR_CONTAINER_FFMPEG_ERR;
    }

    if (!context->playback || !context->playback->isPlaying)
    {
        return cERR_CONTAINER_FFMPEG_NO_ERROR;
    }

    getSeekMutex();
    g_seek_target_seconds = sec * AV_TIME_BASE;
    g_do_seek_target_seconds = true;
    g_stamp = context->playback->stamp;
    releaseSeekMutex();

    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}

static int32_t container_ffmpeg_get_length(Context_t *context, int64_t *length) 
{
    ffmpeg_printf(50, "\n");
    Track_t * videoTrack = NULL;
    Track_t * audioTrack = NULL;
    Track_t * current = NULL;

    if (length == NULL) 
    {
        ffmpeg_err( "null pointer passed\n");
        return cERR_CONTAINER_FFMPEG_ERR;
    }

    context->container->selectedContainer->Command(context, CONTAINER_UPDATE_DURATION, NULL);
    context->manager->video->Command(context, MANAGER_GET_TRACK, &videoTrack);
    context->manager->audio->Command(context, MANAGER_GET_TRACK, &audioTrack);

    if (videoTrack != NULL)
    {
        current = videoTrack;
    }
    else if (audioTrack != NULL)
    {
        current = audioTrack;
    }
    
    *length = 0;

    if (current != NULL)
    {
        /* current->duration <= 0 statt nur == 0: Bei manchen echten Live-
         * Streams ohne jede Laengenangabe (z.B. reine Webcam-URLs) kann die
         * Dauer-Berechnung weiter oben (av_rescale bzw. avContext->duration)
         * je nach Eingabewerten einen negativen Ueberlauf statt exakt 0
         * ergeben. Eine negative "Laenge" ist fuer einen echten Stream nie
         * plausibel und wurde bisher unveraendert als PLAYBACK_LENGTH an
         * serviceapp/Enigma2 durchgereicht - dort blieb die Zeitanzeige trotz
         * laufender Wiedergabe auf 0:00 stehen, weil Enigma2 mit dem Wert
         * nichts anfangen konnte. */
        if (current->duration <= 0)
        {
            return cERR_CONTAINER_FFMPEG_ERR;
        }
        else
        {
            *length = current->duration / 1000;
        }
    }
    else 
    {
        if (avContextTab[0] != NULL)
        {
            *length = avContextTab[0]->duration / 1000;
        } 
        else
        {
           ffmpeg_err( "no Track not context ->no problem :D\n");
           return cERR_CONTAINER_FFMPEG_ERR;
        }
    }

    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}

static int32_t container_ffmpeg_switch_audio(Context_t *context, int32_t *arg)
{
    ffmpeg_printf(10, "track %d\n", *arg);
    getMutex_wr(__FILE__, __FUNCTION__,__LINE__);
    if (context->manager->audio)
    {
        Track_t *Tracks = NULL;
        int32_t TrackCount = 0;
        
        context->manager->audio->Command(context, MANAGER_REF_LIST, &Tracks);
        context->manager->audio->Command(context, MANAGER_REF_LIST_SIZE, &TrackCount);
        if (Tracks && TrackCount)
        {
            int32_t i;
            for (i=0; i < TrackCount; ++i)
            {
                // do not discard any stream from second context
                if (Tracks[i].AVIdx == 0) {
                    ((AVStream*)Tracks[i].stream)->discard = Tracks[i].Id == *arg ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
                }
            }
        }
    }
    releaseMutex(__FILE__, __FUNCTION__,__LINE__);
    
    /* Hellmaster1024: nothing to do here!*/
    int64_t sec = 0;
    context->playback->Command(context, PLAYBACK_SEEK, (void*)&sec);
    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}

static int32_t container_ffmpeg_switch_subtitle(Context_t *context, int32_t *arg)
{
    ffmpeg_printf(10, "track %d\n", *arg);
    
    /* This is made to flush inside the buffer because 
     * subtitles frame was already read and ignored
     * we seek to force ffmpeg to read once again the same data
     * but now we will not ignore subtitle frame
     */
    int64_t sec = 0;
    context->playback->Command(context, PLAYBACK_SEEK, (void*)&sec);
    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}

/* konfetti comment: I dont like the mechanism of overwriting
 * the pointer in infostring. This lead in most cases to
 * user errors, like it is in the current version (libeplayer2 <-->e2->servicemp3.cpp)
 * From e2 there is passed a tag=strdup here and we overwrite this
 * strdupped tag. This lead to dangling pointers which are never freed!
 * I do not free the string here because this is the wrong way. The mechanism
 * should be changed, or e2 should pass it in a different way...
 */
static int32_t container_ffmpeg_get_info(Context_t* context, char ** infoString)
{
    Track_t *videoTrack = NULL;
    Track_t *audioTrack = NULL;
    char     *meta = NULL;

    ffmpeg_printf(20, ">\n");

    if (avContextTab[0] != NULL)
    {
        if ((infoString == NULL) || (*infoString == NULL))
        {
            ffmpeg_err("infostring NULL\n");
            return cERR_CONTAINER_FFMPEG_ERR;
        }

        ffmpeg_printf(20, "%s\n", *infoString);

        context->manager->video->Command(context, MANAGER_GET_TRACK, &videoTrack);
        context->manager->audio->Command(context, MANAGER_GET_TRACK, &audioTrack);

        if ((meta = searchMeta(avContextTab[0]->metadata, *infoString)) == NULL)
        {
            if (audioTrack != NULL)
            {
                AVStream* stream = audioTrack->stream;
                meta = searchMeta(stream->metadata, *infoString);
            }

            if ((meta == NULL) && (videoTrack != NULL))
            {
                AVStream* stream = videoTrack->stream;
                meta = searchMeta(stream->metadata, *infoString);
            }
        }

        if (meta != NULL)
        {
            *infoString = strdup(meta);
        }
        else
        {
            ffmpeg_printf(1, "no metadata found for \"%s\"\n", *infoString);
            *infoString = strdup("not found");
        }
    } 
    else
    {
        ffmpeg_err("avContext NULL\n");
        return cERR_CONTAINER_FFMPEG_ERR;
    }

    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}


static int32_t container_ffmpeg_update_duration(Context_t *context)
{
    if (terminating)
    {
        return cERR_CONTAINER_FFMPEG_NO_ERROR;
    }

    getMutex(__FILE__, __FUNCTION__,__LINE__);
    ffmpeg_printf(10, "Updating durations\n");

    uint32_t cAVIdx = 0;
    for(cAVIdx=0; cAVIdx<IPTV_AV_CONTEXT_MAX_NUM; cAVIdx+=1)
    {
        if(NULL == avContextTab[cAVIdx])
        {
            break;
        }

        ffmpeg_printf(10, "Updating duration for context #%u\n", cAVIdx);
        AVFormatContext *avContext = avContextTab[cAVIdx];

        int32_t n;

        for (n = 0; n < avContext->nb_streams; n++)
        {
            ffmpeg_printf(10, "Updating duration for stream #%d\n", n);
            Track_t *track = NULL;
            AVStream *stream = avContext->streams[n];

            /* some values in track are unset and therefor copyTrack segfaults.
             * so set it by default to NULL!
             */

            switch (get_codecpar(stream)->codec_type)
            {
                case AVMEDIA_TYPE_VIDEO:
                {
                    ffmpeg_printf(10, "Requesting video track\n");
                    context->manager->video->Command(context, MANAGER_GET_TRACK, &track);

                    if( track )
                    {
                        ffmpeg_printf(10, "Old duration: %"PRId64"\n", track->duration);

                        track->duration = (int64_t)av_rescale(stream->duration, (int64_t)stream->time_base.num * 1000, stream->time_base.den);
                        if(stream->duration == AV_NOPTS_VALUE || 0 == strncmp(avContext->iformat->name, "dash", 4))
                        {
                            ffmpeg_printf(10, "Stream has no duration so we take the duration from context\n");
                            track->duration = (int64_t) avContext->duration / 1000;
                        }

                        ffmpeg_printf(10, "New duration: %"PRId64"\n", track->duration);
                    }
                    else
                    {
                        ffmpeg_printf(10, "Video track not found\n");
                    }
                }
                break;

                case AVMEDIA_TYPE_AUDIO:
                {
                    context->manager->audio->Command(context, MANAGER_GET_TRACK, &track);

                    if( track )
                    {
                        ffmpeg_printf(10, "Old duration: %"PRId64"\n", track->duration);

                        track->duration = (int64_t)av_rescale(stream->duration, (int64_t)stream->time_base.num * 1000, stream->time_base.den);

                        if(stream->duration == AV_NOPTS_VALUE)
                        {
                            ffmpeg_printf(10, "Stream has no duration so we take the duration from context\n");
                            track->duration = (int64_t) avContext->duration / 1000;
                        }

                        ffmpeg_printf(10, "New duration: %"PRId64"\n", track->duration);
                    }
                    else
                    {
                        ffmpeg_printf(10, "Audio track not found\n");
                    }
                }
                break;

                case AVMEDIA_TYPE_SUBTITLE:
                {
                    context->manager->subtitle->Command(context, MANAGER_GET_TRACK, &track);
                    if( track )
                    {
                        ffmpeg_printf(10, "Old duration: %"PRId64"\n", track->duration);

                        track->duration = (int64_t)av_rescale(stream->duration, (int64_t)stream->time_base.num * 1000, stream->time_base.den);

                        if(stream->duration == AV_NOPTS_VALUE)
                        {
                            ffmpeg_printf(10, "Stream has no duration so we take the duration from context\n");
                            track->duration = (int64_t) avContext->duration / 1000;
                        }

                        ffmpeg_printf(10, "New duration: %"PRId64"\n", track->duration);
                    }
                    else
                    {
                        ffmpeg_printf(10, "Subtitle track not found\n");
                    }

                    break;
                }
                break;
            }
        } /* for */
    }

    releaseMutex(__FILE__, __FUNCTION__,__LINE__);
    return cERR_CONTAINER_FFMPEG_NO_ERROR;
}

static int32_t Command(void  *_context, ContainerCmd_t command, void *argument)
{
    Context_t  *context = (Context_t*) _context;
    int ret = cERR_CONTAINER_FFMPEG_NO_ERROR;

    ffmpeg_printf(50, "Command %d\n", command);

    if(command != CONTAINER_SET_BUFFER_SEEK_TIME && 
       command != CONTAINER_SET_BUFFER_SIZE && 
       command != CONTAINER_GET_BUFFER_SIZE && 
       command != CONTAINER_GET_BUFFER_STATUS &&
       command != CONTAINER_STOP_BUFFER && 
       command != CONTAINER_INIT && !avContextTab[0])
    {
        return cERR_CONTAINER_FFMPEG_ERR;
    }
    
    
    switch(command)
    {
    case CONTAINER_INIT:
    {
        PlayFiles_t *playFilesNames = (PlayFiles_t *)argument;
        ret = container_ffmpeg_init(context, playFilesNames);
        break;
    }
    case CONTAINER_PLAY:  
    {
        ret = container_ffmpeg_play(context);
        break;
    }
    case CONTAINER_STOP:
    {
        ret = container_ffmpeg_stop(context);
        break;
    }
    case CONTAINER_SEEK: 
    {
        ret = container_ffmpeg_seek(context, (int64_t)*((int64_t*)argument), 0);
        break;
    }
    case CONTAINER_SEEK_ABS: 
    {
        ret = container_ffmpeg_seek(context, (int64_t)*((int64_t*)argument), -1);
        break;
    }
    case CONTAINER_LENGTH: 
    {
        int64_t length = 0;
        ret = container_ffmpeg_get_length(context, &length);
        *((int64_t*)argument) = (int64_t)length;
        break;
    }
    case CONTAINER_SWITCH_AUDIO: 
    {
        ret = container_ffmpeg_switch_audio(context, (int32_t*) argument);
        break;
    }
    case CONTAINER_SWITCH_SUBTITLE: 
    {
        ret = container_ffmpeg_switch_subtitle(context, (int32_t*) argument);
        break;
    }
    case CONTAINER_INFO:
    {
        ret = container_ffmpeg_get_info(context, (char **)argument);
        break;
    }
    case CONTAINER_STATUS:
    {
        *((int32_t*)argument) = hasPlayThreadStarted;
        break;
    }
    case CONTAINER_LAST_PTS:
    {
        *((int64_t*)argument) = latestPts;
        break;
    }
    case CONTAINER_SET_BUFFER_SIZE:
    {
        ret = container_set_ffmpeg_buf_size((int32_t *) argument);
        break;
    }
    case CONTAINER_GET_BUFFER_SIZE:
    {
        int32_t size = 0;
        ret = container_get_ffmpeg_buf_size(&size);
        *((int32_t*)argument) = size;
        break;
    }   
    case CONTAINER_UPDATE_DURATION:
    {
        ret = container_ffmpeg_update_duration(context);
        break;
    }
    default:
        ffmpeg_err("ContainerCmd %d not supported!\n", command);
        ret = cERR_CONTAINER_FFMPEG_ERR;
        break;
    }

    ffmpeg_printf(50, "exiting with value %d\n", ret);
    return ret;
}

static char *FFMPEG_Capabilities[] = {"aac", "avi", "mkv", "mp4", "ts", "mov", "flv", "flac", "mp3", "mpg", "m2ts", "vob", "evo", "wmv","wma", "asf", "mp2", "m4v", "m4a", "fla", "divx", "dat", "mpeg", "trp", "mts", "vdr", "ogg", "wav", "wtv", "asx", "mvi", "png", "jpg", "jpeg", "ra", "ram", "rm", "3gp", "amr", "rmvb", "rm", "webm", "opus", "m3u8", "mpd", NULL };

Container_t FFMPEGContainer = {
    "FFMPEG",
    &Command,
    FFMPEG_Capabilities
};
