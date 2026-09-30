# types.jl — potracelib.h, auxiliary.h, bitmap.h, curve.h/curve.c and the list macros of lists.h.

# ---------------------------------------------------------------------------------------------
# tracing parameters (potracelib.h / potracelib.c param_default)

const TURNPOLICY_BLACK    = 0
const TURNPOLICY_WHITE    = 1
const TURNPOLICY_LEFT     = 2
const TURNPOLICY_RIGHT    = 3
const TURNPOLICY_MINORITY = 4
const TURNPOLICY_MAJORITY = 5
const TURNPOLICY_RANDOM   = 6

# segment tags
const CURVETO = 1
const CORNER  = 2

"""
    PotraceParams(; turdsize=2, turnpolicy=:minority, alphamax=1.0, opticurve=true, opttolerance=0.2)

Tracing parameters, defaults as in potrace 1.16.
- `turdsize`: area (pixels) of the largest path that is suppressed as a speckle.
- `turnpolicy`: how ambiguous turns are resolved in the path decomposition —
  `:black, :white, :left, :right, :minority, :majority, :random`.
- `alphamax`: corner threshold; 0 = polygon (all corners), 4/3 = no corners.
- `opticurve`: join adjacent Bézier segments when possible.
- `opttolerance`: tolerance of that curve optimization.
"""
struct PotraceParams
	turdsize::Int
	turnpolicy::Int
	alphamax::Float64
	opticurve::Bool
	opttolerance::Float64
end

const _TURNPOLICIES = Dict{Symbol,Int}(:black => TURNPOLICY_BLACK, :white => TURNPOLICY_WHITE,
	:left => TURNPOLICY_LEFT, :right => TURNPOLICY_RIGHT, :minority => TURNPOLICY_MINORITY,
	:majority => TURNPOLICY_MAJORITY, :random => TURNPOLICY_RANDOM)

function PotraceParams(; turdsize::Int=2, turnpolicy::Union{Symbol,Int}=:minority, alphamax::Real=1.0,
                       opticurve::Bool=true, opttolerance::Real=0.2)
	tp = turnpolicy isa Symbol ? get(_TURNPOLICIES, turnpolicy, -1) : turnpolicy
	0 <= tp <= 6 || error("unknown turnpolicy $turnpolicy")
	return PotraceParams(turdsize, tp, Float64(alphamax), opticurve, Float64(opttolerance))
end

# ---------------------------------------------------------------------------------------------
# points (auxiliary.h)

struct IPt
	x::Int
	y::Int
end

struct DPt
	x::Float64
	y::Float64
end
DPt(p::IPt) = DPt(p.x, p.y)

# range over the straight line segment [a,b] when lambda ranges over [0,1]
@inline interval(lambda::Float64, a::DPt, b::DPt) = DPt(a.x + lambda * (b.x - a.x), a.y + lambda * (b.y - a.y))

@inline _isign(x) = x > 0 ? 1 : x < 0 ? -1 : 0

# ---------------------------------------------------------------------------------------------
# bitmaps (bitmap.h). Same packing as the C: scanline y starts at word y*dy, the leftmost pixel of a
# word is its most significant bit. Words are 64 bits (the C uses `unsigned long`, 32 bits on
# Windows); the word size only changes speed, never the result.

const BM_WORDBITS = 64
const BM_HIBIT    = UInt64(1) << 63
const BM_ALLBITS  = typemax(UInt64)

"""
    PotraceBitmap(M; threshold, invert=false)

Potrace's 1-bit bitmap. `M` is a matrix laid out the way an image is displayed: `M[1,1]` is the
TOP-left pixel. A `Bool`/`BitMatrix` is taken as is (`true` = foreground, traced). A numeric matrix
is thresholded: pixels `< threshold` are foreground (dark on light, like potrace reading a greymap),
or `>= threshold` with `invert=true`. `threshold` defaults to the midpoint of the matrix's range.
"""
mutable struct PotraceBitmap
	w::Int
	h::Int
	dy::Int                 # words per scanline
	map::Vector{UInt64}     # dy*h words
end

function PotraceBitmap(w::Int, h::Int)
	dy = w == 0 ? 0 : div(w - 1, BM_WORDBITS) + 1
	return PotraceBitmap(w, h, dy, zeros(UInt64, max(dy * h, 1)))
end

function PotraceBitmap(M::Union{BitMatrix, Matrix{Bool}})
	h, w = size(M)
	bm = PotraceBitmap(w, h)
	@inbounds for col = 1:w, row = 1:h
		M[row, col] && BM_USET!(bm, col - 1, h - row)
	end
	return bm
end

function PotraceBitmap(M::Matrix{<:Real}; threshold::Union{Nothing,Real}=nothing, invert::Bool=false)
	thr = threshold === nothing ? (Float64(minimum(M)) + Float64(maximum(M))) / 2 : threshold
	return PotraceBitmap(invert ? M .>= thr : M .< thr)
end

bm_dup(bm::PotraceBitmap) = PotraceBitmap(bm.w, bm.h, bm.dy, copy(bm.map))

@inline _bm_word(bm::PotraceBitmap, x::Int, y::Int) = y * bm.dy + (x >> 6) + 1
@inline bm_mask(x::Int) = BM_HIBIT >> (x & (BM_WORDBITS - 1))
@inline bm_safe(bm::PotraceBitmap, x::Int, y::Int) = 0 <= x < bm.w && 0 <= y < bm.h
@inline BM_UGET(bm::PotraceBitmap, x::Int, y::Int) = (@inbounds bm.map[_bm_word(bm, x, y)] & bm_mask(x)) != 0
@inline BM_GET(bm::PotraceBitmap, x::Int, y::Int) = bm_safe(bm, x, y) && BM_UGET(bm, x, y)
@inline function BM_USET!(bm::PotraceBitmap, x::Int, y::Int)
	@inbounds bm.map[_bm_word(bm, x, y)] |= bm_mask(x)
	return nothing
end

# ---------------------------------------------------------------------------------------------
# curves and paths (curve.h / curve.c)

# vertex is c[i,2] for tag=CORNER, and the intersection of .c[i-1,3]..c[i,1] and c[i,2]..c[i,3]
# for tag=CURVETO. `c[i+1, k+1]` here is the C's c[i][k].
mutable struct PrivCurve
	n::Int
	tag::Vector{Int}
	c::Matrix{DPt}
	alphacurve::Bool
	vertex::Vector{DPt}
	alpha::Vector{Float64}
	alpha0::Vector{Float64}
	beta::Vector{Float64}
end

PrivCurve(n::Int) = PrivCurve(n, zeros(Int, n), fill(DPt(0.0, 0.0), n, 3), false, fill(DPt(0.0, 0.0), n),
                              zeros(n), zeros(n), zeros(n))

struct Sums
	x::Float64
	y::Float64
	x2::Float64
	xy::Float64
	y2::Float64
end

# potrace_path_t + potrace_privpath_t in one. `next` is the linked list; `childlist`/`sibling` the
# tree (a positive path's children are its holes, a hole's children the positives inside it).
mutable struct PotracePath
	area::Int
	sign::Char                      # '+' or '-'
	pt::Vector{IPt}                 # path as extracted from the bitmap
	lon::Vector{Int}
	x0::Int
	y0::Int
	sums::Vector{Sums}
	m::Int
	po::Vector{Int}
	curve::PrivCurve
	ocurve::PrivCurve
	fcurve::PrivCurve               # final curve: `curve` or `ocurve`
	next::Union{PotracePath,Nothing}
	childlist::Union{PotracePath,Nothing}
	sibling::Union{PotracePath,Nothing}
end

PotracePath(pt::Vector{IPt}, area::Int, sign::Char) =
	PotracePath(area, sign, pt, Int[], 0, 0, Sums[], 0, Int[], PrivCurve(0), PrivCurve(0), PrivCurve(0),
	            nothing, nothing, nothing)

"""
    PotraceResult

Result of `potrace`: bitmap size `w`,`h` and the path list `plist` (linked by `next`, each positive
path immediately followed by its holes; tree by `childlist`/`sibling`). `paths(res)` returns the
list as a Vector. A path's final curve is `p.fcurve`: `n` segments, `tag[i]` CORNER or CURVETO,
control points `c[i,1:3]`, all in pixel units, y up.
"""
struct PotraceResult
	w::Int
	h::Int
	plist::Union{PotracePath,Nothing}
end

function paths(res::PotraceResult)
	v = PotracePath[]
	p = res.plist
	while p !== nothing
		push!(v, p)
		p = p.next
	end
	return v
end

# ---------------------------------------------------------------------------------------------
# lists.h. A C "hook" is a `path_t **`: the address of a list head or of some element's `next`/
# `childlist` field. Here it is that owner plus the field name; `_Head` stands in for a local list
# head variable.

mutable struct _Head
	p::Union{PotracePath,Nothing}
end

struct _Hook
	obj::Union{PotracePath,_Head}
	fld::Symbol
end

_hook(h::_Head) = _Hook(h, :p)
@inline _hget(h::_Hook) = getfield(h.obj, h.fld)::Union{PotracePath,Nothing}
@inline _hset!(h::_Hook, v::Union{PotracePath,Nothing}) = setfield!(h.obj, h.fld, v)

# list_insert_beforehook: returns the advanced hook
@inline function _insert_beforehook!(elt::PotracePath, hook::_Hook)
	elt.next = _hget(hook)
	_hset!(hook, elt)
	return _Hook(elt, :next)
end

# list_append: returns the (possibly new) list head
function _list_append(list::Union{PotracePath,Nothing}, elt::PotracePath)
	elt.next = nothing
	list === nothing && return elt
	q = list
	while q.next !== nothing
		q = q.next
	end
	q.next = elt
	return list
end
