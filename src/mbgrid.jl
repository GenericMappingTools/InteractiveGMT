# mbgrid.jl — MB-System's MBGRID gridder, as a Julia function over the C in deps/src/mbgrid.c.
#
# Mirone offered "Minimum Curvature - mbgrid" in its Surface window and interpolate.jl had to drop
# it, because unlike surface/nearneighbor/triangulate it is not a GMT module: it lived in the mbgmt
# supplement (joa-quim/mbgmt, gmtmbgrid.c) and needed that plugin built and installed. The numerics
# are now in the viewer DLL with no GMT dependency at all, so it is available wherever gmtvtk loads.
#
# What mbgrid does that no GMT module does: it BINS first (a Gaussian weighted mean over a
# neighbourhood a few cells wide, so dense soundings are averaged rather than fought over by the
# spline), and only then fills what is left. That two-stage shape is why it is the tool for
# multibeam: the along-track density is enormous and the between-swath gaps are what you actually
# want interpolated.
#
# The C side is stateless and allocates nothing the caller does not own — see deps/src/mbgrid.h.

# --- ABI mirror of `mbgrid_params` (deps/src/mbgrid.h). Field ORDER and TYPES are the ABI:
# doubles first, then Int32s. Change one and bump gmtvtk_abi_version (90_c_api.cpp) together with
# _ABI_REQUIRED (libgmtvtk.jl) — that pair is the ONE version guard for everything this DLL
# exports, mbgrid included. ---
struct _MBParams
	west::Cdouble
	east::Cdouble
	south::Cdouble
	north::Cdouble
	xinc::Cdouble
	yinc::Cdouble
	scale::Cdouble
	extend::Cdouble
	tension::Cdouble
	nx::Cint
	ny::Cint
	clipmode::Cint
	clip::Cint
	verbose::Cint
	registration::Cint
end

struct _MBBreakline
	x::Ptr{Cdouble}
	y::Ptr{Cdouble}
	z::Ptr{Cdouble}
	seg_len::Ptr{Cint}
	nseg::Cint
	reserved::Cint
end

struct _MBStats
	zmin::Cdouble
	zmax::Cdouble
	n_data::Int64
	n_set::Int64
	n_spline::Int64
	n_unset::Int64
end

const _MB_INTERP  = Dict{Symbol,Cint}(:none => Cint(0), :gap => Cint(1), :near => Cint(2), :all => Cint(3))
const _MB_LAY_BCB = Cint(0)          # out[ix*ny + iy] — a Julia (ny, nx) Matrix, as-is
# The solved grid handed BACK to mbgrid_fill. GMT.surface returns a "BCB" grid (column-major, y
# ascending), which is layout 2 exactly — so the :surface path transposes nothing on the way in,
# per SACRED_LAW.md's grid memory-layout law. Never permutedims a solver's answer to fit a code.
const _MB_SG_COLMAJOR = Cint(2)

# Both ccall return types this file uses: Cint from the int entry points, Int64 from mbgrid_nodes.
# Two concrete methods, no abstract supertype standing in for them.
_mb_check(rc::Int64)::Nothing = (rc < 0 && error("mbgrid: " *
	unsafe_string(ccall(_fn(:mbgrid_strerror), Cstring, (Cint,), Cint(rc)))); nothing)
_mb_check(rc::Cint)::Nothing = _mb_check(Int64(rc))

# --- coercions. The Interpolate dialog sends every knob as a STRING (one "opt_<kwarg>=<value>"
# line each, 70_window.cpp), the REPL sends numbers and Symbols. Both arrive here, at the one
# function that grids — never at a second, dialog-only wrapper that would drift from this one.
# The String method is the one that dispatches; the fallback is UNANNOTATED (Float64, Int64,
# Float32, whatever the caller typed) rather than named after an abstract supertype.
_mb_num(v::String)::Float64 = (t = String(strip(v)); isempty(t) ? 0.0 : parse(Float64, t))
_mb_num(v)::Float64 = Float64(v)
_mb_int(v::String)::Cint = (t = String(strip(v)); isempty(t) ? Cint(0) : Cint(round(Int, parse(Float64, t))))
_mb_int(v)::Cint = Cint(round(Int, v))
_mb_bool(v::Bool)::Bool = v
_mb_bool(v::String)::Bool = lowercase(String(strip(v))) in ("1", "true", "yes", "on")
_mb_bool(v)::Bool = v != 0
_mb_sym(v::Symbol)::Symbol = v
_mb_sym(v::String)::Symbol = Symbol(lowercase(String(strip(v))))

# Node vs pixel registration, in every spelling the dialog and the REPL use. mbgrid_dims counts
# CELLS for pixel and NODES for gridline, so this is not cosmetic.
_mb_reg(v::Symbol)::Cint = v in (:pixel, :p) ? Cint(1) : v in (:gridline, :grid, :node, :g) ? Cint(0) :
                           error("registration must be :gridline or :pixel — got :$v")
_mb_reg(v::String)::Cint = _mb_reg(_mb_sym(v))
_mb_reg(v)::Cint = Cint(v == 0 ? 0 : 1)

# -R as (w, e, s, n) from whatever the caller had at hand.
_mb_region(r::NTuple{4,Float64}) = r
_mb_region(r::Tuple)::NTuple{4,Float64} = (length(r) == 4 ?
	(Float64(r[1]), Float64(r[2]), Float64(r[3]), Float64(r[4])) :
	error("region needs 4 numbers, got $(length(r))"))
_mb_region(r::Vector{Float64})::NTuple{4,Float64} = _mb_region(Tuple(r))
_mb_region(r::Vector{Int})::NTuple{4,Float64} = _mb_region(Tuple(r))
function _mb_region(r::String)::NTuple{4,Float64}
	v = [parse(Float64, String(s)) for s in split(replace(r, ',' => '/'), '/')]
	length(v) == 4 || error("region string needs 4 numbers, got \"$r\"")
	return (v[1], v[2], v[3], v[4])
end

_mb_inc(i::Tuple)::NTuple{2,Float64} = (Float64(i[1]), Float64(i[length(i) == 1 ? 1 : 2]))
_mb_inc(i::Vector{Float64})::NTuple{2,Float64} = _mb_inc(Tuple(i))
_mb_inc(i::Vector{Int})::NTuple{2,Float64} = _mb_inc(Tuple(i))
function _mb_inc(i::String)::NTuple{2,Float64}
	v = [parse(Float64, String(s)) for s in split(replace(i, ',' => '/'), '/')]
	return length(v) == 1 ? (v[1], v[1]) : (v[1], v[2])
end
_mb_inc(i)::NTuple{2,Float64} = (Float64(i), Float64(i))     # one number = square cells

# x,y,z columns out of anything the dialog or the REPL is likely to hand over.
_mb_xyz(x::Vector{Float64}, y::Vector{Float64}, z::Vector{Float64}) = (x, y, z)
_mb_xyz(x::Vector, y::Vector, z::Vector) =
	(Vector{Float64}(x), Vector{Float64}(y), Vector{Float64}(z))
function _mb_xyz(mat::Matrix)
	size(mat, 2) >= 3 || error("need at least 3 columns (x, y, z), got $(size(mat, 2))")
	return (Vector{Float64}(mat[:, 1]), Vector{Float64}(mat[:, 2]), Vector{Float64}(mat[:, 3]))
end
_mb_xyz(D::GMTdataset) = _mb_xyz(D.data)
_mb_xyz(D::Vector{<:GMTdataset}) = _mb_xyz(reduce(vcat, (d.data for d in D)))

# A breakline is kept as multi-segment: the densifier must not draw a line from the end of one
# segment to the start of the next. A path is read here with the same reader the file drop uses.
_mb_break(::Nothing) = (Float64[], Float64[], Float64[], Cint[])
_mb_break(D::GMTdataset) = _mb_break([D])
_mb_break(f::String) = isempty(strip(f)) ? _mb_break(nothing) : _mb_break(GMT.gmtread(f))
function _mb_break(D::Vector{<:GMTdataset})
	xs, ys, zs, lens = Float64[], Float64[], Float64[], Cint[]
	for d in D
		size(d.data, 2) >= 3 || error("a breakline segment has $(size(d.data, 2)) columns, need 3")
		size(d.data, 1) >= 2 || continue
		append!(xs, d.data[:, 1]); append!(ys, d.data[:, 2]); append!(zs, d.data[:, 3])
		push!(lens, Cint(size(d.data, 1)))
	end
	return (xs, ys, zs, lens)
end
_mb_break(mat::Matrix) = (Vector{Float64}(mat[:, 1]), Vector{Float64}(mat[:, 2]),
                          Vector{Float64}(mat[:, 3]), Cint[Cint(size(mat, 1))])

"""
    mbgrid(data; region, inc, kwargs...) -> GMTgrid

Grid scattered `x, y, z` with MB-System's MBGRID algorithm: a Gaussian-weighted mean into the grid
cells, then a spline through the cells that stayed empty.

`data` may be a `GMTdataset`, a vector of them, an N×3 matrix, or three vectors passed positionally
as `mbgrid(x, y, z; ...)`.

# Required
- `region`: `(w, e, s, n)`, a 4-vector, or a `"w/e/s/n"` string.
- `inc`: one number, `(dx, dy)`, or `"dx/dy"`.

# Options
- `scale = 1.0`     width of the Gaussian, in grid cells (gmtmbgrid's `-W`). Larger = smoother bins.
- `tension = 0.0`   spline tension, 0 = minimum curvature (`-T`). Only used by `solver = :zgrid`.
- `extend = 0.0`    widen the working grid by this fraction of nx/ny so data just outside `region`
                    still constrain the edge, then crop back (`-E`).
- `clipmode`        `:all` (default, fill everything the spline reached), `:near` (only within
                    `clip` cells of data), `:gap` (only gaps with data on opposite sides), `:none`
                    (bin only, leave the gaps NaN). This is `-C`.
- `clip = 0`        the radius in cells that `:near` and `:gap` use.
- `breakline`       a `GMTdataset`, vector of them, an N×3 matrix, or a file path. Nodes the line
                    crosses are pinned to its z, undiluted by nearby soundings — a soft breakline.
- `registration`    `:gridline` (default) or `:pixel`, as everywhere else in GMT.
- `solver`          `:zgrid` (default) the IGPP/SIO thin-plate spline, in C, fast, the one MBGRID
                    itself uses; or `:surface`, which hands the binned nodes to `GMT.surface` and
                    merges its answer back. `:surface` is slower but is GMT's own solver, tension
                    and all; `:zgrid` extrapolates large empty areas more freely, so it can invent
                    dramatic relief far from any data (use `:near` or `:gap` to bound it).
- `verbose = false` spline iteration progress on stderr.

Every option also accepts the string form the Interpolate dialog sends (`"1.5"`, `"near"`).

Returns a `GMTgrid` with `NaN` at every node nothing reached.

# Example
```julia
D = gmtread("soundings.xyz")
G = mbgrid(D; region = (-9.5, -8.5, 36.5, 37.5), inc = 0.002, scale = 1.5, clipmode = :near, clip = 5)
view_grid(G)
```
"""
function mbgrid(x, y, z; kwargs...)
	xx, yy, zz = _mb_xyz(x, y, z)
	return _mbgrid(xx, yy, zz; kwargs...)
end
function mbgrid(data; kwargs...)
	xx, yy, zz = _mb_xyz(data)
	return _mbgrid(xx, yy, zz; kwargs...)
end

function _mbgrid(x::Vector{Float64}, y::Vector{Float64}, z::Vector{Float64};
                 region = nothing, inc = nothing, scale = 1.0, tension = 0.0, extend = 0.0,
                 clipmode = :all, clip = 0, breakline = nothing, solver = :zgrid,
                 registration = :gridline, verbose = false)

	region === nothing && error("mbgrid needs a region")
	inc    === nothing && error("mbgrid needs an inc")
	length(x) == length(y) == length(z) ||
		error("x, y and z have different lengths ($(length(x)), $(length(y)), $(length(z)))")
	cmode = _mb_sym(clipmode)
	haskey(_MB_INTERP, cmode) ||
		error("clipmode must be one of :all, :near, :gap, :none — got :$cmode")
	slv = _mb_sym(solver)
	slv in (:zgrid, :surface) || error("solver must be :zgrid or :surface — got :$slv")

	(w, e, s, n) = _mb_region(region)
	(dx, dy)     = _mb_inc(inc)
	reg = _mb_reg(registration)
	p = _MBParams(w, e, s, n, dx, dy, _mb_num(scale), _mb_num(extend), _mb_num(tension),
	              Cint(0), Cint(0), _MB_INTERP[cmode], _mb_int(clip), Cint(_mb_bool(verbose)), reg)

	nx, ny = Ref{Cint}(0), Ref{Cint}(0)
	_mb_check(ccall(_fn(:mbgrid_dims), Cint, (Ref{_MBParams}, Ref{Cint}, Ref{Cint}), p, nx, ny))

	bx, by, bz, blen = _mb_break(breakline)
	stats = Ref(_MBStats(0, 0, 0, 0, 0, 0))
	# (ny, nx) column-major IS the C side's BCB layout — no transpose anywhere in this path.
	zout = Matrix{Float32}(undef, Int(ny[]), Int(nx[]))

	GC.@preserve x y z bx by bz blen begin
		bl = _MBBreakline(pointer(bx), pointer(by), pointer(bz), pointer(blen),
		                  Cint(length(blen)), Cint(0))
		bl_ref = isempty(blen) ? Ptr{_MBBreakline}(C_NULL) : Ref(bl)
		if slv == :zgrid
			_mb_check(ccall(_fn(:mbgrid_run), Cint,
			                (Ref{_MBParams}, Ptr{Cdouble}, Ptr{Cdouble}, Ptr{Cdouble}, Int64,
			                 Ptr{_MBBreakline}, Ptr{Cfloat}, Cint, Ref{_MBStats}),
			                p, x, y, z, Int64(length(x)), bl_ref, zout, _MB_LAY_BCB, stats))
		else
			_mb_surface_path!(p, x, y, z, bl_ref, zout, stats)
		end
	end

	st = stats[]
	# GMTgrid's own coordinate convention, not the C's: a gridline grid carries nx NODE CENTRES,
	# a pixel grid carries nx+1 CELL BOUNDARIES (mat2grid checks for exactly `nx + reg`). Either
	# way they start at the region's west/south edge — the half-cell inset that the C works with
	# internally (mbgrid_work_origin) is a fact about where the nodes ARE, not about how GMT
	# writes them down.
	xv = collect(range(w, step = dx, length = Int(nx[]) + Int(reg)))
	yv = collect(range(s, step = dy, length = Int(ny[]) + Int(reg)))
	G  = GMT.mat2grid(zout; x = xv, y = yv, reg = Int(reg),
	                  cmd = "InteractiveGMT mbgrid region=$w/$e/$s/$n inc=$dx/$dy scale=$(_mb_num(scale)) " *
	                        "tension=$(_mb_num(tension)) extend=$(_mb_num(extend)) clipmode=$cmode " *
	                        "clip=$(_mb_int(clip)) solver=$slv")
	G.remark = string("mbgrid: ", st.n_set, " nodes from data, ", st.n_spline,
	                  " by ", slv, ", ", st.n_unset, " empty (", st.n_data, " points)")
	return G
end

# The :surface branch. Same binning, same merge rules; only the solver changes. The nodes the
# binner set are handed to GMT.surface as an ordinary x,y,z table over the WORKING region — the
# grid it returns is then merged back through the same clip logic as zgrid's.
function _mb_surface_path!(p::_MBParams, x::Vector{Float64}, y::Vector{Float64}, z::Vector{Float64},
                           bl_ref, zout::Matrix{Float32}, stats::Ref{_MBStats})
	gx, gy = Ref{Cint}(0), Ref{Cint}(0)
	_mb_check(ccall(_fn(:mbgrid_work_dims), Cint,
	                (Ref{_MBParams}, Ref{Cint}, Ref{Cint}, Ptr{Cint}, Ptr{Cint}),
	                p, gx, gy, C_NULL, C_NULL))
	# The working ORIGIN comes from the C, never re-derived here: it is the one place that knows
	# how -E and the registration combine, and a second copy of that arithmetic is exactly the
	# fork SACRED_LAW.md forbids.
	x0, y0 = Ref{Cdouble}(0.0), Ref{Cdouble}(0.0)
	_mb_check(ccall(_fn(:mbgrid_work_origin), Cint, (Ref{_MBParams}, Ref{Cdouble}, Ref{Cdouble}),
	                p, x0, y0))

	work = Vector{Float32}(undef, Int(gx[]) * Int(gy[]))
	nd, ns = Ref{Int64}(0), Ref{Int64}(0)
	_mb_check(ccall(_fn(:mbgrid_bin), Cint,
	                (Ref{_MBParams}, Ptr{Cdouble}, Ptr{Cdouble}, Ptr{Cdouble}, Int64,
	                 Ptr{_MBBreakline}, Ptr{Cfloat}, Ref{Int64}, Ref{Int64}),
	                p, x, y, z, Int64(length(x)), bl_ref, work, nd, ns))

	cnt = ccall(_fn(:mbgrid_nodes), Int64,
	            (Ref{_MBParams}, Ptr{Cfloat}, Ptr{Cdouble}, Ptr{Cdouble}, Ptr{Cdouble}, Int64),
	            p, work, C_NULL, C_NULL, C_NULL, Int64(0))
	_mb_check(cnt)
	xn, yn, zn = Vector{Float64}(undef, cnt), Vector{Float64}(undef, cnt), Vector{Float64}(undef, cnt)
	_mb_check(ccall(_fn(:mbgrid_nodes), Int64,
	                (Ref{_MBParams}, Ptr{Cfloat}, Ptr{Cdouble}, Ptr{Cdouble}, Ptr{Cdouble}, Int64),
	                p, work, xn, yn, zn, cnt))

	# The working region, not the output region: the -E margin has to be solved too or the merge
	# would read past the end of the solution. Node-registered whatever the OUTPUT registration is
	# — the nodes handed over are node centres, and surface is being asked to hit them exactly.
	eg = x0[] + (Int(gx[]) - 1) * p.xinc
	ng = y0[] + (Int(gy[]) - 1) * p.yinc
	Gs = GMT.surface([xn yn zn]; region = (x0[], eg, y0[], ng), inc = (p.xinc, p.yinc),
	                 tension = p.tension)
	isa(Gs, GMTgrid) || error("mbgrid: surface returned a $(typeof(Gs)), not a grid")
	size(Gs.z, 1) == Int(gy[]) && size(Gs.z, 2) == Int(gx[]) ||
		error("mbgrid: surface returned a $(size(Gs.z)) grid, expected $((Int(gy[]), Int(gx[])))")

	# surface's answer goes over AS IT LIES: a (gy, gx) column-major "BCB" matrix is
	# MBGRID_SG_COLMAJOR by definition. Nothing is transposed, nothing is copied. The layout is
	# CHECKED rather than assumed — the whole point of carrying a layout code is that a buffer
	# says what it is instead of a caller believing what it usually is.
	lay = length(Gs.layout) >= 2 ? Gs.layout[1:2] : "BC"
	lay == "BC" || error("mbgrid: surface returned a \"$(Gs.layout)\" grid; the merge wants column-major, south-first (\"BC..\")")
	sgrid = Gs.z isa Matrix{Float32} ? Gs.z : Matrix{Float32}(Gs.z)
	nsp = Ref{Int64}(0)
	GC.@preserve work sgrid begin
		_mb_check(ccall(_fn(:mbgrid_fill), Cint,
		                (Ref{_MBParams}, Ptr{Cfloat}, Ptr{Cfloat}, Cint, Ref{Int64}),
		                p, work, sgrid, _MB_SG_COLMAJOR, nsp))
		_mb_check(ccall(_fn(:mbgrid_extract), Cint,
		                (Ref{_MBParams}, Ptr{Cfloat}, Ptr{Cfloat}, Cint, Ref{_MBStats}),
		                p, work, zout, _MB_LAY_BCB, stats))
	end
	st = stats[]
	stats[] = _MBStats(st.zmin, st.zmax, nd[], ns[], nsp[], st.n_unset)
	return nothing
end
