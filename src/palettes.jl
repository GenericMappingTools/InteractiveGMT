# palettes.jl — Image > Color Palettes (port of Mirone's src_figs/color_palettes.m, the tool
# mirone_uis.m:303 puts under "Image > Color Palettes > Change Palette").
#
# SPLIT OF WORK (deliberate, and the reason there is no maths duplicated here):
#   * This file SOURCES palettes — the six families Mirone offers (ML, GMT, CET, CAR, GIMP,
#     Thematic), CPT files read from disk, CPT files written to disk, and the CIE76 curve. Every
#     one of them comes back as plain Nx3 RGB rows in 0..1.
#   * The DIALOG (ColorPalettesDialog, 70_window.cpp) owns everything that reworks those rows or
#     decides which z each row sits at: the two stretch sliders, the draggable colour markers,
#     Discretize, Logaritmize, Min/Max Z and the bathymetry hinge. Those are the GUI's own live
#     operations (they must answer inside a mouse drag) and they exist in exactly one place, there.
#
# WHERE THE COLOURS COME FROM
#   * ML — the formula colormaps are ported from color_palettes.m's own copies (autumn … winter,
#     colorcube, vivid) plus the three MATLAB built-ins it calls (hot/hsv/jet); the five 256-row
#     tables the .m carries verbatim (parula/viridis/magma/inferno/turbo) and mkpj.m's
#     jet-improved table live in data/palettes.bin.
#   * GMT — GMT's own master CPTs through `makecpt`, not Mirone's gmt_other_palettes.mat snapshot,
#     so gebco/globe/haxby/… stay whatever GMT currently ships. The three that are Mirone's own
#     (DEM_screen/DEM_print/DEM_poster) come from data/palettes.bin.
#   * CET / CAR / GIMP / Terre_Mer / mag — data/palettes.bin, converted ONCE from Mirone's
#     data/CETperceptual.mat, caris256.mat, gimp256.mat and gmt_other_palettes.mat by
#     `_build_palette_bin` at the bottom of this file (kept in-tree so the file is reproducible).

# ---------------------------------------------------------------------------------------------
# The family lists, verbatim from color_palettes.m (lines 115-147). Order matters: it is the order
# the listbox shows, and Mirone's order is the ported one.
const _PAL_ML = ["autumn", "bone", "colorcube", "cool", "copper", "flag", "gray", "hot", "hsv",
                 "turbo", "jet", "jet-improved", "lines", "pink", "prism", "summer", "winter",
                 "vivid", "parula", "viridis", "magma", "inferno"]

const _PAL_GMT = ["drywet", "gebco", "globe", "rainbow", "haxby", "no_green", "ocean", "polar",
                  "red2green", "sealand", "seis", "split", "topo", "wysiwyg",
                  "DEM_screen", "DEM_print", "DEM_poster"]

const _PAL_CAR = ["CAR -- Blue", "CAR -- Carnation", "CAR -- Cyan", "CAR -- Desert", "CAR -- Earth",
                  "CAR -- Green", "CAR -- HotMetal", "CAR -- Jelly", "CAR -- Magenta",
                  "CAR -- MorningGlory", "CAR -- Mustard", "CAR -- Ocean", "CAR -- OceanLight",
                  "CAR -- Olive", "CAR -- Oysters", "CAR -- Pumpkin", "CAR -- Red", "CAR -- Rose",
                  "CAR -- Saturn", "CAR -- Seafloor", "CAR -- Space", "CAR -- SuperNova",
                  "CAR -- Topographic", "CAR -- TrackLine", "CAR -- Yellow", "CAR -- colors10"]

const _PAL_GIMP = ["Abstract_1", "Abstract_2", "Abstract_3", "Aneurism", "Blinds", "Browns",
                   "Brushed_Aluminium", "Burning_Paper", "Burning_Transparency", "Caribbean_Blues",
                   "Cold_Steel", "Deep_Sea", "Flare_Glow_Angular_1", "Flare_Glow_Radial_2",
                   "Flare_Radial_102", "Flare_Radial_103", "Flare_Rays_Size_1", "Four_bars",
                   "Full_saturation_spectrum_CCW", "Full_saturation_spectrum_CW", "Golden",
                   "Greens", "Horizon_1", "Incandescent", "Land_1", "Land_and_Sea",
                   "Metallic_Something", "Nauseating_Headache", "Pastels", "Pastel_Rainbow",
                   "Rounded_edge", "Shadows_1", "Shadows_2", "Shadows_3", "Skyline",
                   "Skyline_polluted", "Sunrise", "Tropical_Colors", "Wood_1", "Wood_2",
                   "Yellow_Orange"]

const _PAL_CET = ["L1_GREY_GRAY", "L2_REDUCEDGREY", "L3_KRYW_HEAT_FIRE", "L4_KRY_YELLOWHEAT",
                  "L5_KGY", "L6_KBC", "L7_BMW", "L8_BMY", "L9_BGYW", "L10_GEOGRAPHIC",
                  "L11_GEOGRAPHIC2", "L12_DEPTH", "L13_REDTERNARY", "L14_GREENTERNARY",
                  "L15_BLUETERNARY", "L16_KBGYW", "L17_KBGYW", "L18_WORB", "L19_WCMR",
                  "I1-Isoluminant", "I2-Isoluminant", "I3-Isoluminant",
                  "D1_BWR_COOLWARM-Divergent", "D2_GWV-Divergent", "D3_GWR-Divergent",
                  "D4_BKR-Divergent", "D5_GKR-Divergent", "D6_BKY-Divergent",
                  "D7_BJY_DIVBJY-Divergent", "D8_BJR-Divergent", "D9-Divergent", "D10-Divergent",
                  "D11-Divergent", "D12-Divergent", "D13_BWG-Divergent", "D1A_BWRA-Divergent",
                  "C1-Cyclic", "C2_PHASE4-Cyclic", "C3-Cyclic", "C4_PHASE2-Cyclic",
                  "C5_CYCLICGREY-Cyclic", "C6-Cyclic", "C7-Cyclic", "C8-Cyclic", "C9-Cyclic",
                  "R1_RAINBOW", "R2_RAINBOW2", "R3_RAINBOW3",
                  "CBC1-Blind_red-green", "CBC2-Blind_red-green", "CBD1-Blind_red-green",
                  "CBD2-Blind_red-green", "CBL1-Blind_red-green", "CBL2-Blind_red-green",
                  "CBL3-Blind_red-green", "CBL4-Blind_red-green", "CBTC1-Blind_blue-yellow",
                  "CBTC2-Blind_blue-yellow", "CBTD1-Blind_blue-yellow", "CBTL1-Blind_blue-yellow",
                  "CBTL2-Blind_blue-yellow", "CBTL3-Blind_blue-yellow", "CBTL4-Blind_blue-yellow"]

const _PAL_T = ["Mag - anomaly", "SeaLand (m)", "Bathymetry (m)", "Topography (m)",
                "SST (12-26)", "SST (0-20)", "SST (0-35)", "Chlor (0-10)"]

# ---------------------------------------------------------------------------------------------
# data/palettes.bin — "IGMTPAL1", UInt16 count, then per palette: UInt8 name length, name bytes,
# UInt16 row count, rows*3 UInt8 RGB. Read once, cached. Values come back as 0..1 doubles.
const _PAL_BIN   = joinpath(_PKGROOT, "data", "palettes.bin")
const _PAL_CACHE = Ref{Union{Nothing,Dict{String,Matrix{Float64}}}}(nothing)

# Parsed from ONE byte array with plain integer indexing, not from an IOStream: the stream version
# (read(io, T) / read! / do-block) cost 0.389 s to compile on first use, which the user sees as the
# dialog hesitating. Bytes in, indices out — nothing generic to specialise.
function _pal_bin()::Dict{String,Matrix{Float64}}
	_PAL_CACHE[] === nothing || return _PAL_CACHE[]
	D = Dict{String,Matrix{Float64}}()
	if isfile(_PAL_BIN)
		b = read(_PAL_BIN)::Vector{UInt8}
		u16(i) = Int(b[i]) | (Int(b[i+1]) << 8)                  # little-endian, as written
		length(b) > 10 && String(b[1:8]) == "IGMTPAL1" || error("data/palettes.bin: bad magic")
		npal = u16(9)
		p = 11
		for _ in 1:npal
			ln = Int(b[p]);  p += 1
			nm = String(b[p:p+ln-1]);  p += ln
			nr = u16(p);  p += 2
			D[nm] = permutedims(reshape(b[p:p+3nr-1], 3, nr)) ./ 255
			p += 3nr
		end
	else
		@warn "InteractiveGMT: data/palettes.bin missing — CET/CAR/GIMP palettes unavailable"
	end
	_PAL_CACHE[] = D
	return D
end

_pal_stored(key::String)::Matrix{Float64} = get(_pal_bin(), key, zeros(0, 3))

# ---------------------------------------------------------------------------------------------
# The ML (MATLAB) formula colormaps, ported from color_palettes.m's own copies (lines 1401-1514)
# and, for the three it leaves to MATLAB itself, from MATLAB's definitions of hot/hsv/jet.
_pal_ramp(m::Int) = collect(0:m-1) ./ max(m - 1, 1)

_pal_gray(m::Int)   = repeat(_pal_ramp(m), 1, 3)
_pal_autumn(m::Int) = hcat(ones(m), _pal_ramp(m), zeros(m))
_pal_cool(m::Int)   = (r = _pal_ramp(m); hcat(r, 1 .- r, ones(m)))
_pal_summer(m::Int) = (r = _pal_ramp(m); hcat(r, 0.5 .+ r ./ 2, fill(0.4, m)))
_pal_winter(m::Int) = (r = _pal_ramp(m); hcat(zeros(m), r, 0.5 .+ (1 .- r) ./ 2))
_pal_copper(m::Int) = min.(1.0, _pal_gray(m) .* [1.2500 0.7812 0.4975])

# hot: MATLAB's own definition (color_palettes.m calls it straight).
function _pal_hot(m::Int)
	n = trunc(Int, 3 / 8 * m)
	r = vcat((1:n) ./ n, ones(m - n))
	g = vcat(zeros(n), (1:n) ./ n, ones(max(m - 2n, 0)))
	b = vcat(zeros(2n), (1:(m - 2n)) ./ max(m - 2n, 1))
	return hcat(r[1:m], g[1:m], b[1:m])
end

_pal_bone(m::Int) = (7 .* _pal_gray(m) .+ _pal_hot(m)[:, [3, 2, 1]]) ./ 8      # fliplr(hot)
_pal_pink(m::Int) = sqrt.((2 .* _pal_gray(m) .+ _pal_hot(m)) ./ 3)

# hsv: MATLAB's hsv(m) = hsv2rgb with h = (0:m-1)/m, s = v = 1.
function _pal_hsv(m::Int)
	out = zeros(m, 3)
	for i in 1:m
		h = 6 * (i - 1) / m
		k = floor(Int, h); f = h - k
		p, q, t = 0.0, 1 - f, f
		rgb = k == 0 ? (1.0, t, p) : k == 1 ? (q, 1.0, p) : k == 2 ? (p, 1.0, t) :
		      k == 3 ? (p, q, 1.0) : k == 4 ? (t, p, 1.0) : (1.0, p, q)
		out[i, 1], out[i, 2], out[i, 3] = rgb
	end
	return out
end

# jet: MATLAB's own construction (a hsv-derived ramp with the r/g/b bands offset by n).
function _pal_jet(m::Int)
	n = ceil(Int, m / 4)
	u = vcat((1:n) ./ n, ones(n - 1), reverse((1:n) ./ n))
	g0 = ceil(Int, n / 2) - (mod(m, 4) == 1 ? 1 : 0)
	g = [g0 + i for i in 1:length(u)]
	r = g .+ n
	b = g .- n
	J = zeros(m, 3)
	for (k, idx) in enumerate(g); 1 <= idx <= m && (J[idx, 2] = u[k]); end
	for (k, idx) in enumerate(r); 1 <= idx <= m && (J[idx, 1] = u[k]); end
	for (k, idx) in enumerate(b); 1 <= idx <= m && (J[idx, 3] = u[k]); end
	return J
end

# flag / prism: ceil(m/N) stacked copies of a fixed N-colour block, trimmed to m rows.
function _pal_cycle(block::Matrix{Float64}, m::Int)
	nb = size(block, 1)
	out = zeros(m, 3)
	for i in 1:m; out[i, :] = block[mod(i - 1, nb) + 1, :]; end
	return out
end
_pal_flag(m::Int)  = _pal_cycle([1.0 0 0; 1 1 1; 0 0 1; 0 0 0], m)
_pal_prism(m::Int) = _pal_cycle([1.0 0 0; 1 0.5 0; 1 1 0; 0 1 0; 0 0 1; 2/3 0 1], m)

# lines: MATLAB's default axes ColorOrder — the SEVEN-colour set of the MATLAB releases Mirone
# targets (the 2014b redesign replaced it, but this tool predates that and its `lines` is this one).
_pal_lines(m::Int) = _pal_cycle([0.0 0 1; 0 0.5 0; 1 0 0; 0 0.75 0.75; 0.75 0 0.75;
                                 0.75 0.75 0; 0.25 0.25 0.25], m)

# colorcube: the RGB cube minus its greys and pure colours, then r/g/b/grey ramps filling what is
# left. Ported step by step from color_palettes.m:1455-1514, MATLAB's meshgrid order included (g
# runs fastest, then r, then b — that is what `[r(:) g(:) b(:)]` on meshgrid output gives).
function _pal_colorcube(m::Int)
	nrg = trunc(Int, cbrt(m) + eps())
	nb  = (m - nrg^3 == 0 && nrg > 2) ? nrg - 1 : nrg
	rgstep = 1 / (nrg - 1);  bstep = 1 / (nb - 1)
	rows = Vector{NTuple{3,Float64}}()
	for k in 0:nb-1, j in 0:nrg-1, i in 0:nrg-1        # b outer, r middle, g inner
		push!(rows, (j * rgstep, i * rgstep, k * bstep))
	end
	keep = filter(rows) do (r, g, b)
		(abs(g - r) + abs(b - g)) != 0 &&               # not a grey
		min(r + g, g + b, r + b) != 0                   # not a pure colour
	end
	remlen  = m - length(keep) - 1
	rgbn    = fld(remlen, 4)
	kn      = remlen - 3 * rgbn
	out = Vector{NTuple{3,Float64}}(undef, 0)
	append!(out, keep)
	for i in 1:rgbn; push!(out, (i / rgbn, 0.0, 0.0)); end
	for i in 1:rgbn; push!(out, (0.0, i / rgbn, 0.0)); end
	for i in 1:rgbn; push!(out, (0.0, 0.0, i / rgbn)); end
	push!(out, (0.0, 0.0, 0.0))
	for i in 1:kn; push!(out, (i / kn, i / kn, i / kn)); end
	M = zeros(length(out), 3)
	for (i, t) in enumerate(out); M[i, 1], M[i, 2], M[i, 3] = t; end
	return M
end

# vivid (Joseph Kirk's, shipped inside color_palettes.m): ns shades of each of nc spectrum colours,
# intensity swept between the minmax bounds. Defaults are the .m's: 8 colours, [0.15 0.85].
function _pal_vivid(m::Int)
	clrs = [1.0 0 0; 1 0.5 0; 1 1 0; 0 1 0; 0 1 1; 0 0 1; 0.5 0 1; 1 0 1]
	minmax = (0.15, 0.85)
	nc = size(clrs, 1)
	ns = cld(m, nc)
	n  = nc * ns
	d  = n - m
	sup = (2minmax[1], 2minmax[2]);  sub = (2minmax[1] - 1, 2minmax[2] - 1)
	high = ns == 1 ? [min(1.0, sup[1])] : [min(1.0, sup[1] + (sup[2] - sup[1]) * (i - 1) / (ns - 1)) for i in 1:ns]
	low  = ns == 1 ? [max(0.0, sub[1])] : [max(0.0, sub[1] + (sub[2] - sub[1]) * (i - 1) / (ns - 1)) for i in 1:ns]
	fl = clrs[end:-1:1, :]                                  # flipud(clrs)
	M = zeros(n, 3)
	for j in 1:nc, i in 1:ns                                # reshape(map, n, 3): shade fastest
		row = i + (j - 1) * ns
		for c in 1:3
			M[row, c] = fl[j, c] * high[i] + (1 - fl[j, c]) * low[i]
		end
	end
	drop = [1 + (k - 1) * ns for k in 1:d]                  # cmap(1:ns:d*ns,:) = []
	return isempty(drop) ? M : M[setdiff(1:n, drop), :]
end

# A named ML palette, m rows. EVERY one of them is stored data (data/palettes.bin), including the
# formula ones: a colormap is a table of colours, so there is no reason to rebuild it at runtime —
# and rebuilding it means Julia compiling sixteen array-building functions the first time the user
# clicks a name (1.31 s, measured; no single one to blame, they are 50-260 ms each). The formulas
# below are the GENERATORS that produced those rows, kept for `_build_palette_bin` and as the
# fallback if the file is missing, exactly like the .m tables they sit beside.
function _pal_ml(name::String, m::Int = 256)::Matrix{Float64}
	P = _pal_stored("ML/" * name)
	isempty(P) || return _pal_resample(P, m)
	return Base.invokelatest(_pal_ml_formula, name, m)::Matrix{Float64}
end

# The generators. Only reached when data/palettes.bin lacks the name (and by the builder below);
# invokelatest keeps their compile off the normal path even then.
@noinline function _pal_ml_formula(name::String, m::Int = 256)::Matrix{Float64}
	name == "autumn"       && return _pal_autumn(m)
	name == "bone"         && return _pal_bone(m)
	name == "colorcube"    && return _pal_colorcube(m)
	name == "cool"         && return _pal_cool(m)
	name == "copper"       && return _pal_copper(m)
	name == "flag"         && return _pal_flag(m)
	name == "gray"         && return _pal_gray(m)
	name == "hot"          && return _pal_hot(m)
	name == "hsv"          && return _pal_hsv(m)
	name == "jet"          && return _pal_jet(m)
	name == "lines"        && return _pal_lines(m)
	name == "pink"         && return _pal_pink(m)
	name == "prism"        && return _pal_prism(m)
	name == "summer"       && return _pal_summer(m)
	name == "winter"       && return _pal_winter(m)
	name == "vivid"        && return _pal_vivid(m)
	P = _pal_stored("ML/" * name)                          # parula viridis magma inferno turbo jet-improved
	return isempty(P) ? zeros(0, 3) : _pal_resample(P, m)
end

# ---------------------------------------------------------------------------------------------
# Linear resample of an Nx3 palette to m rows (the `interp1(linspace…)` Mirone uses everywhere).
function _pal_resample(P::Matrix{Float64}, m::Int)::Matrix{Float64}
	n = size(P, 1)
	(n == m || n < 2) && return P
	out = zeros(m, 3)
	for i in 1:m
		t = (i - 1) * (n - 1) / max(m - 1, 1)
		k = clamp(floor(Int, t) + 1, 1, n - 1)
		f = t - (k - 1)
		for c in 1:3; out[i, c] = P[k, c] * (1 - f) + P[k+1, c] * f; end
	end
	return out
end

# A GMT master CPT as 256 RGB rows. Mirone read these from gmt_other_palettes.mat; here they are
# GMT's own, sampled the way `_cpt_nodes_range` samples any CPT (drop makecpt's trailing
# foreground row, reuse the last real colour for the top boundary).
@noinline function _pal_gmt(name::String, m::Int = 256)::Matrix{Float64}
	startswith(name, "DEM_") && return _pal_resample(_pal_stored("MIR/" * name), m)
	try
		C = GMT.makecpt(cmap = name, range = (0, 255, 255 / (m - 1)), continuous = true)
		cm = Float64.(C.colormap)
		nseg = size(C.range, 1)
		size(cm, 1) > nseg && (cm = cm[1:nseg, :])
		cm = vcat(cm, cm[end:end, :])
		return _pal_resample(cm[:, 1:3], m)
	catch e
		@warn "Color Palettes: GMT master CPT '$name' failed" exception = (e,)
		return zeros(0, 3)
	end
end

# ---------------------------------------------------------------------------------------------
# The Thematic palettes (color_palettes.m `thematic_pal`). Returns (pal, zlo, zhi, hinge, islog):
#   zlo/zhi  the z range the palette is DEFINED over (NaN = use the grid's own min/max)
#   hinge    the palette row where the coastline discontinuity sits (0 = none). The dialog runs
#            makeCmapBat against the grid's real min/max for these — that is Mirone's own path.
#   islog    the mapping is logarithmic over [zlo,zhi] (the Chlor table).
function _pal_thematic(name::String)
	if startswith(name, "Mag")
		# z_intervals -800 … 800; the palette spreads linearly across that fixed window.
		return (_pal_stored("MIR/mag"), -800.0, 800.0, 0, false)
	elseif startswith(name, "SeaLand")
		return (_pal_stored("MIR/Terre_Mer"), NaN, NaN, 147, false)
	elseif startswith(name, "Bathymetry")
		return (_pal_stored("CAR/Earth"), -6000.0, 0.0, 0, false)
	elseif startswith(name, "Topography")
		return (_pal_stored("CAR/Topographic"), NaN, NaN, 1, false)
	elseif name == "SST (12-26)"
		return (_pal_ml("jet", 255), 12.0, 26.0, 0, false)
	elseif name == "SST (0-20)"
		return (_pal_ml("jet", 255), 0.0, 20.0, 0, false)
	elseif name == "SST (0-35)"
		return (_pal_ml("jet", 255), 0.0, 35.0, 0, false)
	elseif name == "Chlor (0-10)"
		return (_pal_ml("jet", 255), 0.0, 10.0, 0, true)
	end
	for (nm, path) in _pal_custom_thematic()                # OPTcontrol.txt's MIR_CPT, iGMT-side
		nm == name || continue
		# invokelatest, not a plain call: this is the ONLY reference to the .cpt reader from a path
		# the dialog takes when it opens, and inference following it dragged gmtread/GDAL into the
		# compile of every ordinary palette pick (1.68 s, measured). A user's custom CPT compiles it
		# the first time one is actually chosen.
		P, zlo, zhi = Base.invokelatest(_pal_read_cpt, path, true, 256)
		return (P, zlo, zhi, 0, false)
	end
	return (zeros(0, 3), NaN, NaN, 0, false)
end

# Mirone's OPTcontrol.txt `MIR_CPT <name> <file>` lines, which add user CPTs to the Thematic list.
# iGMT keeps its preferences in ~/.gmt/iGMT.ini, so the same feature reads a [ColorPalettes]
# section there: one `name = path.cpt` line per custom palette.
function _pal_custom_thematic()::Vector{Pair{String,String}}
	out = Pair{String,String}[]
	for (k, v) in _ini_section("ColorPalettes")
		isfile(v) && push!(out, k => v)
	end
	return out
end

# ---------------------------------------------------------------------------------------------
# CPT files. `use_z` mirrors Mirone's "Use Z levels" vs "As master": with Z levels the palette is
# resampled over the CPT's OWN z range (so the grid gets mapped into the levels the file defines);
# as master only the colours are taken and the z range stays whatever the window already had.
@noinline function _pal_read_cpt(path::String, use_z::Bool, m::Int = 256)
	C = GMT.gmtread(path)
	rg = Float64.(C.range)
	zlo, zhi = use_z ? (rg[1, 1], rg[end, end]) : (NaN, NaN)
	lo, hi = use_z ? (rg[1, 1], rg[end, end]) : (0.0, 255.0)
	P = try
		Cr = GMT.makecpt(cmap = path, range = (lo, hi, (hi - lo) / (m - 1)), continuous = true)
		cm = Float64.(Cr.colormap)
		nseg = size(Cr.range, 1)
		size(cm, 1) > nseg && (cm = cm[1:nseg, :])
		_pal_resample(vcat(cm, cm[end:end, :])[:, 1:3], m)
	catch                                                   # discrete/odd CPT: use its own colours
		cm = Float64.(C.colormap)
		maximum(cm) > 1.0 && (cm = cm ./ 255)
		_pal_resample(cm[:, 1:3], m)
	end
	return (P, zlo, zhi)
end

# Write the palette as a GMT CPT. Ported from FileSavePalette_CB (color_palettes.m:946):
#   mode "grid256"     — one slice per palette row over [zmin,zmax] (a 256-colour discrete file)
#   mode "master_disc" — 16 discrete slices over 0..1
#   mode "master_cont" — 15 continuous slices over 0..1
@noinline function _pal_save_cpt(path::String, P::Matrix{Float64}, mode::String, zmin::Float64, zmax::Float64)
	master = (mode == "master_disc" || mode == "master_cont")
	pal    = master ? _pal_resample(P, 16) : P
	np     = size(pal, 1)
	dz     = master ? 1 / np : (isfinite(zmin) && isfinite(zmax) && zmax > zmin ? (zmax - zmin) / np : 1 / np)
	z0     = master ? 0.0 : (isfinite(zmin) ? zmin : 0.0)
	c(i, k) = round(Int, clamp(pal[i, k] * 255, 0, 255))
	open(path, "w") do io
		println(io, "# Color palette exported by InteractiveGMT")
		println(io, "# COLOR_MODEL = RGB")
		if mode == "master_cont"
			for i in 1:np-1
				println(io, string(round(z0 + dz * (i - 1), digits = 4), "\t", c(i, 1), "\t", c(i, 2), "\t", c(i, 3),
				                   "\t", round(z0 + dz * i, digits = 4), "\t", c(i+1, 1), "\t", c(i+1, 2), "\t", c(i+1, 3)))
			end
		else
			for i in 1:np
				rgbs = string(c(i, 1), "\t", c(i, 2), "\t", c(i, 3))
				println(io, string(round(z0 + dz * (i - 1), digits = 4), "\t", rgbs, "\t",
				                   round(z0 + dz * i, digits = 4), "\t", rgbs))
			end
		end
		println(io, "F\t255\t255\t255")
		println(io, "B\t0\t0\t0")
		println(io, "N\t128\t128\t128")
	end
	return nothing
end

# ---------------------------------------------------------------------------------------------
# CIE76 ΔE* along the palette (color_palettes.m:1361, from Peter Kovesi's colourmap functions):
# central differences of the L*a*b* coordinates, weighted. W = (1,1,1) gives ΔE*, (1,0,0) the
# lightness difference alone. Plotted in an X,Y plot window (Mirone plots it in `ecran`).
function _rgb_to_lab(P::Matrix{Float64})
	f(t) = t > 0.008856451679035631 ? cbrt(t) : (7.787037037037035 * t + 16 / 116)
	g(u) = u <= 0.04045 ? u / 12.92 : ((u + 0.055) / 1.055)^2.4      # sRGB -> linear
	L = zeros(size(P, 1), 3)
	for i in 1:size(P, 1)
		r, gg, b = g(P[i, 1]), g(P[i, 2]), g(P[i, 3])
		X = (0.4124564r + 0.3575761gg + 0.1804375b) / 0.95047        # D65 white
		Y = (0.2126729r + 0.7151522gg + 0.0721750b)
		Z = (0.0193339r + 0.1191920gg + 0.9503041b) / 1.08883
		fx, fy, fz = f(X), f(Y), f(Z)
		L[i, 1] = 116fy - 16;  L[i, 2] = 500 * (fx - fy);  L[i, 3] = 200 * (fy - fz)
	end
	return L
end

function _pal_cie76(P::Matrix{Float64}, W::NTuple{3,Float64})
	lab = _rgb_to_lab(P)
	n = size(lab, 1)
	n < 3 && return (Float64[], Float64[])
	d = zeros(n, 3)
	for c in 1:3
		d[1, c]   = lab[2, c] - lab[1, c]
		d[n, c]   = lab[n, c] - lab[n-1, c]
		for i in 2:n-1;  d[i, c] = (lab[i+1, c] - lab[i-1, c]) / 2;  end
	end
	dE = [sqrt(W[1] * d[i, 1]^2 + W[2] * d[i, 2]^2 + W[3] * d[i, 3]^2) for i in 1:n]
	return (collect(1.0:n), dE)
end

# ---------------------------------------------------------------------------------------------
# THE C CALLBACK. One entry point, one request string:
#   list;<fam>                          -> txt = names, one per line                    (ret 0)
#   pal;<fam>;<name>                    -> rgb rows; txt = "zlo zhi hinge log"  (ret nrows)
#   readcpt;<master|zlevels>;<path>     -> rgb rows; txt = "zlo zhi hinge log"  (ret nrows)
#   savecpt;<mode>;<path>;<zmin>;<zmax> -> consumes the rgb rows handed in       (ret 0)
#   cie76;<w1w2w3>                      -> consumes the rgb rows, opens an X,Y plot (ret 0)
# `fam` is ML|GMT|CET|CAR|GIMP|T. A negative return is an error whose |ret| bytes are in txt.
const _PAL_LAST = Ref{Matrix{Float64}}(zeros(0, 3))     # last palette handed out (Save Session/debug)

_pal_names(fam::String)::Vector{String} =
	fam == "ML"   ? _PAL_ML   : fam == "GMT"  ? _PAL_GMT :
	fam == "CET"  ? _PAL_CET  : fam == "CAR"  ? _PAL_CAR :
	fam == "GIMP" ? _PAL_GIMP : fam == "T"    ? vcat(_PAL_T, first.(_pal_custom_thematic())) : String[]

# One palette by family + name, as (rows, zlo, zhi, hinge, islog).
function _pal_fetch(fam::String, name::String, m::Int)
	fam == "ML"   && return (_pal_ml(name, m), NaN, NaN, 0, false)
	fam == "GMT"  && return (Base.invokelatest(_pal_gmt, name, m), NaN, NaN, 0, false)
	fam == "CET"  && return (_pal_resample(_pal_stored("CET/" * split(name, '-')[1]), m), NaN, NaN, 0, false)
	fam == "CAR"  && return (_pal_resample(_pal_stored("CAR/" * strip(replace(name, "CAR --" => ""))), m), NaN, NaN, 0, false)
	fam == "GIMP" && return (_pal_resample(_pal_stored("GIMP/" * name), m), NaN, NaN, 0, false)
	fam == "T"    && return _pal_thematic(name)
	return (zeros(0, 3), NaN, NaN, 0, false)
end

_pal_puttxt(txt::Ptr{UInt8}, cap::Cint, s::String) = begin
	b = codeunits(s)
	n = min(length(b), Int(cap) - 1)
	for i in 1:n; unsafe_store!(txt, b[i], i); end
	unsafe_store!(txt, UInt8(0), n + 1)
	n
end

function _pal_putrgb(rgb::Ptr{Float64}, cap::Cint, P::Matrix{Float64})::Int
	n = min(size(P, 1), Int(cap))
	for i in 1:n, c in 1:3
		unsafe_store!(rgb, clamp(P[i, c], 0.0, 1.0), 3 * (i - 1) + c)
	end
	return n
end

_pal_getrgb(rgb::Ptr{Float64}, n::Cint)::Matrix{Float64} =
	permutedims(reshape(unsafe_wrap(Array, rgb, 3 * Int(n); own = false), 3, Int(n)))

# COMPILE COST IS THE POINT OF THIS SHAPE. Opening the dialog makes exactly one call in here
# ("list;<fam>"), and picking a name makes one more ("pal;<fam>;<name>"). Written as a single
# function with every operation inline, that first call cost 2.28 s: inference follows EVERY branch
# it can see, so opening the dialog compiled `makecpt` (the GMT family), `gmtread`/`gmtwrite` (cpt
# I/O) and the whole X,Y plot stack (CIE76) before showing a single colour. Each of those now sits
# behind its own `@noinline` function reached through `invokelatest`, which is an inference barrier:
# the open path compiles the open path, and a rarely-used branch pays for itself the first time it
# is actually used. Do not "simplify" this back into one function.
function _on_palette(scene::Ptr{Cvoid}, creq::Cstring, rgb::Ptr{Float64}, nrows::Cint,
                     txt::Ptr{UInt8}, txtcap::Cint)::Cint
	try
		req = split(unsafe_string(creq), ';')
		op  = String(req[1])
		if op == "list"
			_pal_puttxt(txt, txtcap, join(_pal_names(String(req[2])), '
'))
			return Cint(0)
		elseif op == "pal"
			# Barrier here too: opening the dialog only ever asks for a name LIST, and inference
			# following a direct call compiled the whole fetch side (palettes.bin reader included)
			# before the window appeared. The first PICK compiles what a pick needs.
			return Base.invokelatest(_pal_op_pal, String(req[2]), String(req[3]), rgb, nrows, txt, txtcap)
		end
		return Base.invokelatest(_pal_op_file, op, req, rgb, nrows, txt, txtcap)
	catch e
		return _pal_err(txt, txtcap, "Color Palettes: " * sprint(showerror, e))
	end
end

# "pal;<fam>;<name>". The five table/formula families answer from memory; only the GMT one needs
# makecpt, so it goes through the barrier and the ML/CET/CAR/GIMP/Thematic picks stay compile-free.
function _pal_op_pal(fam::String, name::String, rgb::Ptr{Float64}, nrows::Cint,
                     txt::Ptr{UInt8}, txtcap::Cint)::Cint
	P, zlo, zhi, hinge, islog =
		fam == "GMT" ? (Base.invokelatest(_pal_gmt, name, Int(nrows)), NaN, NaN, 0, false) :
		               _pal_fetch(fam, name, Int(nrows))
	isempty(P) && return _pal_err(txt, txtcap, "palette '$name' not available")
	_PAL_LAST[] = P
	_pal_puttxt(txt, txtcap, "$zlo $zhi $hinge $(islog ? 1 : 0)")
	return Cint(_pal_putrgb(rgb, nrows, P))
end

# Everything that touches a file or opens a plot: readcpt / savecpt / cie76. Never on the open path.
@noinline function _pal_op_file(op::String, req::Vector{SubString{String}}, rgb::Ptr{Float64},
                                nrows::Cint, txt::Ptr{UInt8}, txtcap::Cint)::Cint
	if op == "readcpt"
		P, zlo, zhi = _pal_read_cpt(String(req[3]), String(req[2]) == "zlevels", Int(nrows))
		isempty(P) && return _pal_err(txt, txtcap, "could not read $(req[3])")
		_PAL_LAST[] = P
		_pal_puttxt(txt, txtcap, "$zlo $zhi 0 0")
		return Cint(_pal_putrgb(rgb, nrows, P))
	elseif op == "savecpt"
		_pal_save_cpt(String(req[3]), _pal_getrgb(rgb, nrows), String(req[2]),
		              parse(Float64, req[4]), parse(Float64, req[5]))
		return Cint(0)
	elseif op == "cie76"
		w = String(req[2])
		W = (parse(Float64, w[1:1]), parse(Float64, w[2:2]), parse(Float64, w[3:3]))
		x, y = _pal_cie76(_pal_getrgb(rgb, nrows), W)
		isempty(y) && return _pal_err(txt, txtcap, "palette too short for CIE76")
		p = xyplot(x, y; name = W[2] == 0 ? "CIE76 lightness diff" : "CIE76 Delta E*", color = :black)
		_xy_set_labels(p, "colormap index", W[2] == 0 ? "dL*" : "Delta E*")
		return Cint(0)
	end
	return _pal_err(txt, txtcap, "unknown request '$op'")
end

_pal_err(txt::Ptr{UInt8}, cap::Cint, msg::String)::Cint = Cint(-_pal_puttxt(txt, cap, msg))

function _register_palette()
	fptr = @cfunction((s, r, p, n, t, c) -> Base.invokelatest(_on_palette, s, r, p, n, t, c),
	                  Cint, (Ptr{Cvoid}, Cstring, Ptr{Float64}, Cint, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_palette_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# ---------------------------------------------------------------------------------------------
# PROVENANCE / one-time builder of data/palettes.bin. Not called at runtime — kept so the file can
# be regenerated from a Mirone checkout. `mirdata` is Mirone's data/ dir, `srcfigs` its src_figs/
# (for the five 256-row tables color_palettes.m carries) and `utils` its utils/ (mkpj.m).
#
#   InteractiveGMT._build_palette_bin("C:/SVN/mironeWC")
#
# Reads the .mat files with a minimal uncompressed MATLAB v5 parser (128-byte header, then
# miMATRIX elements: array flags, dimensions, name, real part; 8-byte tags with a small-element
# variant), scales everything to 0-255 uint8 and writes the "IGMTPAL1" container `_pal_bin` reads.
function _build_palette_bin(mirone_root::String, out::String = _PAL_BIN)
	mid = Dict(1 => Int8, 2 => UInt8, 3 => Int16, 4 => UInt16, 5 => Int32, 6 => UInt32,
	           7 => Float32, 9 => Float64, 12 => Int64, 13 => UInt64, 16 => UInt8, 18 => UInt8)
	function rdelem(io)
		w = read(io, UInt32);  hi = w >> 16
		typ, nb, small = hi != 0 ? (Int(w & 0xFFFF), Int(hi), true) : (Int(w), Int(read(io, UInt32)), false)
		if typ == 14                                               # miMATRIX
			stop = position(io) + nb
			flags = rdelem(io)[2];  dims = rdelem(io)[2]
			name  = String(UInt8.(rdelem(io)[2]));  pr = rdelem(io)[2]
			seek(io, stop)
			return (name, (Int.(dims), pr))
		end
		T = mid[typ]
		v = read!(io, Vector{T}(undef, nb ÷ sizeof(T)))
		small ? skip(io, 4 - nb) : skip(io, (8 - (nb % 8)) % 8)
		return (typ, v)
	end
	function readmat5(path)
		io = open(path);  read(io, 128);  D = Dict{String,Any}()
		while !eof(io)
			nm, val = rdelem(io)
			nm isa String && (D[nm] = val)
		end
		close(io);  D
	end
	asmat(v) = reshape(Float64.(v[2]), v[1][1], v[1][2])       # (dims, realpart) -> n x 3
	to255(M) = round.(UInt8, clamp.(maximum(M) <= 1.0 ? M .* 255 : M, 0, 255))
	# The 256-row tables written out inside the .m files themselves.
	function mtable(path, header)
		txt = readlines(path)
		i = findfirst(l -> occursin(header, l), txt)
		rows = NTuple{3,Float64}[];  started = false
		for k in i:length(txt)
			l = strip(txt[k])
			if !started
				occursin("[", l) && (started = true)
				l = replace(l, r"^.*\[" => "")
			end
			isempty(l) && continue
			nums = [parse(Float64, mm.match) for mm in eachmatch(r"[-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?",
			                                                     replace(l, "]" => "", ";" => ""))]
			length(nums) == 3 && push!(rows, (nums[1], nums[2], nums[3]))
			occursin("]", l) && started && length(rows) > 1 && break
		end
		hcat(getindex.(rows, 1), getindex.(rows, 2), getindex.(rows, 3))
	end
	d   = joinpath(mirone_root, "data");  cpm = joinpath(mirone_root, "src_figs", "color_palettes.m")
	car = readmat5(joinpath(d, "caris256.mat"));   gimp = readmat5(joinpath(d, "gimp256.mat"))
	cet = readmat5(joinpath(d, "CETperceptual.mat")); oth = readmat5(joinpath(d, "gmt_other_palettes.mat"))
	entries = Pair{String,Matrix{UInt8}}[]
	for (lbl, hdr) in ("parula" => "function c = parola(m)", "viridis" => "function c = viridis(m)",
	                   "magma" => "function c = magma(m)", "inferno" => "function c = inferno(m)",
	                   "turbo" => "function c = turbo(m)")
		push!(entries, "ML/" * lbl => to255(mtable(cpm, hdr)))
	end
	push!(entries, "ML/jet-improved" => to255(mtable(joinpath(mirone_root, "utils", "mkpj.m"), "JetI =")))
	# The formula colormaps, evaluated ONCE at 256 rows and stored like every other palette — a
	# colormap is data, and generating it at runtime only buys the user the compile time of sixteen
	# array-building functions. `_pal_ml_formula` above stays as the generator these rows came from.
	for n in ("autumn", "bone", "colorcube", "cool", "copper", "flag", "gray", "hot", "hsv", "jet",
	          "lines", "pink", "prism", "summer", "winter", "vivid")
		push!(entries, "ML/" * n => to255(_pal_ml_formula(n, 256)))
	end
	for n in _PAL_CAR;  push!(entries, "CAR/"  * strip(replace(n, "CAR --" => "")) => to255(asmat(car[strip(replace(n, "CAR --" => ""))])));  end
	for n in _PAL_GIMP; push!(entries, "GIMP/" * n => to255(asmat(gimp[n])));  end
	for n in _PAL_CET;  v = String(split(n, '-')[1]);  push!(entries, "CET/" * v => to255(asmat(cet[v])));  end
	for n in ("DEM_screen", "DEM_print", "DEM_poster", "Terre_Mer", "mag", "Terre", "Mer")
		push!(entries, "MIR/" * n => to255(asmat(oth[n])))
	end
	open(out, "w") do io
		write(io, b"IGMTPAL1");  write(io, UInt16(length(entries)))
		for (nm, M) in entries
			b = codeunits(nm)
			write(io, UInt8(length(b)));  write(io, b);  write(io, UInt16(size(M, 1)))
			for i in 1:size(M, 1), j in 1:3;  write(io, M[i, j]);  end
		end
	end
	return length(entries)
end
