```@meta
CurrentModule = InteractiveGMT
```

# plot — 2-D Line Plot

The **plot** icon is the first one on the X,Y Plot tool's gallery row (the strip of miniatures under
the menus). It draws every series of the current page as **lines on linear axes**. From Julia, the
same plot is made with [`plot`](@ref) and [`plot!`](@ref) — GMT.jl's own names and keywords, drawn
in iGMT's interactive window instead of a static figure.

## Syntax

```julia
p = plot(y)
p = plot(x, y)
p = plot(x, Y; multicol=true)    # Y a matrix: one line per column
p = plot(M)                      # M a matrix: column 2 against column 1
p = plot(M; multicol=true)       #   ... and every further column too
p = plot(D)                      # a GMTdataset, or a vector of them
p = plot(x, y; key=value, ...)

plot!(p, x, y; key=value, ...)
plot!(x, y; key=value, ...)      # into the current X,Y window
plot!(p, D)
bar!(p, x, y; width=w)
clear!(p)
```

## Description

- `plot(x, y)` draws the points `(x, y)` joined by straight lines, in an X,Y Plot window.
- `plot(y)` draws `y` against its sample number, `1:length(y)`.
- `plot(M)` takes column 1 of the matrix as X and column 2 as Y, as GMT.jl does. With
  `multicol=true` every further column is a line too; `plot(x, Y; multicol=true)` likewise draws
  each column of `Y` against `x`.
- `plot(D)` takes a `GMTdataset`: column 1 is X, column 2 the line, named after the dataset's column
  names (`multicol=true` for the others). A vector of datasets (a multi-segment table) adds every
  segment.
- `plot!(p, ...)` adds lines to the page that `p` drew; without `p` it adds to the current X,Y
  window. `bar!(p, x, y)` adds a series drawn as bars. `clear!(p)` removes them all.
- The keywords are GMT.jl's: line colour, width, style, markers, legend, axis labels, log axes. See
  [Keywords](@ref xyplot-plot-keywords). A GMT.jl keyword the window cannot honour (`region`,
  `frame`, `savefig`, ...) is an error that names it; `GMT.plot` makes the static figure.

`plot` returns the window's handle, a `QtXYPlot`. While an X,Y Plot window is open, each new
`plot` call adds a **page** (a tab) to it instead of opening a second window.

### The plot icon

Clicking **plot** on the gallery row re-plots every series of the current page as lines, whatever
type it was drawn in before (bars, area, stems, stairs, scatter, ...). Each series keeps its data,
name, colour and style. The axes go back to linear and are re-fitted to all the data. The icon
acts on the page and needs no series selected.

## Running the Examples

Paste each example into the iGMT window's **Julia console**, or into the Julia REPL after
`using InteractiveGMT` (which brings all of GMT.jl with it — do not also load `GMT`, or `plot`
becomes ambiguous). Each one is complete on its own. They use `p` for the plot handle: the console
sets `fig` to its own 3-D window before every command, so do not use `fig` for an X,Y plot there.

## Examples

### Create Line Plot

Create `x` as 100 values from 0 to 2π and plot the sine of `x`.

```julia
x = range(0, 2π; length=100)
y = sin.(x)
p = plot(x, y)
```

### Plot Multiple Lines

Plot two lines on the same page: one with `plot`, the next with `plot!`. Each new line takes the
next colour of the default palette. The legends appear in the chart and in the Object Manager.

```julia
x  = range(-2π, 2π; length=200)
p  = plot(x, sin.(x); legend="sin(x)")
plot!(p, x, cos.(x); legend="cos(x)")
```

The same in one call, with the lines as the columns of a matrix:

```julia
x = range(-2π, 2π; length=200)
p = plot(x, [sin.(x) cos.(x)]; legend="wave", multicol=true)
```

The lines of a matrix are named after `legend` and their position, here `wave 1` and `wave 2`.

### Create Line Plot from Matrix

With `multicol=true` each column of the matrix is one line, drawn against `x`.

```julia
Y = [16  2  3 13
      5 11 10  8
      9  7  6 12
      4 14 15  1]
p = plot(1:4, Y; multicol=true)
```

With no `x`, put X in the first column and use the one-argument form:

```julia
t = 0:0.05:5
p = plot([t exp.(-t) exp.(-2t) exp.(-3t)]; multicol=true)
```

### Plot Values Against Their Index

With only `y`, X is the sample number.

```julia
y = cumsum(randn(500))
p = plot(y; legend="random walk", xlabel="sample", ylabel="position")
```

### Specify Line Style

Plot four sine curves, each in its own line style.

```julia
x = range(0, 2π; length=100)
p = plot(x, sin.(x);           legend="solid")
plot!(p, x, sin.(x .- 0.25);   legend="dash",    ls=:dash)
plot!(p, x, sin.(x .- 0.5);    legend="dot",     ls=:dot)
plot!(p, x, sin.(x .- 0.75);   legend="dashdot", ls="-.")
```

### Specify Line Style, Colour and Marker

A green dashed line with circles at the points.

```julia
x = range(0, 10; length=40)
p = plot(x, sin.(x); lc=:green, ls=:dash, marker=:circle)
```

The same with GMT.jl's `pen`, which carries width, colour and style in one keyword:

```julia
x = range(0, 10; length=40)
p = plot(x, sin.(x); pen=(1, :green, :dash), marker=:circle)
```

Markers alone, with no line:

```julia
x = range(0, 10; length=40)
p = plot(x, cos.(x); ls=:none, marker=:diamond, ms="6p", lc=:red)
```

### Specify Line Width, Marker Size and Colour

A thick line, large square markers, and a colour given as an RGB triple (0–255, or 0–1).

```julia
x = range(-π, π; length=20)
y = tan.(sin.(x)) .- sin.(tan.(x))
p = plot(x, y; lw=2.5, marker=:square, ms="7.5p", lc=(0, 128, 255))
```

### Add Axis Labels and a Page Name

`xlabel` and `ylabel` title the axes. `title` names the window when it opens one, and the page
(the tab) when the window is already open.

```julia
x = range(-2π, 2π; length=200)
p = plot(x, cos.(x); title="Cosine", xlabel="x (rad)", ylabel="cos(x)", legend="cos")
```

### Plot Dates

With X in Unix epoch seconds, `xtime` labels the X axis as dates or times. The ticks follow the
zoom. `xtime` is iGMT's own keyword: GMT.jl has no equivalent.

```julia
using Dates
t = datetime2unix.(DateTime(2026, 6, 1):Hour(6):DateTime(2026, 6, 30))
T = 20 .+ 5 .* sin.(2π .* (t .- t[1]) ./ 86400) .+ randn(length(t))
p = plot(t, T; xtime=:date, legend="temperature", ylabel="°C")
```

The modes are `:date` (format chosen by the span), `:date_ymd`, `:time`, `:decyear` and `:doy`.
Change them later with `xtime!(p, :date_ymd)`, or with **Misc > Time axis (X)**.

### Plot on Log Axes

GMT.jl's `proj=:logx`, `:logy` or `:loglog` set log axes.

```julia
f = 10 .^ range(-1, 3; length=200)
H = 1 ./ sqrt.(1 .+ (f ./ 10) .^ 4)
p = plot(f, H; proj=:loglog, xlabel="frequency (Hz)", ylabel="gain")
```

Switch an axis back to linear with `logscale!(p; x=false)`. The **plot** icon also puts both axes
back to linear; the **semilogx**, **semilogy** and **loglog** icons set them.

### Plot a Table

A `GMTdataset` names its lines after its column names, and its first column titles the X axis.
`multicol=true` draws every column after the first.

```julia
t = 0:0.1:20
D = mat2ds([t sin.(t) 0.5 .* cos.(t)], colnames=["time", "displacement", "velocity"])
p = plot(D; multicol=true)
```

A table read from a file works the same way:

```julia
f = joinpath(tempdir(), "xy_example.dat")
gmtwrite(f, [collect(1:50) cumsum(randn(50))])
p = plot(gmtread(f))
```

### Add Lines to an Existing Page

`plot!` takes the same forms as `plot`: `x, y`, a matrix, or a dataset.

```julia
x = range(0, 4π; length=300)
p = plot(x, sin.(x); legend="sin")
plot!(p, [x 0.5 .* sin.(2 .* x) 0.25 .* sin.(3 .* x)]; multicol=true)
```

### Plot a Circle

```julia
θ = range(0, 2π; length=200)
p = plot(cos.(θ), sin.(θ); legend="unit circle", xlabel="x", ylabel="y")
```

### Clear the Page and Plot Again

```julia
x = range(0, 1; length=50)
p = plot(x, x .^ 2; legend="x²")
clear!(p)
plot!(p, x, sqrt.(x); legend="√x")
```

### Error Bars

GMT.jl's `error_bars` gives each point its error, and the line is drawn with a capped bar at every
point. `asym=true` takes a two-column `[lower upper]` matrix per axis.

```julia
x = 1:10
y = sqrt.(x)
p = plot(x, y; error_bars=(y=0.1 .* y,), marker=:circle)
plot!(p, x, y .+ 1; error_bars=(y=[fill(0.1, 10) fill(0.3, 10)], asym=true), legend="asymmetric")
```

The gallery's **errorbar** icon re-plots the page with the error bars its series carry. A series with
none opens a dialog: take its errors from another series of the page (which is then hidden), a
constant, or a percentage of each point's Y; vertical, horizontal or both.

### Bubble Chart

A vector `ms`, one size per point, draws the series as bubbles (GMT.jl's `bubblechart`): the bubble
area grows with the value, between 5 and 20 points across.

```julia
x = rand(20); y = rand(20); sz = rand(20)
p = plot(x, y; ms=sz, legend="bubbles")
```

The gallery's **bubblechart** icon works like **errorbar**: the sizes a series carries, or the
dialog for one that has none.

### Polar Plots, Feather, Pie and Donut

These gallery icons draw on a **new page**:

- **polarplot**, **polarscatter**, **compassplot**: each series' X is the angle, in radians, and Y the
  radius, drawn as a line, as points, or as arrows from the origin, on polar axes.
- **feather**: the page's first two curves are U and V, drawn as sticks along X (as [`feather`](@ref)).
- **piechart**, **donutchart**: the selected series' values as slices, from 12 o'clock clockwise,
  labelled with their percentage. A sum below 1 leaves the pie partial, as in GMT.jl's `piechart`.

### Change a Line After Plotting

Right-click the line in the **Object Manager** and pick **Line properties…** for its colour, width,
style and markers. **Rename…** changes its legend name, **Delete** removes it.

### Back to Lines from Another Plot Type

Draw one series as a line and one as bars, then click **plot** on the gallery row: both are lines,
with their colours and names.

```julia
x = 1:12
p = plot(x, rand(12); legend="2025")
bar!(p, x, rand(12); legend="2026")
```

## Input Arguments

**`x` — X values**, a vector. Ranges (`0:0.1:10`, `range(...)`) are accepted.

**`y` — Y values**, a vector of the same length as `x`, or a matrix with `length(x)` rows (its first
column is the line; every column with `multicol=true`).

**`M` — table**, a numeric matrix of at least two columns: column 1 is X, column 2 the line (every
further column with `multicol=true`).

**`D` — dataset**, a `GMTdataset` of at least two columns, or a vector of them. Column names name
the lines; column 1's name titles the X axis. With several segments, the lines are named
`"<column> (seg <n>)"`.

**`p` — plot handle**, the `QtXYPlot` returned by `plot`.

## [Keywords](@id xyplot-plot-keywords)

GMT.jl's own keywords, with their aliases. Lengths are GMT.jl's units: a number is points for line
widths and centimetres for marker sizes; a string carries its unit (`"1p"`, `"0.2c"`, `"0.1i"`).

| Keyword | Values | Default |
|---------|--------|---------|
| `legend`, `l` | line name, in the legend and the Object Manager | `"Line n"` |
| `lc`, `linecolor` | `:red`, `"blue"`, ...; an `(r, g, b)` triple, 0–255 or 0–1 | next palette colour |
| `lw`, `linewidth` | line width, points | tool default |
| `ls`, `linestyle` | `:solid`/`"-"`, `:dash`/`"--"`, `:dot`/`":"`, `:dashdot`/`"-."`, `:dashdotdot`/`"-.."`, `:none` (markers only) | `:solid` |
| `pen` | `(width, colour, style)`, or `"width,colour,style"`; any part may be left out | — |
| `marker`, `shape` | `:cross` (`:x`), `:plus` (`"+"`), `:square` (`:s`), `:circle` (`:c`), `:diamond` (`:d`) | no marker |
| `ms`, `markersize` | marker size, cm or a string with its unit; a vector (one per point) draws bubbles | tool default |
| `error_bars`, `E` | `(y=e,)`, `(x=e,)`, both; `asym=true` with `[lower upper]` columns | none |
| `mc`, `markercolor` | the series colour when `lc` is not given (one colour per series) | — |
| `multicol` | `true`: every column after the first is a line | `false` |
| `xlabel`, `ylabel` | axis titles (`plot` only) | `"X"`, `"Y"` |
| `title` | window name, or page name in an open window (`plot` only) | `""` |
| `proj` | `:logx`, `:logy`, `:loglog` (`plot` only) | linear |
| `xtime` | iGMT only: `:linear`, `:date`, `:date_ymd`, `:time`, `:decyear`, `:doy`; `true` = `:date` (`plot` only) | `:linear` |
| `track` | iGMT only: `(lon, lat)` or an n×2 matrix, the track the series was measured along | none |
| `trackgeog` | iGMT only: `false` when `track` is cartesian | `true` |
| `width` | bar width in X units, 0 = automatic (`bar!` only) | `0` |

Named colours: `red`, `green`, `blue`, `black`, `white`, `yellow`, `cyan`, `magenta`, `orange`,
`purple`, `brown`, `gray`, `lightgreen`, `lightred`, `lightblue`, `darkred`, `lightgray`.

With several lines in one call, `lc`, `lw`, `ls` and `marker` apply to every line; restyle them
afterwards from the Object Manager.

## See Also

[X,Y Plot Tool](40-xyplot.md) · [`xtime!`](@ref) · [`logscale!`](@ref) · [`clear!`](@ref) ·
[`bar!`](@ref) · the gallery's **area**, **bar**, **stem**, **stairs** and **scatter** icons.
