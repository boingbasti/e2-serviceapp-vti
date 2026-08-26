# e2-serviceapp-vti

Cross-Build-Umgebung für das Enigma2-Plugin **ServiceApp** samt eigenem **exteplayer3**- und **FFmpeg**-Unterbau, angepasst für VTi-Images (VU+ Uno 4K SE, ARM armv7ahf; VU+ Solo2, MIPS mips32el). ServiceApp bindet Streams (z. B. Zattoo, HLS-Mediatheken) als eigenständige Wiedergabe-Engine in Enigma2 ein.

Dieses Repository enthält ausschließlich Quellcode, eigene Patches und Build-Skripte, **keine fertigen Binaries oder IPK-Pakete**.

## Herkunft & Quellen

- **serviceapp** (`serviceapp/`): Fork von [mx3L/serviceapp](https://github.com/mx3L/serviceapp), enthält zusätzlich Codeanteile aus OpenPLi-Enigma2.
- **exteplayer3** (`exteplayer3-build/src/`): Fork von [skyjet18/exteplayer3](https://github.com/skyjet18/exteplayer3).
- **FFmpeg** (Build-Rezept unter `exteplayer3-build/`, `mips/exteplayer3-build/`): [ffmpeg.org](https://ffmpeg.org/), Version 6.1.1. Der FFmpeg-Quellcode selbst ist nicht Teil dieses Repositories.

Details zu Lizenzen und übernommenen/eigenen Anteilen je Komponente stehen in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Eigene Erweiterungen (Auswahl)

- ABI-Kompatibilitätsfixes für VTi-Enigma2 (VTable-Offsets, Event-Enums, glibc-Symbolversionen auf ARM und MIPS)
- Native HLS-Stream-Vorauswahl im FFmpeg-HLS-Demuxer, um bei Multi-Bitrate-/Multi-Audio-Streams unnötige Sub-Playlist-Downloads zu vermeiden (Details in [docs/hls_preselection_documentation.md](docs/hls_preselection_documentation.md))
- Robustere Bild/Ton-Synchronisation bei HLS-Streams: Cross-Track-AV-Delta-Cache hält Video und Audio auch nach Segment-/Ad-Splice-Wechseln oder längeren Netzwerk-Unterbrechungen synchron
- Diverse Absturz- und Speicherleck-Fixes (u. a. Reentrancy-Guard beim Streamwechsel, EPG-Cache-Absturzsicherheit)
- Optionales Debug-Logging (standardmäßig deaktiviert, um die RAM-Disk `/tmp` nicht vollzuschreiben)
- Automated Picon Sync für Stream-Bouquet-Einträge ohne eigenes Picon
- Wiedergabe-Fortsetzung (Resume) und Sprungmarken für lokale Dateien via `iCueSheet`
- Reale Laufzeit für lokale Dateien ohne Wiedergabe (`getLength()` per FFmpeg-Probe), für Movie-Wall-artige Drittanbieter-Plugins
- Optionaler PCM-Audio-Export für Drittanbieter-Plugins (z. B. Live-Untertitel per Spracherkennung) über eine feste Named Pipe, ohne zweite Verbindung zum Stream (Details in [docs/pcm_audio_export_documentation.md](docs/pcm_audio_export_documentation.md))
- Info-Key für Drittanbieter-Plugins, um zu erkennen, ob eine Wiedergabe gerade über ServiceApp (`exteplayer3`/`gstplayer`) oder über den nativen Player läuft
- MIPS-Portierung inkl. eigenständiger glibc-Kompatibilitätsschicht (siehe [mips/MIPS_HEISENBUG.md](mips/MIPS_HEISENBUG.md), [mips/MIPS_VOD_TIMER_BUG.md](mips/MIPS_VOD_TIMER_BUG.md))

Vollständige Liste aller Fixes mit technischem Hintergrund: [CHANGELOG.md](CHANGELOG.md).

## Voraussetzungen

- Linux-Build-Host (getestet unter Debian)
- Für ARM: Cross-Compiler `gcc-arm-linux-gnueabihf`/`g++-arm-linux-gnueabihf`, `patchelf`
- Für MIPS: Cross-Compiler `gcc-mipsel-linux-gnu`/`g++-mipsel-linux-gnu`
- Ein Sysroot der Zielbox (Header/Libraries von Enigma2 und Python 2.7), von einer laufenden Box bezogen — ist nicht Teil dieses Repositories
- Ein `sigc++-1.2`-Build für die Ziel-Architektur (siehe [Dockerfile](Dockerfile) für ein Beispiel-Setup)

## Build-Anleitung

### ARM (armv7ahf-vfp-neon)

```bash
cd exteplayer3-build
./build-ffmpeg.sh          # FFmpeg-Shared-Libs + Binaries
./build.sh                 # exteplayer3 bauen & als IPK verpacken
./package-ffmpeg-ipk.sh    # FFmpeg-Binary verpacken
cd ..
./build-serviceapp.sh      # serviceapp.so bauen
./package-serviceapp-ipk.sh
```

Ergebnis liegt jeweils in `release/`.

### mips32el (VU+ Solo2)

Analoger Ablauf mit der MIPS-Toolchain, Skripte unter `mips/` bzw. `mips/exteplayer3-build/`:

```bash
cd mips/exteplayer3-build
./build-ffmpeg.sh
./build.sh
./package-ffmpeg-ipk.sh
cd ..
./build-serviceapp.sh
./package-serviceapp-ipk.sh
```

## Lizenz

Eigener Code steht unter der [GNU General Public License v2.0](LICENSE), wie auch alle übernommenen Bestandteile. Details je Komponente in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
