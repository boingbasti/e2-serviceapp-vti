# MIPS Heisenbug — serviceapp set_setting Crash

## Symptom

Nach Installation von `serviceapp.so` auf der VU+ Solo2 (BCM7356, MIPS32r2) startet
Enigma2 in einem Crash-Loop. Der Crash passiert reproduzierbar beim ersten Aufruf von
`serviceapp_set_setting` oder `exteplayer3_set_setting` — genau beim ersten
Schreibzugriff auf eine globale Struct.

Log kurz vor dem Crash (ohne Workaround):
```
[e2-core] [serviceapp_set_setting] setting serviceexteplayer3 options
```
...danach kein weiterer Log, neuer Log-File startet → Crash-Loop alle ~14 Sekunden.

## Heisenbug-Charakter

Das Entfernen der eDebug-Diagnose-Logs bringt den Crash zurück. Das Hinzufügen bringt
ihn zum Verschwinden. Das machte die Diagnose sehr schwer, weil:
- Jede Beobachtung das Verhalten verändert
- Der Code selbst ist korrekt — es liegt an der Laufzeitumgebung


## Was definitiv NICHT hilft (alles getestet)

| Ansatz | Ergebnis |
|--------|----------|
| `-Wl,-z,now` (Eager Binding, alle GOT-Einträge sofort auflösen) | Crash bleibt |
| `__attribute__((noinline))` interne Hilfsfunktionen | Crash bleibt |
| `asm volatile("" ::: "memory")` Compiler Memory-Barrier | Crash bleibt |
| Einzelner eDebug-Call vor dem if-Block | Crash bleibt |
| Einzelner eDebug-Call im if-Block vor dem ersten Store | Crash bleibt |
| 2-3 eDebug-Calls insgesamt | Crash bleibt |

## Was funktioniert (Workaround)

**Ein externer PLT-Call direkt vor JEDEM einzelnen Struct-Feld-Zugriff.**

In der Praxis: eDebug-Calls vor und nach jeder Zuweisung in beiden Funktionen.
Mindestanzahl ca. 7–8 externe Calls pro Funktion. Weniger reicht nicht.

Beispiel `serviceapp_set_setting`:
```cpp
eDebug("[serviceapp_set_setting] set autoTurnOnSubtitles=%d", (int)autoTurnOnSubtitles);
options->autoTurnOnSubtitles = autoTurnOnSubtitles;
eDebug("[serviceapp_set_setting] set HLSExplorer=%d", (int)HLSExplorer);
options->HLSExplorer = HLSExplorer;
// ... usw. für alle 5 Felder
```

Entscheidend ist, dass `eDebug` eine **externe** Funktion aus Enigma2-Core ist — sie geht
über den PLT (Procedure Linkage Table) des dynamischen Linkers. Interne Funktionen
(auch noinline) haben diesen Effekt nicht.


## Hardware / Software

- **Box:** VU+ Solo2
- **SoC:** Broadcom BCM7356
- **CPU-Kern:** MIPS32r2, Big-Endian wird hier als Little-Endian betrieben (mipsel)
- **OS:** VTi 15.0.04 (Linux, BusyBox)
- **Cross-Compiler:** `mipsel-linux-gnu-gcc` (GCC 14, Debian Trixie)
- **Flags:** `-mips32r2 -mhard-float -O2 -fPIC`
- **Python:** 2.7 (Enigma2-Plugin-Interface)
- **ABI:** MIPS O32


## Assembly-Analyse (serviceapp_set_setting, case OPTIONS_SERVICEEXTEPLAYER3)

Der kritische Pfad ohne Workaround:

```asm
; Funktionseinstieg — gp korrekt initialisiert via t9
35d1c: lui  gp,0x4
35d20: addiu gp,gp,-19388
35d24: addu gp,gp,t9        ← gp = GOT-Adresse der eigenen .so

; PyArg_ParseTuple-Call (extern, korrekt)
35d6c: jalr t9
35d78: lw   gp,32(sp)       ← gp restore nach Call

; Switch: options-Pointer laden
35db4: lw   s0,-32720(gp)   ← s0 = Adresse von g_ServiceAppOptionsServiceExt3

; ... mehr Loads ...

; ERSTER STORE — CRASH HIER ohne Workaround
35e68: sb   v0,0(s0)        ← options->autoTurnOnSubtitles = ...
```

Mit Workaround steht zwischen `lw s0,-32720(gp)` und `sb v0,0(s0)` mindestens
ein `jalr t9` (eDebug-Call) pro Store.


## Plausibelste Erklärung

**Page-Fault bei lazy BSS-Allokation kombiniert mit fehlerhaftem Signal-Handler auf MIPS.**

1. Die globalen Structs (`g_ServiceAppOptionsServiceExt3` etc.) liegen in `.bss` der
   `serviceapp.so`. Linux allokiert `.bss`-Seiten lazy (copy-on-write von Zero-Page).

2. Der erste **Schreibzugriff** auf eine nicht-allokierte `.bss`-Seite erzeugt einen
   Page-Fault (SIGSEGV). Normalerweise ist das transparent — der Kernel handhabt es
   und gibt die Kontrolle zurück.

3. Auf BCM7356 / VTi-Kernel scheint Enigma2's SIGSEGV-Handler den MIPS-spezifischen
   `gp`-Register-Kontext beim Wiedereintritt nicht korrekt zu restaurieren. Auf ARM
   gibt es kein äquivalentes globales Pointer-Register, daher tritt das Problem dort
   nicht auf.

4. Externe PLT-Calls (eDebug) lassen den dynamischen Linker PLT-Einträge in das eigene
   Datensegment schreiben — dadurch werden die relevanten Seiten **vor** dem ersten
   eigenen Store physisch allokiert. Der Page-Fault findet dann nicht mehr beim Store
   statt.

Das erklärt:
- Warum `-z,now` nicht hilft: Löst GOT lesend auf, triggert aber nicht den Store-Page-Fault
- Warum interne Calls nicht helfen: Kein PLT-Schreibzugriff auf das eigene Datensegment
- Warum externe Calls helfen: PLT-Resolver schreibt ins eigene Segment, Seiten werden allokiert

**Achtung:** Das ist eine Hypothese — nicht verifiziert. Gegenthesen sind möglich.


## Alternativer sauberer Fix (nicht implementiert)

In der `initserviceapp()`-Funktion (die beim Laden der .so aufgerufen wird) alle
globalen Structs explizit schreiben (z.B. mit `memset` oder Dummy-Zuweisung). Das würde
die Page-Faults beim Laden triggern, bevor Python irgendwelche set_setting-Aufrufe macht.

```cpp
// In initserviceapp(), ganz am Anfang:
// Alle .bss-Seiten vorab allokieren (MIPS BCM7356 Workaround)
volatile char *p = (volatile char *)&g_ServiceAppOptionsServiceExt3;
*p = *p;  // Lese+Schreibe erste Byte → Page-Fault beim Laden, nicht beim Aufruf
// ... für alle globalen Structs
```

Ob das wirklich funktioniert, muss noch getestet werden.


## Getestete Binaries (Referenz)

| MD5 (stripped) | Beschreibung | Ergebnis |
|----------------|-------------|---------|
| `15480b93518b4b341b95226f6f89d7d0` | noinline-Helpers, kein eDebug, -z,now | CRASH |
| `c95074182e8abb4a7c996baffbd11c31` | 3 eDebug gesamt (switch+ptr+summary), kein per-Field | CRASH |
| `183aadbe64b5b11775639dce6795120b` | eDebug vor jedem Feld (7–8 pro Funktion) | OK ✓ |


## Sauberer Fix (implementiert, Commit `0abd314`)

Der "Alternative saubere Fix" von oben wurde umgesetzt und ersetzt den
eDebug-Workaround vollständig: In `initserviceapp()` (Modul-Init, läuft beim
Laden der `.so`) werden jetzt alle globalen Options-Structs einmalig
angefasst, bevor Python irgendwelche `set_setting`-Aufrufe machen kann:

```cpp
#ifdef __mips__
static void pre_fault_bss(void) {
	volatile char *p;
	p = (volatile char *)&g_ExtEplayer3OptionsServiceMP3; *p = *p;
	p = (volatile char *)&g_ExtEplayer3OptionsServiceExt3; *p = *p;
	p = (volatile char *)&g_ExtEplayer3OptionsUser; *p = *p;
	p = (volatile char *)&g_ServiceAppOptionsServiceMP3; *p = *p;
	p = (volatile char *)&g_ServiceAppOptionsServiceExt3; *p = *p;
	p = (volatile char *)&g_ServiceAppOptionsServiceGst; *p = *p;
	p = (volatile char *)&g_ServiceAppOptionsUser; *p = *p;
	p = (volatile char *)&g_GstPlayerOptionsServiceMP3; *p = *p;
	p = (volatile char *)&g_GstPlayerOptionsServiceGst; *p = *p;
	p = (volatile char *)&g_GstPlayerOptionsUser; *p = *p;
}
#endif

PyMODINIT_FUNC
initserviceapp(void)
{
#ifdef __mips__
	pre_fault_bss();
#endif
	Py_InitModule("serviceapp", serviceappMethods);
	...
```

Alle `eDebug`-Aufrufe vor den einzelnen Feld-Zuweisungen in
`exteplayer3_set_setting`/`serviceapp_set_setting` wurden entfernt — nicht
mehr nötig, da die BSS-Seiten bereits beim Laden physisch allokiert sind.

**Bestätigt funktionsfähig** (2026-07-05, deutlich schneller als der
ursprüngliche eDebug-Workaround-Ansatz).

## Stand

- **2026-07-04:** Workaround (eDebug vor jedem Feld) in Produktion, Solo2 lief stabil
- **2026-07-05:** Sauberer Fix (`pre_fault_bss`) implementiert und verifiziert, ersetzt den eDebug-Workaround vollständig (Commit `0abd314`)
- Root Cause (Page-Fault + gp-Register-Restore-Bug) weiterhin nicht abschließend bewiesen, aber der Fix bestätigt die Hypothese indirekt (pre-fault behebt es exakt wie erwartet)
