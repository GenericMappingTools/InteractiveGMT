# mbprocess — processing swath data

MB-System's **mbprocess** turns a raw swath file into its processed copy (`survey.mb88` →
`surveyp.mb88`): it applies the beam flags saved by the editors, merges navigation, recalculates
bathymetry, corrects draft, roll and pitch, and more. What it does to a file is not given on its
command line. It is read from that file's **parameter file**, `survey.mb88.par`, which MB-System's
**mbset** writes (and which mbedit, mbeditviz and CUBE update as they save their edits).

The dialog runs both programs in iGMT, through GMT's MB-System supplement, so no terminal is needed:
it shows what a file's `.par` says, writes the changes made in the dialog back into it with mbset,
and runs mbprocess on every file, its report in the dialog's log.

| | |
|---|---|
| Menu | **Geophysics ▸ MB-System ▸ Process swath data (mbprocess)** |
| Julia | `mbprocess_dialog(file = "")` (`src/mbprocess.jl`) |
| C++ | `deps/src/72_mbprocess.cpp`, `deps/ui/mbprocess.ui` |
| Needs | GMT with the MB-System supplement (Windows: comes with GMT; Linux / macOS: [MB-System plugin](77-mbplugin.md)) |
| Tests | `test/test-mbprocess-gui.jl` (`:gui`) |

---

## The dialog

**Swath file or datalist.** One raw swath file, or a datalist (`*.mb-1`) of them. A datalist is read
by MBIO itself, nested datalists included, and **Files** lists every file it holds. *Format* is the
MBIO format id; left empty it is taken from the file name (`*.mbXX`), and `-1` means a datalist.

- **Process even if up to date** (`-P`): mbprocess normally skips a file whose output is newer than
  the file, its `.par` and the files the `.par` names.
- **Strip comments** (`-N`): leave the comment records out of the processed file.
- **Verbose** (`-V`): mbprocess's verbose report, in the log.

**Edits tab.** *Apply the bathymetry edits* switches the edit save file (`EDITSAVEMODE`) on or off,
for every file in the list. The edit save file is the one each file's `.par` names, `file.esf` by
default. When a single file is processed it can be changed (`EDITSAVEFILE`); the files of a datalist
each keep their own. Selecting a file in **Files** shows what its `.par` says. A file with no `.par`
yet shows what mbprocess would find on its own (its `file.esf`).

**Save .par** writes the parameters into each file's `.par` (mbset) and processes nothing.
**Process** does the same, then runs mbprocess on each file in turn; the busy notice says which file
it is on, and the log shows what mbset and mbprocess reported for every file.

A `.par` is only rewritten when a parameter was changed in the dialog. mbprocess decides whether a
file is out of date partly from the age of its `.par`, so writing the same values again would make
every file look out of date. A file with no `.par` at all gets one first (`mbset -L`, which looks
for the navigation and edit files mbprocess would use by itself): mbprocess does not process a file
without one.

**The other tabs** hold the rest of the `.par`, each control named after its mbset key in its
tooltip (the mbprocess manual documents them all):

| Tab | Keys |
|---|---|
| Navigation | `NAVMODE` `NAVFILE` `NAVFORMAT` `NAVINTERP` `NAVTIMESHIFT` `NAVHEADING` `NAVSPEED` `NAVDRAFT` `NAVATTITUDE` `NAVADJMODE` `NAVADJFILE` |
| Sound velocity | `SVPMODE` `SVPFILE` `SSVMODE` `SSV` `ANGLEMODE` `SOUNDSPEEDREF` `TTMULTIPLY` |
| Tide | `TIDEMODE` `TIDEFILE` `TIDEFORMAT` |
| Roll / pitch / heading | `ROLLBIASMODE` `ROLLBIAS` `ROLLBIASPORT` `ROLLBIASSTBD` `PITCHBIASMODE` `PITCHBIAS` `HEADINGMODE` `HEADINGOFFSET` |
| Draft / heave | `DRAFTMODE` `DRAFT` `DRAFTOFFSET` `DRAFTMULTIPLY` `HEAVEMODE` `HEAVEOFFSET` `HEAVEMULTIPLY` |
| Amplitude / sidescan | `AMPCORRMODE` `AMPCORRFILE` `SSCORRMODE` `SSCORRFILE` `AMPSSCORRTOPOFILE` |

Only the keys you change are written, to every file in the list; every other key keeps what each
file's own `.par` says. Changes not yet written stay on screen when another file is selected, until
**Save .par** or **Process** writes them. Keys the dialog does not show (data cutting, levers,
metadata, kluges) are left as the `.par` has them.

Each control in `deps/ui/mbprocess.ui` carries its mbset key as a `parKey` property (with
`parValues` for a list whose values do not start at 0, and `parDefault` for what it shows when the
`.par` lacks the key), and a "..." button names its edit box in `browseFor`. So a control can be
moved, or another key added, in Qt Designer alone.

mbset reads a file name up to the first space, so files named in the `.par` cannot contain spaces.
