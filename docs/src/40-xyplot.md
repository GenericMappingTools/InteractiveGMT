```@meta
CurrentModule = InteractiveGMT
```

# X,Y Plot Tool

## Opening the Plotter

```julia
using InteractiveGMT, GMT

# Simple plot
t = 0:0.1:10
y = sin.(t)
fig = xyplot([t y])

# From GMTdataset
D = gmtread("data.txt")
fig = xyplot(D)

# Multiple series
D1 = gmtread("series1.txt")
D2 = gmtread("series2.txt")
fig = xyplot([D1, D2])
```

## Adding Series

```julia
add!(fig, more_data; label="Series 2", linestyle="--")
```

## Clearing

```julia
clear!(fig)
```

## Time Series

X-axis auto-formats as dates/times when X contains epoch seconds:

```julia
t = time() .+ (0:3600)  # Next hour
y = rand(length(t))
fig = xyplot([t y]; xtime=true)
```

Or use the function:

```julia
xtime!(fig, true)
```

## Log Axes

```julia
# Log X
fig = xyplot(data; xscale=:log)

# Log Y
fig = xyplot(data; yscale=:log)

# Both
fig = xyplot(data; xscale=:log, yscale=:log)
```

Or use:

```julia
logscale!(fig, :x)  # or :y, :xy
```

## Stick Diagrams

Vector diagrams:

```julia
# Vector components
t = 0:0.1:10
u = cos.(t)
v = sin.(t)
fig = stickplot(t, u, v)

# Azimuth + magnitude
az = rand(360)
mag = rand(360)
fig = stickplot(1:360, az; mag=mag)
```

## Analysis

The **Analysis** menu provides data transforms:

- Remove Mean
- Remove Trend
- 1st/2nd Derivative
- FFT Amplitude / PSD
- Autocorrelation
- Polynomial Fit
- Savitzky-Golay Smoothing
- Butterworth Filter
- Filter Outliers

Select a series first, then choose an analysis.

## Pages

Click **[+]** to add a new page. Right-click tabs for options (New/Duplicate/Rename/Delete).
