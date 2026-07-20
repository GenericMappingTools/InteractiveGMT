# rtp3d.jl — reduce a magnetic field anomaly grid to the pole via 2-D Fourier transform, given the
# inclination/declination of the field and of the magnetization (port of Mirone/M.A.Tivey's
# utils/rtp3d.m, 1992/1994). Can also isolate the X (north), Y (east) or Z (up) component instead
# of doing a full RTP. The 2-D FFT is built from GMT.jl's own `fft1d` (GMT_FFT_1D), applied
# row-wise then column-wise — no FFTW.jl, no in-house FFT (same convention as xyanalysis.jl's 1-D
# case: everything Fourier goes through the GMT library).

"""
    fout, k = rtp3d(f3d, incl_fld, decl_fld, incl_mag, decl_mag; component=0)

Reduce a magnetic field anomaly map `f3d` to the pole, given the inclination/declination (degrees)
of the ambient field (`incl_fld`, `decl_fld`) and of the magnetization (`incl_mag`, `decl_mag`).

`component` selects an alternative output instead of the plain RTP: `1` = X/North, `2` = Y/East,
`3` = Z/Up component. Default `0` is the RTP.

Returns the transformed grid `fout` and the wavenumber array `k`.
"""
function rtp3d(f3d::Matrix{<:Real}, incl_fld::Real, decl_fld::Real, incl_mag::Real, decl_mag::Real;
               component::Integer=0)
	_rtp3d(Float64.(f3d), Float64(incl_fld), Float64(decl_fld), Float64(incl_mag), Float64(decl_mag), Int(component))
end

# Annotate all inputs because Julia recompiles everything if a single input type changes.
function _rtp3d(f3d::Matrix{Float64}, incl_fld::Float64, decl_fld::Float64, incl_mag::Float64, decl_mag::Float64,
                 component::Int)
	D2R = pi / 180
	incl_fld *= D2R;  decl_fld *= D2R
	incl_mag *= D2R;  decl_mag *= D2R

	ny, nx = size(f3d)
	x = collect((-0.5):(1/nx):(0.5 - 1/nx))
	y = collect((-0.5):(1/ny):(0.5 - 1/ny))

	X = Matrix{Float64}(undef, ny, nx)
	Y = Matrix{Float64}(undef, ny, nx)
	@inbounds for c = 1:nx, r = 1:ny
		X[r, c] = x[c]
		Y[r, c] = y[r]
	end
	k = 2pi .* sqrt.(X.^2 .+ Y.^2)			# wavenumber array
	aux = atan.(Y, X)						# auxiliary angle, computed once (as in the .m)

	# ------ geometric and amplitude factors
	Ob = sin(incl_fld) .+ im .* cos(incl_fld) .* sin.(aux .+ decl_fld)
	O = if (abs(incl_fld - incl_mag) > 0.03 || abs(decl_fld - decl_mag) > 0.03 || component != 0)  # 0.03 rad is < 2 deg
		Om = sin(incl_mag) .+ im .* cos(incl_mag) .* sin.(aux .+ decl_mag)
		Ob .* Om
	else
		Ob .* Ob
	end

	if (component != 0)						# X, Y or Z component instead of a plain RTP
		new_inc, new_dec = component == 1 ? (0.0, 0.0) :
		                   component == 2 ? (0.0, 90.0) :
		                   component == 3 ? (90.0, 0.0) : error("rtp3d: component must be 0, 1, 2 or 3")
		new_inc *= D2R;  new_dec *= D2R
		O = O ./ ((sin(new_inc) .+ im .* (cos(new_inc) .* sin.(aux .+ new_dec)) .+ eps()) .* Om)
	end
	O = _fftshift2(O)

	mfin = sum(f3d) / length(f3d)			# mean of the input field
	F = _fft2_gmt(complex.(f3d .- mfin))
	fout = real.(_ifft2_gmt(F ./ O))
	return fout, k
end

# ---- 2-D FFT/IFFT via GMT's own 1-D C FFT (GMT.fft1d -> GMT_FFT_1D), applied separably: each row
# then each column. Mathematically identical to a native 2-D FFT since the DFT is separable.
function _fft2_gmt(A::Matrix{<:Complex})
	ny, nx = size(A)
	B = Matrix{ComplexF64}(undef, ny, nx)
	@inbounds for r = 1:ny
		B[r, :] = GMT.fft1d(view(A, r, :))
	end
	C = Matrix{ComplexF64}(undef, ny, nx)
	@inbounds for c = 1:nx
		C[:, c] = GMT.fft1d(view(B, :, c))
	end
	return C
end

function _ifft2_gmt(A::Matrix{<:Complex})
	ny, nx = size(A)
	B = Matrix{ComplexF64}(undef, ny, nx)
	@inbounds for r = 1:ny
		B[r, :] = GMT.fft1d(view(A, r, :); inverse=true)
	end
	C = Matrix{ComplexF64}(undef, ny, nx)
	@inbounds for c = 1:nx
		C[:, c] = GMT.fft1d(view(B, :, c); inverse=true)
	end
	return C
end

# Same convention as AbstractFFTs' fftshift: circular shift by half (rounded down) each dimension.
_fftshift2(A::Matrix{<:Complex}) = circshift(A, div.(size(A), 2))

# ---------------------------------------------------------------------------------------------
# Geophysics > Magnetics > "Reduction to the Pole" / "Total field to Components" — host glue.
# Port of Mirone's parker_stuff.m ('redPole'/'component' cases) + utils/mboard.m (FFT padding).
# ONE C++ dialog (Rtp3DDialog, 70_window.cpp) and ONE callback here serve both menu entries; the
# only difference is the `component` value the dialog sends (0 = RTP, forced regardless of the
# radio group; 1/2/3 = North/East/Vert, from the dialog's radio group). More parker_stuff.m modes
# (Parker direct/inverse, which DO need the Bat grid) are not ported yet.

# Symmetric Hann window of length n (Mirone's utils/mboard.m local `hanning`, NOT a periodogram
# window — same formula, 0.5 - 0.5*cos, built as a half-window mirrored about the center).
function _hanning(n::Int)
	half = iseven(n) ? n ÷ 2 : (n + 1) ÷ 2
	x = n <= 1 ? Float64[] : collect(0:half-1) ./ (n - 1)
	w = 0.5 .- 0.5 .* cos.(2pi .* x)
	return iseven(n) ? vcat(w, reverse(w)) : vcat(w, reverse(w[1:end-1]))
end

# Mirror-pad `w` (ny × nx) to (2ny × 2nx): top-left quadrant is `w` itself, the other three are its
# vertical/horizontal/both-axes mirror (Mirone utils/mboard.m, 'mirror' mode). Used when the dialog's
# "Mirror" checkbox is on — cheap, no tapering, but doubles every dimension.
function _mboard_mirror(w::Matrix{Float64})
	ny, nx = size(w)
	out = Matrix{Float64}(undef, 2ny, 2nx)
	out[1:ny, 1:nx] = w
	out[ny+1:2ny, 1:nx] = @view w[ny:-1:1, :]
	out[1:ny, nx+1:2nx] = @view w[:, nx:-1:1]
	out[ny+1:2ny, nx+1:2nx] = @view w[ny:-1:1, nx:-1:1]
	return out
end

# Pad `w` (ny × nx) up to (nny × nnx) with a Hann-tapered skirt on all four sides (Mirone
# utils/mboard.m, 'taper' mode — the non-mirror path): each side's skirt fades from the edge value
# down to 0 across its half-Hann-window width. Returns the padded matrix and `to_restore =
# [dny_n, dny_s, dnx_w, dnx_e]` (skirt widths), so the original block can be cropped back out after
# the FFT-domain operation via `w[dny_n+1:dny_n+ny, dnx_w+1:dnx_w+nx]`.
function _mboard_taper(w::Matrix{Float64}, nny::Int, nnx::Int)
	ny, nx = size(w)
	dnx = nnx - nx;  dny = nny - ny
	dnx_w = dnx ÷ 2; dnx_e = dnx - dnx_w
	dny_n = dny ÷ 2; dny_s = dny - dny_n
	to_restore = (dny_n, dny_s, dnx_w, dnx_e)

	# South: append dny_s rows below, each = last row * (falling half-Hann)
	vhan = _hanning(2dny_s)[dny_s+1:end]
	south = vhan * w[ny, :]'
	w = vcat(w, south)
	# East: append dnx_e cols to the right, each = last col * (falling half-Hann)
	vhan = _hanning(2dnx_e)[dnx_e+1:end]
	east = w[:, nx] * vhan'
	w = hcat(w, east)
	# North: prepend dny_n rows above, each = first row * (rising half-Hann)
	vhan = _hanning(2dny_n)[1:dny_n]
	north = vhan * w[1, :]'
	w = vcat(north, w)
	# West: prepend dnx_w cols to the left, each = first col * (rising half-Hann)
	vhan = _hanning(2dnx_w)[1:dnx_w]
	west = w[:, 1] * vhan'
	w = hcat(west, w)
	return w, to_restore
end

# "Good" (5-smooth: prime factors <= 5) FFT sizes, Mirone's utils/mboard.m `nlist` — GMT's FFT
# is fast on these, slow on anything else. Shared with 70_window.cpp's Rtp3DDialog (which owns its
# own copy for the Rows/Cols suggested-size listboxes; kept identical on purpose, see that file).
const _FFT_GOOD_SIZES = (64,72,75,80,81,90,96,100,108,120,125,128,135,144,150,160,162,180,192,200,
	216,225,240,243,250,256,270,288,300,320,324,360,375,384,400,405,432,450,480,
	486,500,512,540,576,600,625,640,648,675,720,729,750,768,800,810,864,900,960,
	972,1000,1024,1080,1125,1152,1200,1215,1250,1280,1296,1350,1440,1458,1500,
	1536,1600,1620,1728,1800,1875,1920,1944,2000,2025,2048,2160,2187,2250,2304,
	2400,2430,2500,2560,2592,2700,2880,2916,3000,3072,3125,3200,3240,3375,3456,
	3600,3645,3750,3840,3888,4000,4096,4320,4374,4500,4608,4800,4860,5000)

# C callback: params = "fieldFile;fieldDip;fieldDec;magDip;magDec;component;newRows;newCols;mirror"
# (component: 0=RTP, 1=North, 2=East, 3=Vert; mirror: "1"/"0"). fieldFile is either a path, or the
# sentinel "selected" meaning "the grid already loaded in this window" (Rtp3DDialog locks Field to
# "In memory grid" and sends this when one is loaded — same convention as grdsample.jl's own
# "selected" case, resolved the same way via `_FIGREG`). Reads/resolves the field grid, optionally
# pads it (mirror or Hann-taper, per Mirone's parker_stuff.m push_compute_CB) when newRows/newCols
# ask for more than the grid already has, runs `rtp3d`, crops back to the original size, and adds
# the result to `scene` (promotes an empty launcher, else adds as an extra surface — same convention
# as `_on_igrf_grid`).
# Returns Cint 1 on success, 0 on failure — Rtp3DDialog (70_window.cpp) shows this as a modal
# result on ITS OWN window (not just _viewer_log_error's Errors console on the parent viewer, which
# the user may not be looking at): a silent Cvoid return here was the actual cause of "Compute does
# nothing, no error anywhere" (2026-07-20) even though this function ran fine end-to-end.
function _on_rtp3d(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		p = split(unsafe_string(cparams), ';')
		fieldFile = String(p[1])
		fieldDip, fieldDec = parse(Float64, p[2]), parse(Float64, p[3])
		magDip, magDec     = parse(Float64, p[4]), parse(Float64, p[5])
		component = parse(Int, p[6])
		newRows, newCols = parse(Int, p[7]), parse(Int, p[8])
		mirror = p[9] == "1"

		G = if fieldFile == "selected"
			fig = get(_FIGREG, scene, nothing)
			fig isa QtFigure ? fig.G : error("No grid loaded in this window")
		else
			GMT.gmtread(fieldFile)
		end
		z = Float64.(G.z)
		ny, nx = size(z)

		if newRows > ny || newCols > nx
			if mirror
				zpad = _mboard_mirror(z)
				fout, _ = rtp3d(zpad, fieldDip, fieldDec, magDip, magDec; component=component)
				fout = fout[1:ny, 1:nx]
			else
				zpad, (dny_n, _, dnx_w, _) = _mboard_taper(z, max(newRows, ny), max(newCols, nx))
				fout, _ = rtp3d(zpad, fieldDip, fieldDec, magDip, magDec; component=component)
				fout = fout[dny_n+1:dny_n+ny, dnx_w+1:dnx_w+nx]
			end
		else
			fout, _ = rtp3d(z, fieldDip, fieldDec, magDip, magDec; component=component)
		end

		title = component == 0 ? "RTP" : component == 1 ? "North component" :
		        component == 2 ? "East component" : "Vertical component"

		# REPLACE, never pile up: recomputing the SAME component (or RTP) must trash the previous
		# result under this exact name — both the C++ actor (gmtvtk_remove_grid_h, same primitive the
		# nested-transplant path uses) and the stale Julia-side reference (_forget_object!) — before
		# adding the new one. Otherwise every recompute left a same-named duplicate grid behind and
		# the Scene Objects list turned into an unreadable pile of "North component" x N.
		ccall(_fn(:gmtvtk_remove_grid_h), Cint, (Ptr{Cvoid}, Cstring), scene, title)
		_forget_object!(scene, :grid, title)

		G2 = deepcopy(G)
		G2.z = Float32.(fout)
		_grid_command!(G2, "InteractiveGMT.rtp3d(field, $fieldDip, $fieldDec, $magDip, $magDec; component=$component)")
		r = G2.range
		geog = _isgeog(G2)
		cz, crgb, ncolor = _cpt_nodes(G2, :turbo)
		has_surface = ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene)
		promote = (has_surface == 0)
		fn = promote ? :gmtvtk_promote_surface_h : :gmtvtk_add_surface_h
		ok = promote ?
			ccall(_fn(fn), Cint,
			  (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
			   Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring),
			  scene, G2.z, Cint(nx), Cint(ny), r[1], r[2], r[3], r[4], Cint(geog),
			  cz, crgb, Cint(ncolor), C_NULL, Cint(0), Cint(0), Cint(0), Cint(0), String(title)) :
			ccall(_fn(fn), Cint,
			  (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble,
			   Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring),
			  scene, G2.z, Cint(nx), Cint(ny), r[1], r[2], r[3], r[4],
			  cz, crgb, Cint(ncolor), C_NULL, Cint(0), Cint(0), Cint(0), Cint(0), String(title))
		if ok == 0
			_viewer_log_error(scene, "RTP3D: window closed, grid not added")
			return Cint(0)
		end
		_remember_object!(scene, :grid, title, G2)
		return Cint(1)
	catch e
		_viewer_log_error(scene, "RTP3D FAILED: $(sprint(showerror, e))")
		@warn "RTP3D FAILED" exception=(e,)
		return Cint(0)
	end
end

function _register_rtp3d()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_rtp3d, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_rtp3d_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
