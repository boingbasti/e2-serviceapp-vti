import os
import subprocess
import shutil

builddir = os.path.dirname(os.path.abspath(__file__))
src_dir = os.path.join(builddir, "ffmpeg-src")

# 1. Clean up old source
shutil.rmtree(os.path.join(src_dir, "ffmpeg-6.1.1"), ignore_errors=True)

# 2. Extract ffmpeg
print("Extracting FFmpeg...")
subprocess.run(["tar", "xf", "ffmpeg-6.1.1.tar.xz"], cwd=src_dir, check=True)

# 3. Read original hls.c
hls_c_path = os.path.join(src_dir, "ffmpeg-6.1.1/libavformat/hls.c")
with open(hls_c_path, "r") as f:
    hls_content = f.read()

# 4. Modify hls.c
# I want to insert my fields into HLSContext
target_ctx = "    AVIOContext *playlist_pb;\n    HLSCryptoContext  crypto_ctx;\n} HLSContext;"
replacement_ctx = "    AVIOContext *playlist_pb;\n    HLSCryptoContext  crypto_ctx;\n    int hls_quality_mode;\n    int hls_audio_default_only;\n} HLSContext;"
assert target_ctx in hls_content, "HLSContext target not found"
hls_content = hls_content.replace(target_ctx, replacement_ctx)

# I want to insert the preselection logic inside hls_read_header
# In ffmpeg 6.1.1, hls_read_header starts with:
# static int hls_read_header(AVFormatContext *s)
# I want to find:
#     if (c->n_variants == 0) {
#         av_log(s, AV_LOG_WARNING, "Empty playlist\n");
#         return AVERROR_EOF;
#     }
# And insert my preselection logic after it!
target_header = """    if (c->n_variants == 0) {
        av_log(s, AV_LOG_WARNING, "Empty playlist\\n");
        return AVERROR_EOF;
    }"""
assert target_header in hls_content, "target_header not found"

# Insert best_variant_idx definition at the beginning of hls_read_header
# static int hls_read_header(AVFormatContext *s)
# {
#     HLSContext *c = s->priv_data;
#     int ret = 0, i;
#     int64_t highest_cur_seq_no = 0;
target_vars = """    HLSContext *c = s->priv_data;
    int ret = 0, i;
    int64_t highest_cur_seq_no = 0;"""
assert target_vars in hls_content, "target_vars not found"
hls_content = hls_content.replace(target_vars, target_vars + "\n\n    int best_variant_idx = 0;")

# Preselection code
preselect_code = """

    /* HLS Preselection Logic */
    if (c->hls_quality_mode != 0) {
        int best_bandwidth = -1;
        int idx;
        for (idx = 0; idx < c->n_variants; idx++) {
            struct variant *v = c->variants[idx];
            int bandwidth = v->bandwidth;
            if (best_bandwidth == -1 ||
                (c->hls_quality_mode == 2 && bandwidth > best_bandwidth) ||
                (c->hls_quality_mode == 1 && bandwidth < best_bandwidth)) {
                best_variant_idx = idx;
                best_bandwidth = bandwidth;
            }
        }
        
        av_log(s, AV_LOG_INFO, "[HLS native preselect] selected variant index %d (bandwidth=%d)\\n",
               best_variant_idx, best_bandwidth);

        /* Discard unselected video variant playlists */
        for (idx = 0; idx < c->n_variants; idx++) {
            if (idx != best_variant_idx) {
                struct variant *v = c->variants[idx];
                if (v->n_playlists > 0 && v->playlists[0] != NULL) {
                    v->playlists[0]->needed = 0;
                    v->playlists[0]->broken = 1;
                }
            }
        }

        struct variant *best_var = c->variants[best_variant_idx];

        /* Keep the selected variant's own playlist -- without this, needed
           stays at its zero-initialized default and the "Open the demuxer"
           loop below skips it too, so no playlist ever gets pls->ctx
           allocated and hls_read_packet segfaults on a NULL context. */
        if (best_var->n_playlists > 0 && best_var->playlists[0] != NULL) {
            best_var->playlists[0]->needed = 1;
            best_var->playlists[0]->broken = 0;
        }

        /* Process audio renditions */
        int best_audio_pls_count = 0;
        struct playlist *audio_playlists[128];
        int audio_is_default[128];
        int has_any_default_audio = 0;

        if (best_var->audio_group && best_var->audio_group[0] != '\\0') {
            for (idx = 0; idx < c->n_renditions; idx++) {
                struct rendition *rend = c->renditions[idx];
                if (rend->type == AVMEDIA_TYPE_AUDIO) {
                    if (strcmp(rend->group_id, best_var->audio_group) == 0) {
                        if (rend->playlist != NULL && best_audio_pls_count < 128) {
                            audio_playlists[best_audio_pls_count] = rend->playlist;
                            audio_is_default[best_audio_pls_count] = (rend->disposition & AV_DISPOSITION_DEFAULT) ? 1 : 0;
                            if (audio_is_default[best_audio_pls_count]) {
                                has_any_default_audio = 1;
                            }
                            best_audio_pls_count++;
                        }
                    } else {
                        if (rend->playlist != NULL) {
                            rend->playlist->needed = 0;
                            rend->playlist->broken = 1;
                        }
                    }
                }
            }

            for (idx = 0; idx < best_audio_pls_count; idx++) {
                int keep = 0;
                if (c->hls_audio_default_only) {
                    if (audio_is_default[idx] || !has_any_default_audio) {
                        keep = 1;
                    }
                } else {
                    keep = 1;
                }

                if (!keep) {
                    audio_playlists[idx]->needed = 0;
                    audio_playlists[idx]->broken = 1;
                    av_log(s, AV_LOG_INFO, "[HLS native preselect] discarding non-default audio rendition playlist %s\\n",
                           audio_playlists[idx]->url);
                } else {
                    audio_playlists[idx]->needed = 1;
                    audio_playlists[idx]->broken = 0;
                    av_log(s, AV_LOG_INFO, "[HLS native preselect] keeping audio rendition playlist %s (default=%d)\\n",
                           audio_playlists[idx]->url, audio_is_default[idx]);
                }
            }
        }

        /* Process subtitle renditions */
        if (best_var->subtitles_group && best_var->subtitles_group[0] != '\\0') {
            for (idx = 0; idx < c->n_renditions; idx++) {
                struct rendition *rend = c->renditions[idx];
                if (rend->type == AVMEDIA_TYPE_SUBTITLE) {
                    if (strcmp(rend->group_id, best_var->subtitles_group) != 0) {
                        if (rend->playlist != NULL) {
                            rend->playlist->needed = 0;
                            rend->playlist->broken = 1;
                        }
                    } else {
                        if (rend->playlist != NULL) {
                            rend->playlist->needed = 1;
                            rend->playlist->broken = 0;
                        }
                    }
                }
            }
        }
    }"""

hls_content = hls_content.replace(target_header, target_header + preselect_code)

# Now I need to modify the playlist loop
#         for (i = 0; i < c->n_playlists; i++) {
#             struct playlist *pls = c->playlists[i];
#             pls->m3u8_hold_counters = 0;
target_loop = """        for (i = 0; i < c->n_playlists; i++) {
            struct playlist *pls = c->playlists[i];
            pls->m3u8_hold_counters = 0;"""
replacement_loop = """        for (i = 0; i < c->n_playlists; i++) {
            struct playlist *pls = c->playlists[i];
            if (c->hls_quality_mode != 0 && (pls->broken || !pls->needed)) {
                continue;
            }
            pls->m3u8_hold_counters = 0;"""
assert target_loop in hls_content, "target_loop not found"
hls_content = hls_content.replace(target_loop, replacement_loop)

# Modify variant check loop
#     for (i = 0; i < c->n_variants; i++) {
#         if (c->variants[i]->playlists[0]->n_segments == 0) {
target_var_loop = """    for (i = 0; i < c->n_variants; i++) {
        if (c->variants[i]->playlists[0]->n_segments == 0) {"""
replacement_var_loop = """    for (i = 0; i < c->n_variants; i++) {
        if (c->hls_quality_mode != 0 && (!c->variants[i]->playlists[0]->needed || c->variants[i]->playlists[0]->broken)) {
            continue;
        }
        if (c->variants[i]->playlists[0]->n_segments == 0) {"""
assert target_var_loop in hls_content, "target_var_loop not found"
hls_content = hls_content.replace(target_var_loop, replacement_var_loop)

# Modify duration calculation
#     if (c->variants[0]->playlists[0]->finished) {
#         int64_t duration = 0;
#         for (i = 0; i < c->variants[0]->playlists[0]->n_segments; i++)
#             duration += c->variants[0]->playlists[0]->segments[i]->duration;
target_duration = """    if (c->variants[0]->playlists[0]->finished) {
        int64_t duration = 0;
        for (i = 0; i < c->variants[0]->playlists[0]->n_segments; i++)
            duration += c->variants[0]->playlists[0]->segments[i]->duration;"""
replacement_duration = """    if (c->variants[best_variant_idx]->playlists[0]->finished) {
        int64_t duration = 0;
        for (i = 0; i < c->variants[best_variant_idx]->playlists[0]->n_segments; i++)
            duration += c->variants[best_variant_idx]->playlists[0]->segments[i]->duration;"""
assert target_duration in hls_content, "target_duration not found"
hls_content = hls_content.replace(target_duration, replacement_duration)

# Modify starting segments loop
#     for (i = 0; i < c->n_playlists; i++) {
#         struct playlist *pls = c->playlists[i];
# 
#         if (pls->n_segments == 0)
target_start_seg = """    for (i = 0; i < c->n_playlists; i++) {
        struct playlist *pls = c->playlists[i];

        if (pls->n_segments == 0)"""
replacement_start_seg = """    for (i = 0; i < c->n_playlists; i++) {
        struct playlist *pls = c->playlists[i];
        if (c->hls_quality_mode != 0 && (pls->broken || !pls->needed)) {
            continue;
        }

        if (pls->n_segments == 0)"""
assert target_start_seg in hls_content, "target_start_seg not found"
hls_content = hls_content.replace(target_start_seg, replacement_start_seg)

# Modify playlist alloc loop
#         AVDictionary *options = NULL;
#         struct segment *seg = NULL;
# 
#         if (!(pls->ctx = avformat_alloc_context()))
target_alloc = """        AVDictionary *options = NULL;
        struct segment *seg = NULL;

        if (!(pls->ctx = avformat_alloc_context()))"""
replacement_alloc = """        AVDictionary *options = NULL;
        struct segment *seg = NULL;

        if (c->hls_quality_mode != 0 && (pls->broken || !pls->needed)) {
            continue;
        }

        if (!(pls->ctx = avformat_alloc_context()))"""
assert target_alloc in hls_content, "target_alloc not found"
hls_content = hls_content.replace(target_alloc, replacement_alloc)

# Fix: st->id = pls->index (set later in update_streams_from_subdemuxer) collides
# between video and audio when a muxed-audio HLS variant's raw position in
# c->playlists[] is non-zero (e.g. the highest-bandwidth variant of a 5-variant
# master lands at index 4). exteplayer3's own container_ffmpeg.c only assigns a
# distinguishing fallback id when the upstream id is falsy (0), so id=4 for both
# video and audio means every audio packet gets routed into the video path and
# is silently dropped -- picture plays, sound doesn't. When my preselect is
# active, at most one video-variant playlist ever reaches this point, so forcing
# index 0 here is always safe and puts it on the same already-correct id=0 path
# that "auto" (first variant) already used successfully.
target_index = """        pls->index  = i;
        pls->needed = 1;
        pls->parent = s;"""
replacement_index = """        pls->index  = (c->hls_quality_mode != 0) ? 0 : i;
        pls->needed = 1;
        pls->parent = s;"""
assert target_index in hls_content, "target_index not found"
hls_content = hls_content.replace(target_index, replacement_index)

# Add options
#     {"seg_max_retry", "Maximum number of times to reload a segment on error.",
#      OFFSET(seg_max_retry), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
#     {NULL}
target_options = """    {"seg_max_retry", "Maximum number of times to reload a segment on error.",
     OFFSET(seg_max_retry), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    {NULL}"""
replacement_options = """    {"seg_max_retry", "Maximum number of times to reload a segment on error.",
     OFFSET(seg_max_retry), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    {"hls_quality_mode", "HLS start quality mode (0=auto, 1=lowest, 2=highest)",
     OFFSET(hls_quality_mode), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 2, FLAGS},
    {"hls_audio_default_only", "HLS audio default-only filter",
     OFFSET(hls_audio_default_only), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, FLAGS},
    {NULL}"""
assert target_options in hls_content, "target_options not found"
hls_content = hls_content.replace(target_options, replacement_options)

# Safety Net: Mark all unallocated/closed playlists as broken and not needed at the end of hls_read_header
target_end = "    update_noheader_flag(s);\n\n    return 0;\n}"
replacement_end = """    if (c->hls_quality_mode != 0) {
        for (i = 0; i < c->n_playlists; i++) {
            struct playlist *pls = c->playlists[i];
            if (!pls->ctx) {
                pls->needed = 0;
                pls->broken = 1;
            }
        }
    }

    update_noheader_flag(s);

    return 0;
}"""
assert target_end in hls_content, "target_end not found"
hls_content = hls_content.replace(target_end, replacement_end)

# Write modified hls.c
with open(hls_c_path, "w") as f:
    f.write(hls_content)

print("hls.c modified successfully!")

# 5. Generate patch using diff -u
print("Generating patch...")
original_hls_path = "ffmpeg-6.1.1/libavformat/hls.c"
# To do a clean diff -u against the original, extract it again to a temp location
shutil.rmtree(os.path.join(src_dir, "ffmpeg-temp"), ignore_errors=True)
os.makedirs(os.path.join(src_dir, "ffmpeg-temp"))
subprocess.run(["tar", "xf", "../ffmpeg-6.1.1.tar.xz"], cwd=os.path.join(src_dir, "ffmpeg-temp"), check=True)

# Run diff -u between the original hls.c in ffmpeg-temp and the modified one in ffmpeg-6.1.1
res = subprocess.run([
    "diff", "-u", 
    "ffmpeg-temp/ffmpeg-6.1.1/libavformat/hls.c", 
    "ffmpeg-6.1.1/libavformat/hls.c"
], cwd=src_dir, capture_output=True, text=True)

# Write the generated patch to ffmpeg-hls-native-preselect.patch
patch_content = res.stdout
# Clean up paths in patch headers to match expected format
patch_content = patch_content.replace("ffmpeg-temp/ffmpeg-6.1.1/libavformat/hls.c", "ffmpeg-6.1.1/libavformat/hls.c")

patch_file_path = os.path.join(builddir, "ffmpeg-hls-native-preselect.patch")
with open(patch_file_path, "w") as f:
    f.write(patch_content)

# Clean up temp
shutil.rmtree(os.path.join(src_dir, "ffmpeg-temp"), ignore_errors=True)
shutil.rmtree(os.path.join(src_dir, "ffmpeg-6.1.1"), ignore_errors=True)

print("Patch generated and saved successfully!")
