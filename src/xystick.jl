# xystick.jl — stick (vector) diagrams for the X,Y plot tool (Mirone's ecran('stick',…)). At each
# time t a small vector is drawn from the y=0 baseline in the (u,v) direction — the classic
# oceanographic current / geomagnetic vector time series. Rendered as ONE series of disconnected
# 2-point segments separated by NaN (vtkChartXY breaks a polyline at NaN), so no C++ change is
# needed: the segments go through the normal add_series path.

# (u,v) components from azimuth degrees (oceanographic: 0° = +y "North", clockwise to +x "East").
_stick_uv_from_az(az, mag) = (mag .* sind.(az), mag .* cosd.(az))

# Build the NaN-separated segment arrays: for each i, base (t[i],0) -> tip (t[i]+u*sc, v*sc), then a
# NaN break. `sc` scales both components (so the u:v ratio — the stick angle in data units — is kept).
function _stick_segments(t, u, v, sc)
	n = length(t)
	segx = Vector{Float64}(undef, 3n)
	segy = Vector{Float64}(undef, 3n)
	@inbounds for i in 1:n
		k = 3 * (i - 1)
		segx[k+1] = t[i];               segy[k+1] = 0.0
		segx[k+2] = t[i] + u[i] * sc;   segy[k+2] = v[i] * sc
		segx[k+3] = NaN;                segy[k+3] = NaN
	end
	return segx, segy
end

# Auto stick scale: a full-magnitude stick spans ~5% of the time axis.
function _stick_scale(t, u, v)
	xspan = maximum(t) - minimum(t)
	mmax  = maximum(sqrt.(float.(u).^2 .+ float.(v).^2))
	mmax = mmax == 0 ? 1.0 : mmax
	xspan = xspan == 0 ? 1.0 : xspan
	return 0.05 * xspan / mmax
end

"""
    stickplot(t, u, v; scale=:auto, color=nothing, title="Stick diagram", kwargs...) -> QtXYPlot
    stickplot(t, azimuth; mag=nothing, kwargs...) -> QtXYPlot

Draw a **stick (vector) diagram**: at each `t` a vector is plotted from the y=0 baseline. Give the
vectors as `(u, v)` components, or as `azimuth` degrees (oceanographic: 0° = up/North, clockwise)
with optional `mag` (default 1). `scale` sizes the sticks (`:auto` ≈ 5 % of the time span per
full-magnitude vector). Returns a [`QtXYPlot`](@ref); extra keywords (`xlabel`, `ylabel`, `xtime`,
`name`) pass through.

```julia
t  = collect(0:0.5:48)                 # hours
az = 90 .+ 60 .* sin.(2π .* t ./ 12)   # tide-turning current direction
stickplot(t, az; mag=1 .+ 0.3 .* cos.(2π .* t ./ 12), title="Current sticks", xlabel="hour")
```
"""
function stickplot(t::AbstractVector, u::AbstractVector, v::AbstractVector;
                   scale=:auto, color=nothing, name::AbstractString="sticks",
                   title::AbstractString="Stick diagram", kwargs...)
	(length(t) == length(u) == length(v)) ||
		error("stickplot: t, u, v must have equal length")
	sc = scale === :auto ? _stick_scale(t, u, v) : Float64(scale)
	segx, segy = _stick_segments(Float64.(t), Float64.(u), Float64.(v), sc)
	p = xyplot(segx, segy; name, color, title, kwargs...)
	add!(p, [float(minimum(t)), float(maximum(t))], [0.0, 0.0]; name="baseline", color=:gray)
	return p
end

function stickplot(t::AbstractVector, azimuth::AbstractVector; mag=nothing, kwargs...)
	m = mag === nothing ? ones(length(t)) : mag
	u, v = _stick_uv_from_az(azimuth, m)
	return stickplot(t, u, v; kwargs...)
end
