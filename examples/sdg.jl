# SDG demo — "Second Derivative in the direction of the Gradient", the Grid Tools > SDG entry
# (port of Mirone's mirone.m GridToolsSDG_CB). SDG was the method originally recommended to pick the
# FOS (foot of the continental slope) for an article-76 (Law of the Sea) shelf extension: the FOS is
# the point of MAXIMUM CHANGE OF GRADIENT at the base of the slope, which is exactly where the SDG
# peaks.
#
# Self-contained: no network, no data files. The margin below is synthetic, so the answer is known
# in closed form and the demo can check itself.
#
#   depth(x) = -150 - 4650·½(1 + tanh((x - xc)/w))       shelf -150 m  ->  abyssal plain -4800 m
#
# whose second derivative peaks at  x = xc + w·atanh(1/√3),  i.e. 16.46 km seaward of the slope's
# inflexion — that is the FOS this demo must recover.
#
# It also shows WHY the `p` parameter exists: the same margin with a little sea-floor roughness is
# hopeless at p = 1 (an interpolating spline reproduces the noise, and its second derivative is all
# noise) and clean at the default p.
#
# Run:   julia examples/sdg.jl        or in the REPL:  include("examples/sdg.jl")

using InteractiveGMT
using GMT

const IG = InteractiveGMT

# --- a synthetic passive margin, in km (Cartesian: no geographic subtleties in the way) ----------
xc, w = 150.0, 25.0                                  # slope centre and half-width
x  = collect(0.0:0.5:300.0)
y  = collect(0.0:0.5:60.0)                           # square cells, as a real bathymetry grid has
depth(a) = -150 - 4650 * 0.5 * (1 + tanh((a - xc) / w))
FOS_TRUE = xc + w * atanh(1 / sqrt(3))               # 166.46 km — where d²z/dx² is largest

Z = Float32[depth(a) for _ in y, a in x]
G = GMT.mat2grid(Z; x = x, y = y)

# Deterministic sea-floor roughness (an LCG, so the demo prints the same numbers every run).
Zn = copy(Z)
let seed = UInt32(2024)                              # UInt32 throughout: an Int literal here would
	for i in eachindex(Zn)                           # promote the state and stop it wrapping
		seed = UInt32(1664525) * seed + UInt32(1013904223)
		Zn[i] += Float32(12) * (Float32(seed >> 8) / Float32(1 << 24) - 0.5f0)     # ±6 m
	end
end
Gn = GMT.mat2grid(Zn; x = x, y = y)

# Where does the SDG put the FOS? Read the peak off a profile across the middle of the grid.
pick(R) = x[argmax(view(R.z, size(R.z, 1) ÷ 2, :))]

R_clean = IG.sdg(G)                                   # default p = csaps's own estimate
p_used  = IG.csaps_p_guess(y[1:5])
println("smoothing parameter p (csaps estimate) : ", p_used)
println("FOS, analytic                          : ", round(FOS_TRUE, digits = 2), " km")
println("FOS, SDG on the clean margin           : ", pick(R_clean), " km")
println("FOS, SDG on the rough margin, p = 1    : ", pick(IG.sdg(Gn; p = 1.0)), " km   <- noise wins")
println("FOS, SDG on the rough margin, default p: ", pick(IG.sdg(Gn)), " km")
# The default `p` is only a starting point — GridToolsSDG_CB puts it in an edit box precisely so the
# analyst can turn it down until the FOS stops moving. That sweep is the whole workflow:
for p in (0.99, 0.9, 0.5, 0.1)
	println("   p = ", rpad(p, 6), " -> FOS ", pick(IG.sdg(Gn; p = p)), " km")
end

# Mirone's three menu entries are this same field, sign-masked. `positive` is the one that carries
# the FOS (the slope steepens into the rise); `negative` marks the shelf break at the top.
R_pos = IG.sdg(G; sign = :positive)
R_neg = IG.sdg(G; sign = :negative)
println("shelf break (SDG negative peak)        : ", x[argmin(view(R_neg.z, size(R_neg.z, 1) ÷ 2, :))], " km")

# --- and the same thing in the viewer ------------------------------------------------------------
# In the app this is Grid Tools > SDG > Positive / Negative / Both, which asks for `p` and adds the
# result to the SAME window as a new, checked Scene Object. Here, two plain windows side by side.
fig_bat = view_grid(G;     cmap = :geo,   title = "i'GMT  —  synthetic margin (bathymetry)")
fig_sdg = view_grid(R_pos; cmap = :turbo, title = "i'GMT  —  SDG (positive): the FOS ridge")

# Script mode has no REPL: keep the process alive until the windows close. (The parentheses are not
# decoration: `@__FILE__ && f()` swallows the `&&` into the macro's arguments and fails to parse.)
(abspath(PROGRAM_FILE) == @__FILE__) && wait_windows()
