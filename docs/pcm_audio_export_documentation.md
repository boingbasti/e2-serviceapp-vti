# PCM Audio Export for Third-Party Plugins

This document describes a serviceapp/exteplayer3 feature aimed at third-party plugin developers: an opt-in way to receive already-decoded PCM audio during playback, without opening a second connection to the stream or reimplementing any demuxing/track-selection logic.

## 1. The Problem

A third-party plugin that wants live access to the audio of whatever is currently playing (e.g. speech-to-text live subtitles) traditionally has no clean way to get it. The only realistic option was opening a second, independent connection to the same stream URL and demuxing/decoding it a second time, entirely separately from exteplayer3's own playback. For live IPTV streams in particular, this second connection can cause playback instability (provider connection limits, resource contention on constrained hardware), and reimplementing HLS playlist parsing/track selection/timing on the plugin side is itself a significant, error-prone undertaking.

## 2. The Solution: Opt-in PCM Export via Named Pipe

exteplayer3 can be told (via a new serviceapp setting, default off) to additionally write the PCM audio it already decodes for normal playback into a fixed named pipe (FIFO) at:

```
/tmp/exteplayer3_pcm_audio.fifo
```

Any process that opens this FIFO for reading receives the same audio exteplayer3 is currently playing, correctly demuxed, track-selected, and time-ordered, exactly once, with no second connection to the source involved.

The path is also exported as a constant for plugins that prefer not to hardcode it:

```python
import serviceapp_caps
if getattr(serviceapp_caps, "HAS_PCM_AUDIO_EXPORT", False):
    fifo_path = serviceapp_caps.PCM_AUDIO_EXPORT_FIFO_PATH
```

## 3. Enabling the Feature

Setup → ServiceApp → exteplayer3 options → **"PCM audio export for third-party plugins"**. Off by default.

Enabling it forces software decoding for AAC, AAC_LATM, AC3, EAC3, DTS, WMA and MP3, overriding any individually configured hardware-passthrough settings for the duration of the session. This is necessary and intentional: in the default hardware-passthrough case, exteplayer3 never decodes these formats itself, the compressed packet goes straight to the hardware decoder, so there is no PCM to export at all. Forcing software decoding is the only way to make this feature actually produce data for the audio formats used by most real-world streams. This does mean higher CPU load while the setting is active; only enable it if a plugin you actually use needs it.

## 4. Frame Format

Each PCM chunk written to the FIFO is prefixed with a fixed 16-byte little-endian header:

```c
struct PcmAudioExportHeader
{
    uint32_t magic;           /* 0x504D4341 ("ACMP" as bytes, "PCMA" read as little-endian uint32) */
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample; /* always 16 */
    uint32_t payload_len;     /* size of the PCM payload that immediately follows, in bytes */
};
```

The payload is interleaved signed 16-bit PCM at the given sample rate/channel count. `sample_rate`/`channels` can change between frames (e.g. after a channel/codec change) — readers should not assume they stay constant for the life of the pipe.

### Reading the stream

```python
import struct

HEADER = struct.Struct("<IIHHI")

def read_frames(fifo_fd):
    while True:
        header_bytes = read_exact(fifo_fd, HEADER.size)
        magic, sample_rate, channels, bits, payload_len = HEADER.unpack(header_bytes)
        if magic != 0x504D4341:
            resync(fifo_fd)  # see "Loss and resynchronization" below
            continue
        payload = read_exact(fifo_fd, payload_len)
        yield sample_rate, channels, bits, payload
```

## 5. Loss and Resynchronization (by design, not a bug)

This export is **best-effort and lossy on purpose**. To guarantee normal playback is never affected, exteplayer3 writes to the pipe exclusively via non-blocking syscalls, and never retries or waits:

- If nobody has the FIFO open for reading, frames are silently dropped (checked cheaply via a timestamp comparison, actual reconnect attempts are rate-limited to once every 2 seconds).
- If the pipe buffer is full (reader too slow), the frame is dropped (`EAGAIN`/`EWOULDBLOCK`).
- A PCM frame is often larger than the kernel's atomic pipe-write guarantee (`PIPE_BUF`). A partial write is accepted as-is and never followed by a second write call. A reader that encounters bytes not matching the 16-byte header layout (an implausible `sample_rate`/`channels`/`payload_len`, or a magic mismatch) should scan forward byte-by-byte for the next occurrence of the magic value and resume from there.
- If the reader closes the pipe while exteplayer3 is writing, exteplayer3 detects this (`EPIPE`), closes its end, and attempts to reopen immediately (the 2-second rate limit only applies to *repeated failed* attempts, not to a reconnect following a detected disconnect).

None of this should be treated as a bug report. A plugin relying on this export should be tolerant of occasional gaps and resynchronization, exactly like a real-time monitoring tap.

## 6. Known Limitations

- **One writer, one meaningful reader.** The pipe path is fixed and global. If two exteplayer3 processes are active at once (e.g. a PIP window using a separate `serviceexteplayer3`/`user` profile), both would write into the same pipe and interleave/corrupt each other's frames. This is not solved by design (a per-process path would need per-process discovery on the reader side, which conflicts with the goal of a simple fixed contract) — the feature is intended for the primary playback process.
- **No control channel.** There is currently no way to query current sample rate/channel count except by reading the header of the next frame; a reader must simply be prepared for it to change between frames.

## 7. Implementation Notes (for anyone maintaining this)

### A. exteplayer3 playback engine ([container_ffmpeg.c](../exteplayer3-build/src/container/container_ffmpeg.c))
- `pcm_audio_export_set()`/`pcm_audio_export_get()` control the feature; `pcm_audio_export_set(1)` performs a one-time `mkfifo()` (idempotent) and sets `signal(SIGPIPE, SIG_IGN)`. This is essential: nothing else in the exteplayer3 process ever installs a SIGPIPE handler, so without this, a `write()` to a pipe with no reader would terminate the entire playback process (`EPIPE` → default SIGPIPE disposition).
- `pcm_audio_export_write()` is called right after the existing `Write(context->output->audio->Write, ...)` hardware-output call, using the same already-resampled `output[0]` interleaved-S16 buffer and `pcmExtradata` metadata — no separate decode path, no extra CPU cost beyond what forcing software decoding already implies.
- All I/O in the hot path is either a cheap timestamp comparison or a single guaranteed-non-blocking syscall (`open(..., O_NONBLOCK)` on a FIFO never blocks per POSIX `fifo(7)`; `write()` on a non-blocking fd returns `EAGAIN` instead of blocking). No additional thread, no locks.

### B. exteplayer3 CLI ([exteplayer.c](../exteplayer3-build/src/main/exteplayer.c))
- New flag `-K` (no argument). After the `getopt` loop, if `-K` was seen, the existing `*_software_decoder_set(1)` setters are called for all commonly-passthrough-by-default codecs.

### C. serviceapp ([exteplayer3.h](../serviceapp/src/serviceapp/exteplayer3.h), [exteplayer3.cpp](../serviceapp/src/serviceapp/exteplayer3.cpp), [serviceapp.cpp](../serviceapp/src/serviceapp/serviceapp.cpp))
- `ExtEplayer3Options.pcmAudioExportEnabled` (plain `bool`, deliberately not a configurable path/string — keeps the Python↔C++↔argv chain simple and avoids any escaping/quoting concerns). Set via `exteplayer3_set_setting()`'s extended `PyArg_ParseTuple` format, translated to `-K` in `ExtEplayer3::buildCommand()`.
- No change needed to MIPS's `pre_fault_bss()`: the new field lives inside an already pre-faulted `ExtEplayer3Options` instance (see `mips/MIPS_HEISENBUG.md` for why that pre-fault exists at all). Adding a *new, separate* global flag outside these structs would reintroduce that exact risk class — don't.

### D. Plugin settings ([plugin.py](../serviceapp/src/plugin/plugin.py) / [serviceapp_client.py](../serviceapp/src/plugin/serviceapp_client.py))
- Standard per-profile `ConfigBoolean`, default `False`, one entry per `config_serviceapp.exteplayer3` key (`servicemp3`, `serviceexteplayer3`).

### Build
Shared source for ARM and MIPS (`exteplayer3-build/src/`, no per-architecture fork needed for this feature, unlike `glibc_compat_ffmpeg.c`). `mkfifo`/`open`/`write`/`close`/`signal` are all POSIX-baseline glibc symbols present at `GLIBC_2.0` on both the ARM and MIPS box targets — verified against the real box `libc-2.21.so` via `objdump -T`, no `glibc_version_pin.h`/`patch_glibc_version.py` entry needed beyond what the automatic MIPS patcher already handles for `signal` (`GLIBC_2.34` → `GLIBC_2.0`, confirmed correct for this specific symbol against the real box glibc, unlike the unrelated `exp2`/`log2` case documented elsewhere in this project's history).
