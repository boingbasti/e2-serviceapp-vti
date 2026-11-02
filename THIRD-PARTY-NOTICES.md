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

## FFmpeg (Build-Rezept unter `exteplayer3-build/`, `mips/exteplayer3-build/`, `vti16/exteplayer3-build/`)

- **Herkunft:** [ffmpeg.org](https://ffmpeg.org/), Version 6.1.1 (VTi 15 ARM/MIPS) bzw. 7.0.2 (VTi 16, siehe CHANGELOG-Abschnitt „VTi 16" für den Hintergrund zur Versionsumstellung)
- **Lizenz:** GPL/LGPL, abhängig von den Compile-Flags (siehe `LICENSE.md`/`COPYING.GPLv2` im offiziellen FFmpeg-Quellpaket, hier nicht mitgeliefert)
- Der FFmpeg-Quellcode selbst ist **nicht** Teil dieses Repositories, nur das eigene Build-Rezept (`build-ffmpeg.sh`, `package-ffmpeg-ipk.sh`) und eigene bzw. übernommene Patches:
- **Eigener Patch:** `exteplayer3-build/ffmpeg-hls-native-preselect.patch` — native HLS-Stream-Vorauswahl direkt im HLS-Demuxer (`libavformat/hls.c`), um unnötige Sub-Playlist-Downloads bei Multi-Bitrate/Multi-Audio-Streams zu vermeiden (Details in `docs/hls_preselection_documentation.md`)
- **Übernommene Patches aus [skyjet18/FFmpeg](https://github.com/skyjet18/FFmpeg)** (Branch `release/6.1-patched`, ebenfalls GPL/LGPL wie Upstream-FFmpeg): `exteplayer3-build/ffmpeg-cenc-drm-support.patch` behebt ein Wiedergabeproblem bei einem speziellen DASH-Anwendungsfall (verzerrtes/vermischtes Bild), inklusive einer verbesserten DASH-Zeitleiste (PTS startet garantiert bei 0)
- **Eigener Patch:** `exteplayer3-build/ffmpeg-eac3-stream-type-fix.patch` — ergänzt den PMT-Stream-Type `0x87` (Enhanced AC-3 nach ATSC-Konvention) in `libavformat/mpegts.c`, der dort bisher fehlte und dadurch bei manchen Sendern zu einer komplett fehlenden Tonspur führte
- **Eigener Patch, teils auf offiziellen FFmpeg-Upstream-Commits basierend:** `exteplayer3-build/ffmpeg-dash-avsync-fix.patch` — behebt Bild/Ton-Versatz bei Live-DASH-Sendern in `libavformat/dashdec.c`: ein eigener, stream-übergreifender PTS-Nullpunkt statt eines eigenen pro Representation, sowie zwei bereits offiziell in FFmpeg upstream gemergte Fixes (Segmentnummer-Berechnung berücksichtigt jetzt den Playlist-Startindex)
- **Eigener Patch:** `exteplayer3-build/ffmpeg-dash-live-edge-delay-fix.patch` — hält bei Live-DASH-Streams einen dauerhaften Sicherheitsabstand zur Live-Kante ein (`libavformat/dashdec.c`) und ergänzt bei fehlendem `suggestedPresentationDelay` einen Fallback-Abstand nach DASH-IF-IOP 4.3.4.4, sowie einen kurzen Wiederholungsversuch bei einem verfrüht angefragten Segment
- **Eigener Patch:** `exteplayer3-build/ffmpeg-dash-subtitle-starvation-fix.patch` — verwirft eine neu angelegte DASH-Untertitelspur (`libavformat/dashdec.c`) standardmäßig, statt sie erst nach erfolgreicher Codec-Probe zu bewerten, damit eine praktisch nie befüllte Spur nicht dauerhaft Video/Audio blockiert
- **Eigener Patch:** `exteplayer3-build/ffmpeg-dash-short-buffer-startup-fix.patch` — berechnet die Live-Startposition für `<SegmentTimeline>`-Manifeste (`libavformat/dashdec.c`) direkt in Segment-Anzahl statt in Sekunden umzurechnen, damit Video- und Audio-Spuren bei krummen Segmentdauern garantiert denselben Startpunkt bekommen, inklusive Mindestabstand sowohl zur Live-Kante als auch zur Verfallsgrenze bei kurzen Live-Puffern
- **Eigener Patch:** `exteplayer3-build/ffmpeg-dash-header-parsing-manifest-freeze-fix.patch` — friert das DASH-Manifest (`libavformat/dashdec.c`) während der Komponenten-Initialisierung beim Öffnen eines Streams ein, damit bei mehreren Repräsentationen (Video/Audio/Untertitel) nicht durch einen zwischenzeitlich veröffentlichten neuen Server-Snapshot unterschiedliche Startsegmente pro Spur berechnet werden
- **Übernommene Patches aus [skyjet18/FFmpeg](https://github.com/skyjet18/FFmpeg)** (Version 183): `exteplayer3-build/ffmpeg-skyjet183-upstream-sync.patch` — XML-Maskierung von Kaufmanns-Und in DASH-BaseURLs, Korrektur der Grenze zwischen ablaufenden und noch gültigen Segmenten, überlaufsicherere Segmentdauer-Berechnung, Unterstützung für HLS-EVENT-Playlists (laufende Übertragungen mit Spulen im bisherigen Verlauf) sowie die Dateiendung `.webvtt` zusätzlich zu `.vtt`
- **Eigener Patch:** `exteplayer3-build/ffmpeg-dash-http-options.patch` — unterdrückt bei eigenständigen DASH-Segmentdateien (`libavformat/dashdec.c`) einen von FFmpeg sonst unaufgefordert mitgeschickten HTTP-Range-Header, der für einen einfachen Komplett-Download unnötig ist
- **Eigener Patch:** `exteplayer3-build/ffmpeg-hls-sequence-wrap-resync-fix.patch` (FFmpeg 7.0.2) / `exteplayer3-build/ffmpeg-hls-sequence-wrap-resync-fix-6.1.1.patch` (FFmpeg 6.1.1) — resynct den HLS-Demuxer (`libavformat/hls.c`) bei einem server-seitigen Media-Sequence-Reset nach einem Werbeblock auf die tatsächliche Live-Kante mit kleinem Puffer-Vorlauf, statt bis zum Reload-Limit auf ein nie veröffentlichtes Segment zu warten
- **Eigener Patch:** `exteplayer3-build/ffmpeg-hls-discontinuity-starvation-fix.patch` — behebt in `libavformat/hls.c` zwei Ursachen für ein dauerhaftes Standbild bei HLS-Werbeübergängen mit getrennten Audio-/Video-Renditions: einen Zeitstempel-Vergleich, der eine zurückhängende Spur fälschlich für "in der Zukunft" hält und nie wieder auswählt, sowie eine rein indexbasierte Stream-Zuordnung, die bei wechselnder interner Reihenfolge einzelner Werbeclips das Video an den falschen Slot bindet; zusätzlich ein Sicherheitsnetz gegen jeden verbleibenden Randfall sowie optionales, per Umgebungsvariable schaltbares Diagnose-Logging für die eigene Fehlersuche
- **Hinweis:** Der hier dokumentierte Build verwendet OpenSSL (`--enable-openssl --enable-nonfree`) für HTTPS-Unterstützung. Damit gebaute Binaries dürfen laut FFmpeg-Lizenzbedingungen nicht weiterverteilt werden — dieses Repository enthält daher ausschließlich Quellcode/Patches/Build-Skripte, keine fertigen Binaries oder IPK-Pakete.

## zlib

- **Herkunft:** [zlib](https://zlib.net/)
- **Lizenz:** zlib License
- Wird als Systembibliothek für den Cross-Build benötigt (siehe `build-ffmpeg.sh`), ist nicht Teil dieses Repositories.

## libxml2

- **Herkunft:** [libxml2](https://gitlab.gnome.org/GNOME/libxml2)
- **Lizenz:** MIT
- Wird für den FFmpeg-`dash`-Demuxer benötigt. Nicht mitgeliefert, sondern zur Laufzeit gegen die bereits im VTi-Feed vorhandene Version gelinkt (als `Depends` eingetragen), ist nicht Teil dieses Repositories.

## libdav1d (VTi 16)

- **Herkunft:** [dav1d](https://code.videolan.org/videolan/dav1d)
- **Lizenz:** BSD-2-Clause
- Nur für den VTi-16-Build (`--enable-libdav1d`), für AVIF-Dekodierung. Nicht mitgeliefert, sondern zur Laufzeit gegen die bereits im VTi-16-Image vorhandene Version (`libdav1d.so.7`) gelinkt, ist nicht Teil dieses Repositories.
