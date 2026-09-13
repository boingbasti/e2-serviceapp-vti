# Third-Party Notices

Dieses Repository baut auf mehreren fremden Open-Source-Projekten auf. Der eigene Code steht unter der [GPL-2.0](LICENSE), wie auch alle übernommenen Bestandteile unten. Diese Datei listet Herkunft und Lizenz der einzelnen Komponenten sowie eine grobe Übersicht der eigenen Änderungen.

## serviceapp (`serviceapp/`)

- **Herkunft:** [mx3L/serviceapp](https://github.com/mx3L/serviceapp)
- **Lizenz:** GPL-2.0 (siehe `serviceapp/README`, `serviceapp/AUTHORS`)
- **Enthält außerdem Codeanteile aus:** OpenPLi-Enigma2 (`serviceapp/src/serviceapp/wrappers.cpp`, `wrappers.h`), ebenfalls GPL-2.0
- **Eigene Änderungen:** Cross-Compile-Anpassungen für VTi/Enigma2 (glibc-Kompatibilität, ABI-Fixes für VTable-Offsets und Event-Enums), diverse Absturz- und Speicherleck-Fixes, HLS-Preselect-Integration, Debug-Logging-Toggle, Automated Picon Sync

## cJSON (`serviceapp/src/serviceapp/cJSON/`)

- **Herkunft:** [cJSON](https://github.com/DaveGamble/cJSON) von Dave Gamble
- **Lizenz:** MIT (siehe `serviceapp/src/serviceapp/cJSON/LICENSE`)
- Unverändert eingebunden.

## exteplayer3 (`exteplayer3-build/src/`)

- **Herkunft:** [skyjet18/exteplayer3](https://github.com/skyjet18/exteplayer3), selbst ein Fork der ursprünglichen e2iplayer/libeplayer3-Codebasis
- **Lizenz:** GPL-2.0-or-later (siehe Lizenzheader in den einzelnen Quelldateien, z. B. `src/main/exteplayer.c`)
- **Eigene Änderungen:** fread-Fix, MOV_TEXT-Untertitel-Unterstützung, X-DRM-Api-Level-Header, HLS-native-Preselect-Integration (`hls_quality_mode`, `hls_audio_default_only`), Netzwerk-Timeout-Erkennung über `AVERROR_EOF`, glibc-Kompatibilitätsschicht für ARM/MIPS

## FFmpeg (Build-Rezept unter `exteplayer3-build/`, `mips/exteplayer3-build/`)

- **Herkunft:** [ffmpeg.org](https://ffmpeg.org/), Version 6.1.1
- **Lizenz:** GPL/LGPL, abhängig von den Compile-Flags (siehe `LICENSE.md`/`COPYING.GPLv2` im offiziellen FFmpeg-Quellpaket, hier nicht mitgeliefert)
- Der FFmpeg-Quellcode selbst ist **nicht** Teil dieses Repositories, nur das eigene Build-Rezept (`build-ffmpeg.sh`, `package-ffmpeg-ipk.sh`) und eigene bzw. übernommene Patches:
- **Eigener Patch:** `exteplayer3-build/ffmpeg-hls-native-preselect.patch` — native HLS-Stream-Vorauswahl direkt im HLS-Demuxer (`libavformat/hls.c`), um unnötige Sub-Playlist-Downloads bei Multi-Bitrate/Multi-Audio-Streams zu vermeiden (Details in `docs/hls_preselection_documentation.md`)
- **Übernommene Patches aus [skyjet18/FFmpeg](https://github.com/skyjet18/FFmpeg)** (Branch `release/6.1-patched`, ebenfalls GPL/LGPL wie Upstream-FFmpeg): `exteplayer3-build/ffmpeg-cenc-drm-support.patch` behebt ein Wiedergabeproblem bei einem speziellen DASH-Anwendungsfall (verzerrtes/vermischtes Bild), inklusive einer verbesserten DASH-Zeitleiste (PTS startet garantiert bei 0)
- **Eigener Patch:** `exteplayer3-build/ffmpeg-eac3-stream-type-fix.patch` — ergänzt den PMT-Stream-Type `0x87` (Enhanced AC-3 nach ATSC-Konvention) in `libavformat/mpegts.c`, der dort bisher fehlte und dadurch bei manchen Sendern zu einer komplett fehlenden Tonspur führte
- **Hinweis:** Der hier dokumentierte Build verwendet OpenSSL (`--enable-openssl --enable-nonfree`) für HTTPS-Unterstützung. Damit gebaute Binaries dürfen laut FFmpeg-Lizenzbedingungen nicht weiterverteilt werden — dieses Repository enthält daher ausschließlich Quellcode/Patches/Build-Skripte, keine fertigen Binaries oder IPK-Pakete.

## zlib

- **Herkunft:** [zlib](https://zlib.net/)
- **Lizenz:** zlib License
- Wird als Systembibliothek für den Cross-Build benötigt (siehe `build-ffmpeg.sh`), ist nicht Teil dieses Repositories.
