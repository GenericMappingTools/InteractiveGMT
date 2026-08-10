# fftstuff.jl — the FFT tool (port of Mirone's src_figs/fft_stuff.m).
#
# ONE engine, THREE doors, exactly as in Mirone (mirone_uis.m):
#   Mag/Grav > FFT tool      (line 604) -> 'Allopts' -> the full dialog, over a grid
#   Image > FFT Spectrum     (line 374) -> 'Allopts' -> the same dialog, over an image (RGB -> grey)
#   Grid Tools > Spectrum >  (704-706)  -> 'Amplitude' / 'Power' / 'Autocorr': the same maths with no
#                                          dialog at all — Mirone's "quick mode", which calls
#                                          `sectrumFun` straight and never builds the window.
# So the operations live HERE, in functions that take a grid and a settings struct, and the dialog is
# only a way to fill that struct in. A quick-mode menu entry calls the same function with defaults.
#
# WHAT IS NOT WRITTEN HERE, because it already exists (SACRED_LAW.md: same operation, same function):
#   * the padding — `_mboard_taper` / `_mboard_mirror` and the good-FFT-size list `_FFT_GOOD_SIZES`
#     (rtp3d.jl), themselves ports of Mirone's utils/mboard.m. The FFT tool's "# Rows / # Cols"
#     listboxes are those same numbers.
#   * the transform — `GMT.fft2d!` (GMT_FFT_2D), the same entry point rtp3d.jl and hillshade.jl use.
#     No FFTW, here or anywhere else in this package.
#   * "Remove trend" — GMT's own `grdtrend` with -N3 -D, which is what `c_grdtrend(Z,head,'-D','-N3')`
#     is in the .m file.

# Moritz's 1980 IGF value for gravity in mGal at 45 degrees latitude — the constant that turns the
# vertical derivative into a gravity anomaly (Geoid2FAA) and the integral into a geoid height
# (FAA2Geoid). Verbatim from fft_stuff.m.
const _FFT_IGF45 = 980619.9203

# How the grid's x/y units are to be read. The dialog's CONFIRM popup (Geogs / Meters / Kilometers)
# sets it, and Mirone means the shouting: nothing downstream can tell metres from degrees on its own.
@enum FFTCoords FFT_GEOG FFT_METERS FFT_KM

# Everything the maths needs besides the grid itself: the padded size, how the axes are to be read,
# and whether a plane comes off first. One struct so every operation takes the same thing.
struct FFTOpts
	new_nx::Int
	new_ny::Int
	coords::FFTCoords
	detrend::Bool
end

# ---------------------------------------------------------------------------------------------
# Geometry: the grid's node spacing in METRES, which is what every wavenumber below is built from.
#
# The geographic case is Mirone's `scltln` (fft_stuff.m:764), a port and not a substitution: it is
# the local metres-per-degree of the WGS-84 ellipsoid (Snyder 1987, USGS PP 1395 pp. 24-25),
# evaluated at the grid's middle latitude. That is a SCALE FACTOR, not a distance between two points
# — a geodesic solver answers a different question and would put a different number here.
function _fft_scltln(lat::Float64)
	a, b = 6378137.0, 6356752.1                      # WGS-84, metres (gmt_defaults.h)
	e2 = 1 - (b * b) / (a * a)
	s  = sind(lat)
	den = sqrt(1 - e2 * s * s)
	sclat = (pi / 180) * a * (1 - e2) / den^3        # metres per degree of latitude
	sclon = (pi / 180) * a * cosd(lat) / den         # metres per degree of longitude
	return (sclat, sclon)
end

# The node spacing the transform must use, in metres — or in kilometres when the user says the grid
# is in km, which is why the km branch multiplies by 1000 (fft_stuff.m's `is_km`: scaled_dx/dy were
# in metres, so the wavenumbers come out per km).
function _fft_scaled_inc(G::GMTgrid, o::FFTOpts)
	r = G.range
	dx, dy = Float64(G.inc[1]), Float64(G.inc[2])
	if o.coords == FFT_GEOG
		sclat, sclon = _fft_scltln((r[4] + r[3]) / 2)
		return (dx * sclon, dy * sclat)
	elseif o.coords == FFT_KM
		return (dx * 1000, dy * 1000)
	end
	return (dx, dy)
end

# ---------------------------------------------------------------------------------------------
# The wavenumber array, ifftshifted — `wavenumber_and_mboard` in the .m file. `split=true` returns
# (kx, ky) separately, which the directional derivative and the analytic signal need; otherwise the
# modulus sqrt(kx^2+ky^2), which is what continuation, differentiation and integration use.
function _fft_wavenumbers(nnx::Int, nny::Int, dx::Float64, dy::Float64; split::Bool = false)
	nx2, ny2 = fld(nnx, 2), fld(nny, 2)
	sft_x = iseven(nnx) ? 1 : 0
	sft_y = iseven(nny) ? 1 : 0
	dkx = 2pi / (nnx * dx)
	dky = 2pi / (nny * dy)
	kx = [(i * dkx) for i in -nx2:(nx2 - sft_x)]
	ky = [(j * dky) for j in -ny2:(ny2 - sft_y)]
	if split
		KX = _fft_ishift([kx[i] for j in eachindex(ky), i in eachindex(kx)])
		KY = _fft_ishift([ky[j] for j in eachindex(ky), i in eachindex(kx)])
		return (KX, KY)
	end
	return _fft_ishift([hypot(kx[i], ky[j]) for j in eachindex(ky), i in eachindex(kx)])
end

# fftshift / ifftshift — index moves, no arithmetic. Written here because AbstractFFTs comes with
# FFTW, which this package deliberately does not depend on (the transform itself is GMT's).
_fft_shift(A::Matrix)  = circshift(A, (fld(size(A, 1), 2), fld(size(A, 2), 2)))
_fft_ishift(A::Matrix) = circshift(A, (-fld(size(A, 1), 2), -fld(size(A, 2), 2)))

# ---------------------------------------------------------------------------------------------
# Forward / inverse 2-D transform, GMT's own (GMT_FFT_2D). Kept as the only two places this package's
# FFT tool touches a transform, so a change of entry point is a change in one spot.
# ComplexF32 because that is what GMT_FFT_2D takes (GMT/src/fft1d.jl); the inverse NORMALISES, so a
# forward+inverse round trip returns the input unchanged (verified: max|err| = 0, DC term = sum).
_fft_fwd(Z::AbstractMatrix{<:Real}) = GMT.fft2d!(ComplexF32.(Z))
_fft_inv(F::Matrix{ComplexF32}) = Float64.(real.(GMT.fft2d!(copy(F); inverse = true)))

# ---------------------------------------------------------------------------------------------
# The grid as a plain Float64 matrix, row 1 = south — through `_zmat`, THE accessor for Julia-side
# maths on a grid's z (SACRED_LAW.md, grid memory-layout law: never index G.z directly).
_fft_zmat(G::GMTgrid) = Matrix{Float64}(_zmat(G))

# "Remove trend": GMT's grdtrend, N=3 (a plane), -D = keep the residual. Same call Mirone makes.
function _fft_detrend(G::GMTgrid)
	try
		return GMT.grdtrend(G; model = 3, diff = true)
	catch e
		@warn "FFT tool: could not remove the trend (continuing with the raw grid)" exception = (e,)
		return G
	end
end

# Pad to (new_ny x new_nx) with mboard's Hann-tapered skirt, and say where the original sits inside
# the padded array. `mode = :mirror` doubles each dimension instead (mboard's other mode).
# Returns (Zpadded, (row0, col0)) with the original at Z[row0+1 : row0+ny, col0+1 : col0+nx].
function _fft_pad(z::Matrix{Float64}, o::FFTOpts, mode::Symbol = :taper)
	ny, nx = size(z)
	if mode == :mirror
		zp = _mboard_mirror(z)
		return (zp, (0, 0))                       # mirror replicates; the original starts at (1,1)
	end
	(o.new_ny <= ny && o.new_nx <= nx) && return (copy(z), (0, 0))
	zp, (d1_lo, _, d2_lo, _) = _mboard_taper(z, max(o.new_ny, ny), max(o.new_nx, nx))
	return (zp, (d1_lo, d2_lo))
end

# Crop the padding skirt back off — `unband` in the .m file.
_fft_unband(Z::Matrix, band::Tuple{Int,Int}, ny::Int, nx::Int) =
	Z[(band[1] + 1):(band[1] + ny), (band[2] + 1):(band[2] + nx)]

# The grid a transform hands back: the input's geometry and georeferencing, new values. The layout is
# stamped "BCB" because this IS a freshly built column-major matrix (row 1 = south) whatever the
# source grid's own layout was — SACRED_LAW.md's grid memory-layout law says so in as many words.
function _fft_result_grid(G::GMTgrid, Z::Matrix{Float64})
	G2 = GMT.mat2grid(Float32.(Z), G)
	G2.layout = "BCB"
	G2.names  = String[]
	G2.range[5:6] = [extrema(G2.z)...]
	return G2
end

# A spectrum is NOT the input's geometry: its axes are wavenumbers, so it gets its own X/Y, and it is
# never geographic (fft_stuff.m sets `handMir.geog = 0` on the window it opens for one).
function _fft_spectrum_grid(Z::Matrix{Float64}, x::Vector{Float64}, y::Vector{Float64})
	G = GMT.mat2grid(Float32.(Z); x = x, y = y)
	G.layout = "BCB"
	G.names  = String[]
	return G
end

# ---------------------------------------------------------------------------------------------
# SPECTRA AND CORRELATIONS — `sectrumFun` (fft_stuff.m:463), whose six modes are the whole of
# Grid Tools > Spectrum and the dialog's four spectrum buttons:
#   :power       log10(F conj(F) / (nx ny) + 1)        Power Spectrum      (+1 so log(0) cannot bite)
#   :amplitude   log10(|F| / (nx ny) + 1)              Amplitude spectrum
#   :autocorr    fftshift(real(ifft2(|fft2 Z|^2)))     Auto Correlation
#   :crosspower  log10((ReF1 ReF2 + ImF1 ImF2)/(nx ny) + 1)   Cross Spectra    (needs G2)
#   :crosscorrel fftshift(real(ifft2(|fft2 Z1 . fft2 Z2|)))   Cross Correlation (needs G2)
# The padding skirt is cut off AFTER the shift, keeping the central nx x ny block — verbatim from the
# .m file, where the crop indices come straight out of mboard's band.
function fft_spectrum(G::GMTgrid, mode::Symbol, o::FFTOpts; G2::Union{GMTgrid,Nothing} = nothing)
	two = mode in (:crosspower, :crosscorrel)
	(two && G2 === nothing) && error("$(mode) needs a second grid (Grid2)")
	Gw = o.detrend ? _fft_detrend(G) : G
	z  = _fft_zmat(Gw)
	ny, nx = size(z)
	Z, band = _fft_pad(z, o)
	Z2 = nothing
	if two
		G2w = o.detrend ? _fft_detrend(G2) : G2
		z2  = _fft_zmat(G2w)
		size(z2) == (ny, nx) || error("the two grids must have the same size ($(size(z2)) vs $((ny, nx)))")
		Z2, _ = _fft_pad(z2, o)
	end
	crop(A) = _fft_unband(A, band, ny, nx)
	local S::Matrix{Float64}
	if mode in (:power, :amplitude, :crosspower)          # these are cheaper done shifted-then-cropped
		F = crop(_fft_shift(_fft_fwd(Z)))
		if mode == :power
			S = @. log10(real(F * conj(F)) / (nx * ny) + 1)
		elseif mode == :amplitude
			S = @. log10(abs(F) / (nx * ny) + 1)
		else
			F2 = crop(_fft_shift(_fft_fwd(Z2)))
			S = @. log10((real(F) * real(F2) + imag(F) * imag(F2)) / (nx * ny) + 1)
		end
	elseif mode == :autocorr
		F = _fft_fwd(Z)
		S = crop(_fft_shift(_fft_inv(ComplexF32.(abs.(F) .^ 2))))
	elseif mode == :crosscorrel
		F = _fft_fwd(Z) .* _fft_fwd(Z2)
		S = crop(_fft_shift(_fft_inv(ComplexF32.(abs.(F)))))
	else
		error("unknown spectrum mode '$mode'")
	end
	# The axes. A correlation is in SAMPLES (delta = 1); a spectrum is in wavenumbers built from the
	# padded size and the scaled node spacing. km and geographic both report per km (the .m file
	# multiplies by 1000 in both branches, for different reasons).
	if mode in (:autocorr, :crosscorrel)
		dkx = dky = 1.0
	else
		dx, dy = _fft_scaled_inc(G, o)
		dkx = 2pi / (o.new_nx * dx)
		dky = 2pi / (o.new_ny * dy)
		(o.coords == FFT_KM) && (dkx *= 1000; dky *= 1000)
		(o.coords == FFT_GEOG) && (dkx *= 1000; dky *= 1000)
	end
	nx2, ny2 = fld(nx, 2), fld(ny, 2)
	sft_x = iseven(nx) ? 1 : 0
	sft_y = iseven(ny) ? 1 : 0
	x = [i * dkx for i in -nx2:(nx2 - sft_x)]
	y = [j * dky for j in -ny2:(ny2 - sft_y)]
	return (_fft_spectrum_grid(S, x, y), _FFT_SPECTRUM_NAMES[mode])
end

const _FFT_SPECTRUM_NAMES = Dict(:power => "Power spectrum", :amplitude => "Amplitude spectrum",
	:autocorr => "Autocorrelation", :crosspower => "Cross spectra",
	:crosscorrel => "Cross Correlation")

# ---------------------------------------------------------------------------------------------
# RADIAL POWER AVERAGE — `push_radialPowerAverage_CB`, itself Walter Smith's grdfft routine: one 1-D
# estimate per radial wavenumber bin, summing over the other dimension. Answers (freq, power, label)
# for the X,Y plot; the DC component is dropped and the frequency (not the wavenumber) is reported.
function fft_radial_power(G::GMTgrid, o::FFTOpts)
	# `grdfft -E` IS this computation — Mirone's own comment on push_radialPowerAverage_CB says so
	# ("ORIGINAL TEXT FROM WALTER SMITH IN GRDFFT"); the .m file re-implements it only because MATLAB
	# had no GMT to call. Here there is one, so the module does it:
	#   -E[r]        radial power spectrum -> a table of f, power, 1 std dev
	#   -N<nx>/<ny>  the padded size the dialog's # Rows / # Cols ask for
	#   +d / +l      remove the best-fitting plane, or leave the data alone ("Remove trend")
	#   -fg          a geographic grid, converted to metres by the module itself
	dims = string(o.new_nx, '/', o.new_ny, o.detrend ? "+d" : "+l")
	D = (o.coords == FFT_GEOG) ? GMT.grdfft(G; radial_power = :r, inquire = dims, f = :g) :
	                             GMT.grdfft(G; radial_power = :r, inquire = dims)
	M = (D isa Vector) ? D[1].data : D.data
	size(M, 2) >= 2 || error("grdfft -E returned no spectrum")
	freq  = Float64.(M[:, 1])
	power = Float64.(M[:, 2])
	# UNITS. -fg makes a geographic grid metric, so its frequencies are 1/m like any other metric
	# grid's — reported in 1/km, as Mirone does. A grid whose own units are km already answers in
	# 1/km and must NOT be scaled again.
	(o.coords == FFT_GEOG) && (freq .*= 1000)
	(o.coords == FFT_KM)   && return (freq, power, "Frequency (1/km)")
	(o.coords == FFT_GEOG) && return (freq, power, "Frequency (1/km)")
	return (freq, power, "Frequency (1/m)")
end

# ---------------------------------------------------------------------------------------------
# FIELD TRANSFORMS. Each is: (optionally) remove a plane, pad, transform, multiply by a function of
# the wavenumber, invert, crop the skirt. Only the middle line differs between them, so they share
# everything else through `_fft_transform`.
function _fft_transform(G::GMTgrid, o::FFTOpts, name::String, apply!::Function; split::Bool = false)
	Gw = o.detrend ? _fft_detrend(G) : G
	z  = _fft_zmat(Gw)
	ny, nx = size(z)
	Z, band = _fft_pad(z, o)
	nny, nnx = size(Z)
	dx, dy = _fft_scaled_inc(G, o)
	K = split ? _fft_wavenumbers(nnx, nny, dx, dy; split = true) : _fft_wavenumbers(nnx, nny, dx, dy)
	out = apply!(Z, K)
	return (_fft_result_grid(G, _fft_unband(out, band, ny, nx)), name)
end

# Upward (positive height) / downward continuation: F * exp(-k z).
fft_continuation(G::GMTgrid, o::FFTOpts, zup::Real) =
	_fft_transform(G, o, "U/D Continuation", (Z, k) -> _fft_inv(_fft_fwd(Z) .* Float32.(exp.(-k .* zup))))

# N-th vertical derivative: F * k^n. With `scale = _FFT_IGF45` and n forced to 1 this is Geoid2FAA —
# the geoid-to-free-air-anomaly conversion, which is exactly that derivative times gravity.
function fft_derivative(G::GMTgrid, o::FFTOpts, n::Int; scale::Float64 = 1.0)
	n_der = (scale == 1.0) ? max(n, 1) : 1
	name  = (scale == 1.0) ? "$(n_der)th Vertical Derivative" : "Gravity anomaly (mGal)"
	return _fft_transform(G, o, name, function (Z, k)
		kk = n_der > 1 ? k .^ n_der : k
		F = _fft_fwd(Z) .* Float32.(kk .* scale)
		F[1, 1] = 0
		return _fft_inv(F)
	end)
end

# Integration: F / k. With `scale = _FFT_IGF45` this is FAA2Geoid (a geoid height).
function fft_integrate(G::GMTgrid, o::FFTOpts; scale::Float64 = 1.0)
	name = (scale == 1.0) ? "Integrated grid" : "Geoid height"
	return _fft_transform(G, o, name, function (Z, k)
		kk = copy(k)
		kk[1, 1] = eps()                       # k=0 would divide by zero; the DC term is zeroed below
		F = _fft_fwd(Z) ./ Float32.(kk .* scale)
		F[1, 1] = 0
		return _fft_inv(F)
	end)
end

# Directional derivative, azimuth CCW from north: multiply by i*(sin(az) kx + cos(az) ky).
fft_dir_derivative(G::GMTgrid, o::FFTOpts, azim::Real) =
	_fft_transform(G, o, "$(azim) Azimuthal Derivative", function (Z, K)
		KX, KY = K
		fact = Float32.(sind(azim) .* KX .+ cosd(azim) .* KY)
		F = _fft_fwd(Z)
		F[1, 1] = 0
		return _fft_inv(complex.(-imag.(F) .* fact, real.(F) .* fact))
	end; split = true)

# ---------------------------------------------------------------------------------------------
# THE C CALLBACK. One request string, ';'-separated:
#
#   <op>;<grid1>;<grid2>;<newRows>;<newCols>;<coords>;<detrend>;<value>
#
# `op` is one of the eleven operations; `grid1`/`grid2` are a file path or the sentinel "selected"
# (the grid this window already shows, resolved through `_FIGREG` exactly as rtp3d.jl does);
# `coords` is 0/1/2 = Geogs/Meters/Kilometers (the dialog's CONFIRM popup); `value` carries the one
# number the operation needs (continuation height, derivative order, azimuth) and is ignored by the
# rest. Returns 1 on success, 0 on failure with the reason in the window's Errors console.
#
# The QUICK menu entries (Grid Tools > Spectrum > Amplitude/Power/Autocorrelation) send exactly the
# same string with the grid's own size and no trend removal — there is no second path for them.
function _on_fftstuff(scene::Ptr{Cvoid}, cparams::Cstring, txt::Ptr{UInt8}, txtcap::Cint)::Cint
	try
		p = split(unsafe_string(cparams), ';')
		length(p) >= 8 || error("FFT tool: malformed request '$(unsafe_string(cparams))'")
		op = String(p[1])
		G  = _fft_grid_arg(scene, String(p[2]))
		# "size" — the dialog asking how big the thing it is about to transform actually is. It must
		# ask, because the host's own grid layer is a 2x2 placeholder for an image window (the flat
		# plane the texture rides on), so the C side would otherwise seed the padding boxes with 2.
		if op == "size"
			ny0, nx0 = _grid_dims(G)[2], _grid_dims(G)[1]
			_pal_puttxt(txt, txtcap, string(ny0, ' ', nx0))
			return Cint(1)
		end
		G2 = isempty(strip(String(p[3]))) ? nothing : _fft_grid_arg(scene, String(p[3]))
		# The padded size can never be SMALLER than the grid — the quick menu entries send 0/0
		# meaning "the grid's own size", and a nonsense number typed in the dialog must not become a
		# division by zero in the wavenumber step.
		ny, nx = _grid_dims(G)[2], _grid_dims(G)[1]
		nrows = max(something(tryparse(Int, String(p[4])), 0), ny)
		ncols = max(something(tryparse(Int, String(p[5])), 0), nx)
		coords = (FFT_GEOG, FFT_METERS, FFT_KM)[clamp(parse(Int, p[6]), 0, 2) + 1]
		o = FFTOpts(ncols, nrows, coords, p[7] == "1")
		val = something(tryparse(Float64, String(p[8])), 0.0)

		if op in ("power", "amplitude", "autocorr", "crosspower", "crosscorrel")
			Gout, name = fft_spectrum(G, Symbol(op), o; G2 = G2)
		elseif op == "radial"
			freq, power, xlab = fft_radial_power(G, o)
			# Mirone opens `ecran` for this one; here that is the X,Y tool — and per the standing rule
			# a second call lands as a NEW PAGE on the one that is already open, never a new window.
			Base.invokelatest(xyplot, freq, power; name = "Radial average power spectrum",
			                  title = "Radial average power spectrum", xlabel = xlab,
			                  ylabel = "Log(Power)", yscale = :log10)
			return Cint(1)
		elseif op == "udcont"
			Gout, name = fft_continuation(G, o, val)
		elseif op == "derivative"
			Gout, name = fft_derivative(G, o, max(1, round(Int, val)))
		elseif op == "geoid2faa"
			Gout, name = fft_derivative(G, o, 1; scale = _FFT_IGF45)
		elseif op == "dirderivative"
			Gout, name = fft_dir_derivative(G, o, val)
		elseif op == "analytic"
			Gout, name = fft_analytic_signal(G, o)
		elseif op == "integrate"
			Gout, name = fft_integrate(G, o)
		elseif op == "faa2geoid"
			Gout, name = fft_integrate(G, o; scale = _FFT_IGF45)
		else
			error("FFT tool: unknown operation '$op'")
		end
		# BUILD THE SURFACE. `_remember_object!` alone only files the result in the Julia registry —
		# it puts a row in Scene Objects and draws NOTHING, which is exactly how "Power Spectrum"
		# came out as an empty window with a tidy row in the panel. `_add_grid_to_scene` (drop.jl) is
		# THE door a grid enters a window by, and it does the remembering itself.
		has = ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene)
		_add_grid_to_scene(scene, Gout, name; promote = (has == 0), record = false) ||
			error("window closed, grid not added")
		# A spectrum, a correlation and a continued field are all NEW DERIVED VARIABLES, so they enter
		# through the ONE shared transition (SACRED_LAW.md's derived-variable display + axes laws):
		# named, checked, source unchecked, axes and camera re-framed to their OWN limits — which for a
		# spectrum are wavenumbers, an extent that has nothing to do with the grid it came from.
		_adopt_derived!(scene, name, Gout)
		return Cint(1)
	catch e
		_viewer_log_error(scene, "FFT tool FAILED: $(sprint(showerror, e))")
		@warn "FFT tool FAILED" exception = (e,)
		return Cint(0)
	end
end

# "selected" = the grid this window is showing (same sentinel and same resolution as rtp3d.jl); any
# other string is a file, read through `_gmtread_trb`, THE reader for the open/drop doors.
# An IMAGE is accepted too and converted to a grey grid — that is what Image > FFT Spectrum feeds in,
# and what Mirone does there (`cvlib_mex('color', Z, 'rgb2gray')` in GridToolsSectrum_CB).
function _fft_grid_arg(scene::Ptr{Cvoid}, spec::String)
	data = if spec == "selected"
		# THE resolver for "what is this window showing" (`_find_object_named`, savefile.jl): it
		# answers for BOTH kinds and falls back to the window's own registry entry. A grid first —
		# an image window that also carries a grid is a grid window for this purpose — then the
		# picture, which is what Image > FFT Spectrum runs on.
		_, g = _find_object_named(scene, :grid)
		if g === nothing
			_, g = _find_object_named(scene, :image)
		end
		g === nothing ? error("this window has no grid or image to transform") : g
	else
		_gmtread_trb(spec)
	end
	data isa GMTgrid && return data
	data isa GMTimage && return _fft_image_to_grid(data)
	error("the FFT tool needs a grid or an image (got a $(typeof(data)))")
end

# An image as a grid of grey VALUES. Mirone converts RGB to grey and warns that it does; a
# single-band image is already the quantity it will transform.
function _fft_image_to_grid(I::GMTimage)
	m = I.image
	g = if ndims(m) == 3 && size(m, 3) >= 3
		# Rec. 601 luma, the same weights cvlib's rgb2gray uses.
		Float32.(0.299 .* m[:, :, 1] .+ 0.587 .* m[:, :, 2] .+ 0.114 .* m[:, :, 3])
	else
		Float32.(ndims(m) == 3 ? m[:, :, 1] : m)
	end
	# `mat2grid(mat, I)` — the image's OWN georeferencing, registration included. Handing x/y over by
	# hand instead is wrong the moment the image is pixel-registered: those vectors then hold nx+1
	# coordinates for nx columns, and GMT rejects the pair ("size of x,y vectors incompatible").
	return GMT.mat2grid(g, I)
end

function _register_fftstuff()
	fptr = @cfunction((s, c, t, n) -> Base.invokelatest(_on_fftstuff, s, c, t, n)::Cint,
	                  Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_fftstuff_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# 3-D analytic signal: sqrt((dT/dx)^2 + (dT/dy)^2 + (dT/dz)^2), each derivative taken in the
# wavenumber domain (dx, dy by i*k; dz by |k|).
fft_analytic_signal(G::GMTgrid, o::FFTOpts) =
	_fft_transform(G, o, "3D Analytic Signal", function (Z, K)
		KX, KY = Float32.(K[1]), Float32.(K[2])     # the transform is ComplexF32 (GMT_FFT_2D)
		F = _fft_fwd(Z)
		F[1, 1] = 0
		dTdx = _fft_inv(complex.(-imag.(F) .* KX, real.(F) .* KX)) .^ 2
		dTdy = _fft_inv(complex.(-imag.(F) .* KY, real.(F) .* KY)) .^ 2
		kmod = sqrt.(KX .^ 2 .+ KY .^ 2)
		dTdz = _fft_inv(F .* kmod) .^ 2
		return sqrt.(dTdx .+ dTdy .+ dTdz)
	end; split = true)
