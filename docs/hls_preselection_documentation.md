# HLS Native Stream Preselection and Audio Filtering

This document details the architecture and implementation of the native HLS stream preselection and audio/subtitle track filtering inside FFmpeg and `exteplayer3`.

## 1. The Problem
Historically, when playing an HLS Master Playlist (containing multiple video resolutions/bandwidths and multiple audio tracks/CDNs), the standard FFmpeg HLS demuxer parsed the master playlist and immediately proceeded to initialize the sub-demuxer context (`pls->ctx`) for **every single audio rendition** during `hls_read_header()`. 

To probe codecs and stream properties, `avformat_find_stream_info` downloaded segment files for all audio tracks. For streams with multiple audio tracks (such as ZDF in `OeMediathek` or `StreamAnything`, which have primary and backup audio tracks across 6-8 rendition playlists), this resulted in:
- A delay of **6 to 10 seconds** during startup.
- Heavy network traffic downloading unneeded `.m3u8` playlists and audio segments.
- Potential player hangs or timeouts (often resulting in audio-only playback with `w:0, h:0` video properties due to missing or delayed video analysis).

---

## 2. The Solution: Native HLS Preselection
Instead of attempting to modify stream states *after* they are parsed (which is too late to prevent sub-demuxer initialization and downloads), I implemented **native HLS stream preselection** inside the FFmpeg HLS demuxer (`libavformat/hls.c` im FFmpeg-Quellbaum, hier nicht enthalten — siehe `exteplayer3-build/ffmpeg-hls-native-preselect.patch`).

### Custom AVOptions in `HLSContext`
I added two custom configuration parameters directly to FFmpeg's `hls_options` array:
- `hls_quality_mode` (int: `0` = auto/default, `1` = lowest quality, `2` = highest quality)
- `hls_audio_default_only` (bool: `0` = load all audio tracks, `1` = only load the `DEFAULT=YES` audio rendition)

These options are stored in the private `HLSContext` structure:
```c
typedef struct HLSContext {
    ...
    int hls_quality_mode;
    int hls_audio_default_only;
} HLSContext;
```

---

## 3. Code Modifications

### A. FFmpeg Demuxer (`libavformat/hls.c`, nicht enthalten, siehe `exteplayer3-build/ffmpeg-hls-native-preselect.patch`)
1. **Preselection Evaluation**: In `hls_read_header()`, right after parsing the master playlist but before any sub-playlists are downloaded:
   - I iterate over `c->variants` to find the best variant matching `hls_quality_mode` (lowest or highest bandwidth).
   - Initialize all playlists as unneeded (`needed = 0`, `broken = 1`).
   - Keep the primary video playlist of the selected variant (`needed = 1`, `broken = 0`).
   - Keep only the `DEFAULT=YES` audio track belonging to the selected variant group (if `hls_audio_default_only` is active).
   - Keep only the subtitle playlist belonging to the selected variant group.
2. **Sub-Playlist Skip**:
   - In the sub-playlist parsing loop, skip unneeded playlists.
   - In the segment selection loop, skip unneeded playlists.
   - In the sub-demuxer allocation loop (`avformat_alloc_context()`), skip unneeded playlists.
   - This prevents any context allocation, download, or stream parsing for unused variants and backup audio renditions.

### B. exteplayer3 playback engine ([container_ffmpeg.c](../exteplayer3-build/src/container/container_ffmpeg.c))
1. **Passing Dictionary Options**: In `container_ffmpeg_init_av_context()`, before opening the input file, these options are written into the `avio_opts` dictionary passed to `avformat_open_input()`:
   ```c
   if (g_hls_quality_mode != 0) {
       char num[16];
       sprintf(num, "%d", g_hls_quality_mode);
       av_dict_set(&avio_opts, "hls_quality_mode", num, 0);
   }
   if (g_hls_audio_default_only) {
       av_dict_set(&avio_opts, "hls_audio_default_only", "1", 0);
   }
   ```
2. **Removed Hack**: Commented out the old `container_ffmpeg_preselect_hls_streams()` function which attempted to overwrite internal struct offsets of the private `struct playlist` in memory after loading.

### C. Enigma2 Plugin ([plugin.py](../serviceapp/src/plugin/plugin.py))
1. **Enabled by Default**: Updated `hls_audio_default_only` to be `True` by default, ensuring immediate performance gains on a fresh installation:
   ```python
   config_serviceapp.exteplayer3[key].hls_audio_default_only = ConfigBoolean(default = True)
   ```

---

## 4. Build and Deployment Pipeline

### GLIBC Compatibility Patching
Because the compiled shared libraries (`libavformat.so.60`, `libavcodec.so.60`, etc.) are built on newer system toolchains, they import newer GLIBC symbols (e.g. `GLIBC_2.38`, `GLIBC_2.35`). To allow them to load on older Enigma2 box images:
- I expanded [patch_glibc_version.py](../exteplayer3-build/patch_glibc_version.py) to map these symbols back to the older version (`GLIBC_2.4` for ARM, `GLIBC_2.0` for MIPS).
- I modified `build-ffmpeg.sh` to run `patch_glibc_version.py` on all compiled `.so` shared libraries before packaging or copying.

### MIPS Cross-Compilation (`mips/`)
The MIPS build uses the shared source code files from `exteplayer3-build/src/` (which includes the modified `container_ffmpeg.c`).
- The HLS preselection patch is automatically applied to the freshly unpacked MIPS FFmpeg source directory during build using:
  ```bash
  patch -p1 -d "ffmpeg-${FFMPEG_VER}" < "${BUILDDIR}/../../exteplayer3-build/ffmpeg-hls-native-preselect.patch"
  ```
- The MIPS GLIBC patcher maps symbols to `GLIBC_2.0` and runs on both the executable and all shared libraries in `ffmpeg-libs/`.
