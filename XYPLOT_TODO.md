# X,Y plot tool — roadmap / remaining work

The standalone 2-D plotter (evolution of the Profile), a port of Mirone's `ecran.m`
(`C:\SVN\mironeWC\src_figs\ecran.m`, ~4000 lines). C++ GUI in `deps/src/65_xyplot.cpp`
(+ exports in `90_c_api.cpp`); Julia bridge in `src/xyplot.jl`, `src/xyanalysis.jl`,
`src/xystick.jl`. Tests: `test/test-xyplot-unit.jl` (CI-safe math) + `:xyplot`-tagged GUI
items in `test/test-scene-gui.jl`.

This file is the durable backlog — keep it updated as items land.

## Done
- Window shell: menubar (File/Analysis/Misc), toolbar, Object Manager (series tree), foldable
  Data Viewer, status-bar coord readout, export PNG. Plot via **vtkChartXY**.
- Julia API: `xyplot`, `add!`, `clear!`, `QtXYPlot`; `xyplot(D::GMTdataset/Vector)`.
- File: Open / Save / New (Julia `gmtread`/`gmtwrite` via callbacks); axis labels; colour cycling
  (tab10); per-series **line properties** dialog (colour/width/style/marker) + the same as
  `xyplot`/`add!` kwargs (`linestyle`, `marker`, `markersize`).
- Analysis (`ecran` "Analysis" menu): Remove Mean, Remove Trend, 1st/2nd derivative, FFT Amplitude,
  FFT PSD, Autocorrelation, **Fit polynomial** (degree dlg), **Savitzky-Golay smoothing** (window
  dlg, stands in for the smoothing spline), **Butterworth filter** (cutoff+low/high dlg),
  **Filter Outliers / despike** (n-σ dlg; `_despike` = Savitzky-Golay baseline + MAD threshold +
  interp-fill — a faithful port of ecran's one-shot `outliers_clean`). All dep-free (in-house
  radix-2 FFT + small LSQ/median solvers).
- Time axes (X = epoch seconds): date(auto/ymd) / time / decimal-year / day-of-year, auto-updating
  ticks (`xtime!`, `xtime=` kwarg).
- **Log axes** (X/Y): Misc menu toggles + `logscale!` / `xscale=`/`yscale=` kwargs.
- 3-D Profile → X,Y tool: ProfilePanel right-click + `profile_to_xyplot(fig)` (seed callback gives
  the spawned window a Julia mirror so Save/Analysis work).
- Front-door routing: a 2-col non-geographic table via `iview`/drop → X,Y tool (`xy=` override).
- **Stick (vector) diagrams**: `stickplot(t,u,v)` / `stickplot(t,az; mag)` (NaN-separated segments).

## Remaining — ecran specialists (port on demand)

### Magnetic anomaly modelling (the big chunk) — ecran `push_magBar_CB` (2183), `push_syntheticRTP_CB` (2339), `push_ageFit_CB` (2591), `edit_startAge`/`edit_ageEnd`/`popup_ageFit`/`edit_ageFit`/`slider_filter`
- Magnetic reversal **bar code** on top of a profile (geomagnetic polarity timescale between two ages).
- **Synthetic RTP** profile (reduced-to-the-pole synthetic magnetic anomaly from a spreading model).
- **Age fit by correlation**: slide a synthetic against the measured profile to pick isochron ages
  (`hSynthetic`, `hAgeLine_fit`, "Guide me" first-guess, contamination-factor slider).
- Heavy + domain-specific (seafloor-spreading / isochron picking). Needs the polarity timescale data
  + a magnetisation/forward model. Likely a separate module `src/xymag.jl` + a magnetic-bar widget.

### Extensional / tectonic measuring — ecran `extensional_CB` (1641), `plotHeaves_CB` (1652), `plotExx_CB` (1660), `saveHeaves_CB` (1669)
- "Activate extensional measuring": interactive pick of fault heaves on a profile → plot Heaves /
  Exx (extension strain) / save. Interactive (mouse picking on the curve).

### Spectral depth-to-sources (Spector-Grant) — PARTIAL, NOT a faithful port — ecran `dynSlope_CB` (674), `recompSI`
- What exists: a NON-interactive **dialog** (Analysis > "Depth to sources (Spector-Grant)…") that
  fits ln(power) vs k over a TYPED band and reports depth = |slope|/(4π)·unit (`_spector_grant`,
  math verified on a synthetic spectrum, op "specgrant:f1:f2:xfac"). It overlays a fit line.
- NOT done — this is NOT ecran's tool. ecran's `dynSlope_CB` is INTERACTIVE: click-drag a band
  directly on the spectrum, with a live slope/depth readout and an editable fit line. Also missing:
  the "Slope/Intercept" context readout and bandpass-on-the-chunk (`do_bandFilter`). A real port
  needs drag-band selection on the vtkChartXY chart.

### Sound Velocity Profile — ecran `AnalysisSVP_CB` (2066)
- Oceanographic SVP tool (sound speed vs depth). Check exactly what it computes in ecran.

### Histogram / Bar graph — ecran "Show histogram" / "Show Bar graph" (line cmenu, ~548)
- Histogram of a series (bin + count) and a bar plot. vtkChartXY has `vtkChart::BAR`. Dep-free, easy,
  broadly useful — good next pick.

### Sessions — ecran `FileSaveSession_CB` (1723) / `FileOpenSession_CB` (1825)
- Save/restore the whole figure (all series + styles + labels + axis modes) to a file and reload.
  Julia-side: serialise the `QtXYPlot.series` + style/axis state; reopen with `xyplot`/`add!`.

### GMT-script export — ecran `write_gmt_script` / `plot_composer` (File > Save GMT script)
- Emit a GMT batch/script that reproduces the plot. Lower priority.

### Smaller ecran goodies
- **Reference a line to another** (`do_reference`, "Reference me") — subtract/relate two series.
- **Scatter from two CMOP lines** (`make_scatterPlot`).
- **Red markers** save/restore (`FileSaveRedMark`) — annotation picks.
- **Segmentation** (`AnalysisSegmentation_CB`, commented out in ecran).

## Polish / non-ecran
- Real **Reinsch cubic smoothing spline** (vs the current Savitzky-Golay) — needs a banded solver
  hand-rolled (no LinearAlgebra/SparseArrays dep; Project.toml is off-limits).
- `xtime` / `logscale` set from Julia do **not** tick the Misc-menu radio/checkboxes (cosmetic).
- README / docs section for the X,Y tool.
- Butterworth edge ringing (circular FFT) — taper/reflective padding.
- A `yscale=:log`/`xscale=:log` set before a later `add!` does not persist (the added plot resets the
  axis behaviour) — re-apply log after add, or track scale state on QtXYPlot.
