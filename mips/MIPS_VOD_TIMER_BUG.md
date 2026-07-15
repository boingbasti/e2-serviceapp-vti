# MIPS VOD-Timer-Bug — eTimer feuert nie in serviceapp

## Symptom

Bei VOD-Wiedergabe (Videos mit Anfang/Ende, nicht Livestreams) über `serviceapp`/
`exteplayer3` läuft im Player keine Zeit/kein Fortschrittsbalken. **Bestätigt
MIPS-spezifisch:** identischer Skin auf der ARM-Box (VU+ Uno 4K SE) zeigt die
Zeitanzeige korrekt an, auf der VU+ Solo2 (MIPS) mit identischem Setup nicht.

## Was ausgeschlossen wurde

- **Skin/UI** — ausgeschlossen (User hat identischen Skin auf beiden Boxen getestet).
- **`exteplayer3` selbst** — liefert auf MIPS korrekt `PLAYBACK_LENGTH`/`J`-JSON-Events,
  wenn man es direkt per CLI aufruft (getestet mit
  `https://www.w3schools.com/html/mov_bbb.mp4`).
- **`vfork()`/`pipe()`/`poll()`/`read()` grundsätzlich** — mit einem isolierten
  Test-Programm (identisches Muster wie `bidirpipe()` in `myconsole.cpp`) auf der
  Solo2 verifiziert: Kind-Prozess-Ausgabe kam zuverlässig über Pipe+Poll beim
  Parent an (10/10 Zeilen empfangen). Der Mechanismus als solcher funktioniert
  auf diesem MIPS-Toolchain+Kernel einwandfrei.
- **Größe des Mutex-Puffers** — `sizeof(pthread_mutex_t)=24`, `alignof=4` auf MIPS,
  passt exakt zum vorhandenen `_vtis_ref_mutex[24]`-Stub-Puffer aus den früheren
  ABI-Fixes (siehe unten). Der Puffer ist also groß genug.

## Kernbefund (per Live-Test bestätigt)

Bei echter Wiedergabe (magentamusik.de VOD-Stream) auf der Solo2 zeigt
`/tmp/serviceapp.log`, dass nach

```
[serviceapp] eConsoleContainer::execute: timer started, returning
[serviceapp] console->execute returned 0
```

**rein gar nichts mehr passiert** — keine einzige `exteplayer3 stderr: ...`-Zeile
(die auf der ARM-Box sofort und durchgehend erscheinen). `exteplayer3` läuft
nachweislich (Hardware-Ausgabe aktiv, per `ps` bestätigt), aber die
stdout/stderr-Pipe-Ausgabe wird auf MIPS nie eingesammelt.

Zur Bestätigung wurde direkt in `eConsoleContainer::pollPipes()` (`myconsole.cpp`)
eine unbedingte Diagnose-Logzeile eingebaut (loggt bei **jedem** Aufruf, nicht nur
bei Dateneingang):

```cpp
void eConsoleContainer::pollPipes()
{
    static int dbg_calls = 0;
    dbg_calls++;
    if (dbg_calls <= 5 || dbg_calls % 100 == 0)
        SALOG("DIAG pollPipes: call #%d killstate=%d pid=%d fd0=%d fd1=%d fd2=%d",
              dbg_calls, killstate, pid, fd[0], fd[1], fd[2]);
    ...
    int n = ::poll(pfd, 3, 0);
    if (dbg_calls <= 5 || dbg_calls % 100 == 0)
        SALOG("DIAG pollPipes: call #%d poll()=%d errno=%d revents0=%d revents2=%d",
              dbg_calls, n, errno, pfd[0].revents, pfd[2].revents);
    ...
}
```

**Ergebnis: Selbst diese Diagnose-Zeile erscheint nie im Log.** `pollPipes()`
wird also nicht ein einziges Mal aufgerufen — der `pollTimer` (ein `eTimer`,
per `eTimer::create()` + `initTimerMutex()` + `AddRef()` + `start(20, false)`
initialisiert, exakt wie alle anderen Timer im Code) feuert nie.

Diese Diagnose-Instrumentierung ist **nicht** im Git-Repo committed (nur lokal
im Working Tree getestet und danach wieder entfernt) — bei Bedarf oben erneut
einbauen.

## Hypothese (nicht abschließend verifiziert)

Der in der ABI-Fix-Session dokumentierte, hartkodierte Mutex-Offset `+8` in
`initTimerMutex()`:

```cpp
inline void initTimerMutex(eTimer *timer) {
    if (timer) pthread_mutex_init((pthread_mutex_t*)((char*)timer + 8), NULL);
}
```

wurde **per Trial-and-Error speziell auf der ARM-Box verifiziert**, nicht aus
der pthread-Mutex-Größe/Ausrichtung selbst hergeleitet — die bräuchte nur
4-Byte-Alignment und würde nach einem 4-Byte-`ref`-Feld gar keine Lücke bis
Offset 8 erzwingen. Der Offset kommt vermutlich vom tatsächlichen
Speicherlayout, das VTis kompiliertes `enigma2`-Kern-Binary für die jeweilige
Architektur nutzt, nicht aus abstrakten ABI-Regeln.

**Das von VTi kompilierte `enigma2`-Kernbinary für vusolo2 (MIPS) wurde separat
gebaut** (eigener Compiler-Lauf, evtl. andere GCC-Version/Padding-Regeln als
beim ARM-Build) — der reale Mutex-Offset in `eTimer` könnte daher auf MIPS ein
anderer sein als `+8`. Wird der Mutex an der falschen Speicherstelle
initialisiert, landet `pthread_mutex_init()` auf Garbage-Speicher (kein Crash,
da innerhalb des 256-Byte-Zero-Puffers aus `eTimer::create()`), während der
echte, vom Enigma2-Mainloop intern für die Scheduling-Logik genutzte Mutex
uninitialisiert/inkonsistent bleibt. Das würde exakt erklären, warum die
Konstruktion "erfolgreich" durchläuft (keine SALOG-Fehler, keine Crashes), der
Timer aber nie feuert.

**Wichtig:** Dies ist eine Hypothese, kein abschließend bewiesener Fund — es
wurde noch nicht verifiziert, welcher Offset auf MIPS tatsächlich richtig wäre.

## Bezug zu den ABI-Fixes (ARM-Session)

Siehe Commit `02146cc` und die zugehörigen Notizen für den vollen Kontext, wie
`+8` für ARM gefunden wurde (RC 1–4: `DECLARE_REF`-Mutex-Stub, `timespec`-Größe,
`eSmartPtrList m_clients`, `initTimerMutex`+`AddRef`). Alle diese Werte wurden
nur auf ARM verifiziert (`sizeof_eTimer=72` stimmt zwar auch auf MIPS, das
allein beweist aber nicht, dass ALLE internen Feld-Offsets identisch sind).

## Nächste Schritte

1. Den echten Mutex-Offset für MIPS empirisch reverse-engineeren — analog zum
   Vorgehen, das für ARM die "5 ABI-Fixes" hervorgebracht hat (vermutlich
   Trial-and-Error mit verschiedenen Offsets + Beobachten, ob
   `DIAG pollPipes`-Zeilen erscheinen).
2. Alternativ: einen Weg finden, den echten Struct-Layout des kompilierten
   MIPS-`enigma2`-Binarys zu inspizieren (Debug-Symbole falls vorhanden, oder
   systematisches Speicher-Dumping eines echten `eTimer`-Objekts zur Laufzeit).
3. Betrifft vermutlich **nicht nur** `pollTimer`, sondern alle `eTimer`-
   Instanzen in serviceapp auf MIPS — also auch `my_subtitle_sync_timer`,
   `my_event_updated_info_timer` und `PlayerBackend::myTimer`
   (Positions-Update-Timer). Bisher aber nicht einzeln verifiziert, ob
   wirklich alle betroffen sind oder nur bestimmte.

## Hardware / Software

- **Box:** VU+ Solo2
- **SoC:** Broadcom BCM7356
- **CPU-Kern:** MIPS32r1 (BMIPS5000), mipsel
- **OS:** VTi 15.0.04
- **Cross-Compiler:** `mipsel-linux-gnu-gcc` (GCC 14, Debian Trixie)
- **Vergleichsbox:** VU+ Uno 4K SE (ARM), identischer
  Skin, Zeitanzeige funktioniert dort einwandfrei

## Fix (implementiert, Commit `0abd314`)

Bestätigt sich die Hypothese oben im Kern, aber die Lösung ist einfacher als
"den richtigen Offset finden": `pthread_mutex_init()` wird auf MIPS einfach
**gar nicht mehr aufgerufen**, statt einen anderen Offset zu suchen:

```cpp
inline void initTimerMutex(eTimer *timer) {
#ifndef __mips__
    if (timer) {
        pthread_mutex_init((pthread_mutex_t*)((char*)timer + 8), NULL);
    }
#endif
}
```

**Warum das funktioniert:** `eTimer::create()` liefert laut den ABI-Fix-Notizen
einen 256-Byte **zero-initialisierten** Puffer. Ein komplett genulltes
`pthread_mutex_t` entspricht bei glibc/NPTL exakt `PTHREAD_MUTEX_INITIALIZER`
(die Default-Initialisierung ist bei NPTL bitweise Null) — der Mutex ist also
bereits gültig, OHNE dass er explizit initialisiert werden muss. Der eigene
manuelle `pthread_mutex_init()`-Aufruf am geratenen Offset `+8` war auf MIPS
vermutlich selbst das Problem (schrieb an der falschen/benachbarten Stelle im
Objekt und hat dadurch andere Felder — vermutlich genau die für die
Timer-Scheduling-Logik relevanten — korrumpiert), nicht eine fehlende
Initialisierung.

**Bestätigt funktionsfähig** (2026-07-05).

## Stand

- **2026-07-05:** Root Cause eingegrenzt (Timer feuerte nie) und behoben
  (Commit `0abd314`) — `pthread_mutex_init()` wird auf MIPS in
  `initTimerMutex()` komplett übersprungen, der bereits genullte Speicher
  dient als gültiger Default-Mutex. ARM bleibt unverändert (ruft
  `pthread_mutex_init()` weiterhin wie bisher auf).
