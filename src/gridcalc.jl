# gridcalc.jl — Grid Tools > "Grid calculator" (port of Mirone's src_figs/grid_calculator.m).
# Combine the grids of one window with an arithmetic expression, node by node.
#
# The C++ dialog is GridCalculatorDialog (70_window.cpp, loads deps/ui/grid_calculator.ui). Two
# entry points:
#
#   _gridcalc_names(scene, basename) — fills that list: every grid of the window whose limits AND
#       increments match the window's base grid EXACTLY (anything else cannot be combined node by
#       node, so it is not offered).
#   _on_gridcalc(scene, params)      — the Compute button: substitute every grid name by its data,
#       evaluate the expression element-wise, add the result as a new derived grid.
#
# HOW A GRID IS NAMED IN AN EXPRESSION. Mirone had to preface every name with `&` because it parsed
# and eval'd the raw string; here the names are substituted TEXTUALLY, before anything is parsed, so
# a BARE name is enough — `layer B + 1` works, blanks and all. `&name` / `&{name}` stay valid (that
# is what an old expression looks like, and Mirone muscle memory) and are resolved first.
# The one case that still NEEDS the `&`: a grid whose label is exactly a function name this
# calculator offers (a grid called `abs`), where the bare form would eat the function call. Those
# labels are left alone in the bare pass — see `_GC_FUNCS`.
# A bare name is matched only when it stands alone: the characters around it must not be part of an
# identifier, so a grid called `a` never matches the `a` inside `atan`. Longest names are tried
# first, so `layer0.grd` wins over a grid also called `layer0`.

# `&{any label}` or `&plain_name`.
const _GC_TOKEN = r"&\{([^}]*)\}|&([A-Za-z_][A-Za-z0-9_.]*)"

# Function names the keypad offers (plus the ones Base makes reachable in the same breath). A grid
# whose label is one of these can only be written `&label`.
const _GC_FUNCS = Set(["sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh",
                       "exp", "log", "log2", "log10", "sqrt", "abs", "sign", "floor", "ceil",
                       "round", "min", "max", "pi", "e"])

# A failure the calculator FINDS BEFORE computing — an unknown name, a grid of other limits, an
# operator given the wrong kind of operand. Its message is meant for the user as it is: the dialog
# shows it in a box instead of "Compute failed".
struct _GcRejected <: Exception
	msg::String
end
Base.showerror(io::IO, e::_GcRejected) = print(io, e.msg)

# `&` is two things: Mirone's grid-name prefix (`&topo`) and the logical AND of two masks
# (`(topo > 0) & mask`). It is a NAME PREFIX only where an operand is expected — at the start, or
# after an operator, `(` or `,`. After a name, a number or `)` it is the operator, and stays text.
# `&{label}` is always a name (no expression puts a `{` after an AND).
function _gc_sub_tokens(code::String, resolve::Function)::String
	res = IOBuffer();  i = firstindex(code)
	for m in eachmatch(_GC_TOKEN, code)
		k = m.offset
		print(res, SubString(code, i, prevind(code, k)))
		j = prevind(code, k)
		while j >= firstindex(code) && isspace(code[j]);  j = prevind(code, j);  end
		prefix = m.captures[1] !== nothing || j < firstindex(code) || code[j] in "(,+-*/^\\<>=!|&~"
		print(res, prefix ? resolve(_gc_token_name(m.match)) : m.match)
		i = nextind(code, k + ncodeunits(m.match) - 1)
	end
	print(res, SubString(code, i))
	return String(take!(res))
end

# ---- The expression check: what kind of value every piece of the expression is, BEFORE anything is
# evaluated. Operand kinds:  :grid  a grid (a number per node)        :num   a plain number
#                            :mask  a mask (true/false per node)      :bool  `true` / `false`
# Rules (each broken one is a `_GcRejected` naming the offending piece, never a Julia MethodError):
#   > >= < <=        grids and numbers only — a mask has no order;               result: mask
#   == !=            two grids/numbers, or two masks/true/false (never mixed);   result: mask
#   & | xor ! ~      masks (and true/false) only; && and || are the same as & |; result: mask
#   isnan(…) etc.    a test per node;                                            result: mask
#   ifelse(m, A, B)  m a mask, A/B both grids/numbers or both masks; or one of them an RGB image and
#                    the other an RGB image, a whole number 0-255 or a colour (r, g, b) of 0-255 —
#                    :rgb, the only place an RGB image (or a colour) may stand;    result: RGB image
#   ifelse(m, A)     B left out = nothing: NaN for a grid/number, fully transparent (255@100) for an
#                    RGB image (`_gc_ifelse2` writes B in before the check)
#   + - * / ^, any other Base function   a mask counts as 0/1 (`topo * mask`);  result: grid
const _GC_ORDER = (:<, :<=, :>, :>=, :≤, :≥)
const _GC_EQUAL = (:(==), :!=, :≠)
const _GC_LOGIC = (:&, :|, :xor, :⊻, :!, :~)
const _GC_PREDS = (:isnan, :isfinite, :isinf, :iszero, :isone, :signbit, :iseven, :isodd, :isinteger)
const _GC_CONSTS = Dict{Symbol,Any}(:pi => pi, :π => pi, :e => ℯ, :ℯ => ℯ, :NaN => NaN, :Inf => Inf)

_gc_masky(k::Symbol) = k === :mask || k === :bool
_gc_numy(k::Symbol)  = k === :grid || k === :num

# `&&`/`||` as the element-wise `&`/`|`, dotted operators (`.>`, `f.(x)`) undotted (the whole
# expression is broadcast anyway), constants (pi, e) by their values.
function _gc_lower(ex)
	ex isa Symbol && return haskey(_GC_CONSTS, ex) ? _GC_CONSTS[ex] : ex
	ex isa Expr || return ex
	if ex.head === :&& || ex.head === :||
		return Expr(:call, ex.head === :&& ? :& : :|, map(_gc_lower, ex.args)...)
	elseif ex.head === :. && length(ex.args) == 2 && ex.args[2] isa Expr && ex.args[2].head === :tuple
		return Expr(:call, ex.args[1], map(_gc_lower, ex.args[2].args)...)
	elseif ex.head === :call && ex.args[1] isa Symbol
		f = String(ex.args[1])
		fs = (length(f) > 1 && f[1] == '.') ? Symbol(f[2:end]) : ex.args[1]
		return Expr(:call, fs, map(_gc_lower, ex.args[2:end])...)
	elseif ex.head === :comparison
		return Expr(:comparison, (isodd(i) ? _gc_lower(a) :
		            (a isa Symbol && length(String(a)) > 1 && String(a)[1] == '.' ? Symbol(String(a)[2:end]) : a)
		            for (i, a) in enumerate(ex.args))...)
	end
	return Expr(ex.head, map(_gc_lower, ex.args)...)
end

# A piece of the expression as the user wrote it: generated variables back to their grid names.
_gc_show(ex, labels::Dict{Symbol,String}) =
	replace(string(ex), r"_gc\d+" => v -> get(labels, Symbol(v), v))

# The kind of `ex` (see the rules above), or a `_GcRejected` saying which piece breaks which rule.
function _gc_kind(ex, kinds::Dict{Symbol,Symbol}, labels::Dict{Symbol,String})::Symbol
	sh(x) = "`" * _gc_show(x, labels) * "`"
	what = _gc_what
	ex isa Bool && return :bool
	ex isa Real && return :num
	if ex isa Symbol
		haskey(kinds, ex) && return kinds[ex]
		throw(_GcRejected("unknown name `$ex`: not a grid of this window, a Load Grid file, nor a constant"))
	end
	ex isa Expr || throw(_GcRejected("$(sh(ex)) is not something the calculator understands"))

	# (r, g, b): a colour, for `ifelse` beside an RGB image (checked there)
	if ex.head === :tuple
		(length(ex.args) == 3 && all(a -> a isa Integer && !(a isa Bool) && 0 <= a <= 255, ex.args)) ||
			throw(_GcRejected("$(sh(ex)): a colour is three whole numbers 0-255, (r, g, b)"))
		return :color
	end

	# a < b <= c
	if ex.head === :comparison
		for i in 2:2:length(ex.args) - 1
			_gc_check_compare(ex.args[i], ex.args[i-1], ex.args[i+1], kinds, labels)
		end
		return :mask
	end
	ex.head === :call && ex.args[1] isa Symbol ||
		throw(_GcRejected("$(sh(ex)) is not something the calculator understands"))
	f, args = ex.args[1], ex.args[2:end]

	if f in _GC_ORDER || f in _GC_EQUAL
		length(args) == 2 || throw(_GcRejected("$(sh(ex)): `$f` compares exactly two things"))
		_gc_check_compare(f, args[1], args[2], kinds, labels)
		return :mask
	end
	if f in _GC_LOGIC
		for a in args
			k = _gc_kind(a, kinds, labels);  _gc_pixel_only(k, a, labels)
			_gc_masky(k) && continue
			hint = k === :grid ? " Make a mask from it first, e.g. ($(_gc_show(a, labels)) > 0)." :
			       " `&` and `|` bind tighter than a comparison: put each comparison in parentheses, " *
			       "e.g. (a > 1) & (b < 2)."
			throw(_GcRejected("`$f` works on MASKS (true/false per node), but $(sh(a)) is $(what(k))." * hint))
		end
		return :mask
	end
	if f in _GC_PREDS
		length(args) == 1 || throw(_GcRejected("$(sh(ex)): `$f` takes one grid"))
		k = _gc_kind(args[1], kinds, labels);  _gc_pixel_only(k, args[1], labels)
		_gc_numy(k) || throw(_GcRejected("`$f` tests the values of a grid, but $(sh(args[1])) is $(what(k))"))
		return :mask
	end
	if f === :ifelse
		length(args) == 3 || throw(_GcRejected("$(sh(ex)): ifelse takes (mask, A) or (mask, A, B)"))
		kc = _gc_kind(args[1], kinds, labels)
		_gc_masky(kc) || throw(_GcRejected("ifelse's first argument must be a mask (e.g. topo > 0), but $(sh(args[1])) is $(what(kc))"))
		ka, kb = _gc_kind(args[2], kinds, labels), _gc_kind(args[3], kinds, labels)
		if ka === :rgb || kb === :rgb
			for (k, a) in ((ka, args[2]), (kb, args[3]))
				(k === :rgb || k === :color || _gc_is_byte(a)) && continue
				throw(_GcRejected("ifelse with an RGB image: the other of A and B must be an RGB image, a whole " *
				                  "number 0-255 or a colour (r, g, b), but $(sh(a)) is $(what(k))"))
			end
			return :rgb
		end
		(ka === :color || kb === :color) &&
			throw(_GcRejected("a colour or a transparency (`255@50`) paints an RGB image: the other of A and B " *
			                  "must be an RGB image, e.g. ifelse(mask, image, 255@50)"))
		_gc_masky(ka) && _gc_masky(kb) && return :mask
		_gc_numy(ka) && _gc_numy(kb) && return :grid
		throw(_GcRejected("ifelse picks between two values of the same kind, but $(sh(args[2])) is $(what(ka)) and $(sh(args[3])) is $(what(kb))"))
	end
	if f === :_gc_tr                                   # `v@t`: a fill with transparency (see _GC_TRANSP)
		v, t = args[1], args[2]
		(_gc_is_byte(v) || (v isa Expr && v.head === :tuple && _gc_kind(v, kinds, labels) === :color)) ||
			throw(_GcRejected("$(sh(ex)): before `@` goes a whole number 0-255 or a colour (r, g, b)"))
		(t isa Real && !(t isa Bool) && 0 <= t <= 100) ||
			throw(_GcRejected("$(sh(ex)): after `@` goes a transparency from 0 to 100 (%)"))
		return :color
	end
	(isdefined(Base, f) && getfield(Base, f) isa Function) ||
		throw(_GcRejected("`$f` is not a function the calculator knows"))
	ks = [_gc_kind(a, kinds, labels) for a in args]
	for (k, a) in zip(ks, args)
		_gc_pixel_only(k, a, labels)
	end
	return any(k -> k === :grid || k === :mask, ks) ? :grid : :num
end

# TWO-argument `ifelse(mask, A)`: B is filled in from A's kind, so where the mask is false there is
# NOTHING — an RGB image becomes fully transparent there (`ifelse(mask, img, 255@100)`), a grid or a
# number gets NaN (`ifelse(mask, topo, NaN)`). Rewritten bottom-up before the kind check, so a nested
# two-argument ifelse is completed first and everything after (check, eval) only ever sees three.
function _gc_ifelse2(ex, kinds::Dict{Symbol,Symbol}, labels::Dict{Symbol,String})
	ex isa Expr || return ex
	ex = Expr(ex.head, (_gc_ifelse2(a, kinds, labels) for a in ex.args)...)
	(ex.head === :call && ex.args[1] === :ifelse && length(ex.args) == 3) || return ex
	A = ex.args[3]
	k = _gc_kind(A, kinds, labels)
	fill = k === :rgb ? Expr(:call, :_gc_tr, 255, 100) : _gc_numy(k) ? NaN :
		throw(_GcRejected("ifelse(mask, A) fills the rest with nothing: A must be a grid, a number or an RGB " *
		                  "image, but `$(_gc_show(A, labels))` is $(_gc_what(k)). Give B: ifelse(mask, A, B)"))
	return Expr(:call, :ifelse, ex.args[2], A, fill)
end

_gc_what(k::Symbol) = k === :grid ? "a grid" : k === :mask ? "a mask (true/false per node)" :
                      k === :num ? "a number" : k === :rgb ? "an RGB image" :
                      k === :color ? "a colour (r, g, b)" : "true/false"

# A whole number 0-255 written in the expression (a fill value beside an RGB image).
_gc_is_byte(a) = a isa Integer && !(a isa Bool) && 0 <= a <= 255

# An RGB image or a colour is a PICTURE, not a value per node: anywhere but A/B of ifelse it is refused.
function _gc_pixel_only(k::Symbol, a, labels::Dict{Symbol,String})
	(k === :rgb || k === :color) || return nothing
	throw(_GcRejected("`$(_gc_show(a, labels))` is $(_gc_what(k)): it can only be A or B of " *
	                  "ifelse(mask, A, B), e.g. ifelse(mask, image, 255) or ifelse(mask, image, (255, 0, 0))"))
end

# One comparison `a op b`, checked against the rules above.
function _gc_check_compare(op, a, b, kinds::Dict{Symbol,Symbol}, labels::Dict{Symbol,String})
	ka, kb = _gc_kind(a, kinds, labels), _gc_kind(b, kinds, labels)
	_gc_pixel_only(ka, a, labels);  _gc_pixel_only(kb, b, labels)
	sa, sb = "`" * _gc_show(a, labels) * "`", "`" * _gc_show(b, labels) * "`"
	if op in _GC_ORDER
		for (k, s) in ((ka, sa), (kb, sb))
			_gc_masky(k) && throw(_GcRejected("`$op` compares grids and numbers, but $s is a mask (true/false " *
			                                  "per node), which has no order. Test a mask with == or !=, or combine masks with & | !"))
		end
	elseif op in _GC_EQUAL
		(_gc_masky(ka) == _gc_masky(kb)) || throw(_GcRejected("`$op` compares two grids/numbers or two masks, " *
		                                   "but $sa and $sb are one of each. Compare a mask with true or false"))
	else
		throw(_GcRejected("`$op` is not a comparison"))
	end
	return nothing
end

# TRANSPARENCY: `v@t` — a whole number 0-255 or a colour (r, g, b), then a transparency 0-100 % — is
# written `_gc_tr(v, t)` before parsing (`@` is a macro sign to Julia's parser). It is the painted side
# of an RGB `ifelse` only: `ifelse(mask, img, 255@50)` paints white at 50 % transparency where the mask
# is false; `@100` is fully see-through, whatever the number.
const _GC_TRANSP = r"(\([^()]*\)|(?<![A-Za-z0-9_.])[0-9]+(?:\.[0-9]*)?)\s*@\s*([0-9]+(?:\.[0-9]*)?)"
_gc_sub_transparency(code::AbstractString) = replace(String(code), _GC_TRANSP => s"_gc_tr(\1, \2)")

# The grid name written inside a token (braced or not).
function _gc_token_name(tok::AbstractString)::String
	s = String(tok)[2:end]                       # drop the '&'
	return (startswith(s, '{') && endswith(s, '}')) ? s[2:prevind(s, lastindex(s))] : s
end

# Can these two grids be combined node by node? Same node count, same limits, same increments and
# same registration — "EXACT SAME LIMITS and increments", with only a floating-point epsilon of
# slack (a grid that went through a file round-trip can differ in the last bit).
function _gc_same_geom(G::GMTgrid, ref::GMTgrid)::Bool
	# Node counts from the COORDINATES (grid-layout law): size(z) is (ny,nx) or (nx,ny) depending on the
	# memory layout, so two grids of one geometry read by different readers looked different.
	_grid_dims(G) == _grid_dims(ref) || return false
	G.registration == ref.registration || return false
	tolx = abs(ref.inc[1]) * 1e-6;  toly = abs(ref.inc[2]) * 1e-6
	(abs(G.inc[1] - ref.inc[1]) <= tolx && abs(G.inc[2] - ref.inc[2]) <= toly) || return false
	for (k, tol) in ((1, tolx), (2, tolx), (3, toly), (4, toly))
		abs(G.range[k] - ref.range[k]) <= tol || return false
	end
	return true
end

# The same question for an IMAGE (an RGB operand of `ifelse`): its pixel count, registration,
# increments and limits against the reference grid's, with the same epsilon of slack.
function _gc_same_geom(I::GMTimage, ref::GMTgrid)::Bool
	nx = length(I.x) - I.registration;  ny = length(I.y) - I.registration
	(nx, ny) == _grid_dims(ref) || return false
	I.registration == ref.registration || return false
	tolx = abs(ref.inc[1]) * 1e-6;  toly = abs(ref.inc[2]) * 1e-6
	(abs(I.inc[1] - ref.inc[1]) <= tolx && abs(I.inc[2] - ref.inc[2]) <= toly) || return false
	for (k, tol) in ((1, tolx), (2, tolx), (3, toly), (4, toly))
		abs(I.range[k] - ref.range[k]) <= tol || return false
	end
	return true
end

# ---- RGB images. A picture of three or four bands is not one value per node (a one-band palette image
# IS: its indices), so it is never a grid operand: only A or B of `ifelse(mask, A, B)` — Mirone run_cmd
# `mask(Z)` (= ifelse(M, A, 255)) and `mix(Z, img2, mask)` (= ifelse(M, img2, A)) in one function.
_gc_is_rgb(I::GMTimage) = ndims(I.image) == 3 && size(I.image, 3) >= 3

# A band-planar UInt8 copy to paint into (never the source image's own buffer),
# the way `_coast_mask_image` (coastmask.jl) prepares the image it paints.
function _gc_rgb_copy(I::GMTimage)::GMTimage
	J  = I
	Ib = _to_band_planar(J)
	Ib === J && (Ib = deepcopy(J))
	eltype(Ib.image) == UInt8 || (Ib = _stretch_to_u8(Ib))
	_gc_true_label!(Ib)
	return Ib
end

# A picture the calculator made is born with the layout code the display reads it by (`_north_first`,
# via `_true_layout`, grid.jl), so a crop of it reads the rows the screen shows (measured, see
# `_roi_crop_image`).
_gc_true_label!(I::GMTimage) = (I.layout = _true_layout(I.layout); I)   # the rule: grid.jl

# The same copy WITH an alpha band (4th band, opaque = 255), for an `ifelse` that paints transparency
# (`255@50`). Built in ONE pass straight from the source's own memory — band-planar or pixel-interleaved
# alike — into the final 4-band array: never a 3-band copy first and a 4-band one after it. A source
# that already carries an alpha band keeps it.
function _gc_rgba_copy(I::GMTimage)::GMTimage
	eltype(I.image) == UInt8 || (I = _stretch_to_u8(I))
	S = I.image;  lay = I.layout
	nb = size(S, 3)
	inter = length(lay) >= 3 && lay[3] == 'P'
	d1, d2 = inter ? (size(S, 1), size(S, 2)) : (size(S, 1), size(S, 2))
	A = Array{UInt8}(undef, d1, d2, 4)
	P = inter ? reshape(S, nb, d1, d2) : S          # interleaved memory, band fastest (as _to_band_planar)
	Threads.@threads for j in 1:d2
		@inbounds for b in 1:4, i in 1:d1
			A[i, j, b] = b <= nb ? (inter ? P[b, i, j] : S[i, j, b]) : 0xff
		end
	end
	J = GMT.mat2img(A, I)
	J.layout = inter ? lay[1:2] * "B" * lay[4:end] : lay
	return _gc_true_label!(J)
end

# Linear index of pixel (ix, iy) — iy counted from the SOUTH — in a band-planar image, plus the band
# stride: the row-order rule of the texture packing (`_north_first`, drape.jl), as coastmask.jl uses it.
function _gc_rgb_index(I::GMTimage)
	S = I.image;  lay = I.layout
	rowmajor = length(lay) >= 2 && lay[2] == 'R'
	nx, ny = rowmajor ? (size(S, 1), size(S, 2)) : (size(S, 2), size(S, 1))
	nf = _north_first(lay, rowmajor)
	idx = (ix, iy) -> (r = nf ? ny - iy + 1 : iy; rowmajor ? ix + (r - 1) * nx : r + (ix - 1) * ny)
	nb = ndims(S) == 3 ? size(S, 3) : 1
	return idx, nx * ny, min(nb, 3)
end

# A ONE-band image as a calculator operand: its pixel values (a Landsat band's DNs) as a Float32 grid
# on the image's own georeference. `nothing` for an RGB(A) picture — three bands are not one value
# per node. The buffer is taken where it lies: the layout travels with it (`mat2grid(z, I)` copies
# `I.layout`), and (ny,nx) dims over that memory are GMT.jl's grid convention for either order.
#
# A MASK (a Bool image, `_commit_mask!`) stays a mask: a Bool grid, true/false per node, never a 0/1
# Float32 one — the expression check (`_gc_kind`) needs to know which operands are masks.
function _gc_image_grid(I::GMTimage)::Union{GMTgrid,Nothing}
	(ndims(I.image) == 2 || size(I.image, 3) == 1) || return nothing
	nx = length(I.x) - I.registration
	ny = length(I.y) - I.registration
	length(I.image) == nx * ny || return nothing
	# NO COPY: the grid's z IS the image's own buffer, in its own element type (a Landsat band stays
	# UInt16). It used to be a Float32 copy of every one-band image in the window, made on every list
	# refresh and every Compute whether the expression used it or not — four times each image's memory,
	# for nothing. Integer values become Float32 lazily, where an expression reads them (`_gc_operand`).
	G = GMT.mat2grid(reshape(I.image, ny, nx), I)
	# The row order the buffer REALLY has — `_north_first` (drape.jl), the rule the display reads it
	# with: gmtread's disk reader hands a row-major image back north row first but labels it 'B', and
	# the grid inherited that label, so a Landsat band came out of the calculator upside down.
	lay = I.layout
	rowmajor = length(lay) >= 2 && lay[2] == 'R'
	length(lay) >= 3 && (G.layout = string(_north_first(lay, rowmajor) ? 'T' : 'B', lay[2], 'B'))
	return G
end

# Every grid this window knows about, in Scene Objects order: [(label, GMTgrid), …]. The window's
# base grid is remembered under the empty name (grid.jl `_remember_object!(h, :grid, "", G)`), so it
# takes the label the dialog sees in Scene Objects (`basename`, handed over by the C++ side).
# One-band IMAGES are operands too (a Landsat band, any 16-bit image), at full precision: a 16-bit
# image's stashed original, never its 8-bit display copy.
function _gridcalc_objects(scene::Ptr{Cvoid}, basenm::AbstractString)
	out = Tuple{String,GMTgrid}[]
	origs = get(_IMG_ORIG, scene, Dict{String,GMTimage}())
	for (kind, nm, data) in get(_SCENE_OBJS, scene, Tuple{Symbol,String,Any}[])
		label = isempty(nm) ? String(basenm) : nm
		G = (kind === :grid && data isa GMTgrid) ? data :
		    (kind === :image && data isa GMTimage) ? _gc_image_grid(get(origs, nm, data)) : nothing
		G === nothing && continue
		any(t -> t[1] == label, out) || push!(out, (label, G))
	end
	if isempty(out)                                   # never registered? fall back to the figure itself
		fig = get(_FIGREG, scene, nothing)
		fig isa QtFigure && push!(out, (String(basenm), fig.G))
	end
	return out
end

# Every RGB image of this window (three or four bands), in Scene Objects order: [(label, GMTimage), …].
# Operands of `ifelse(mask, A, B)` only (see `_gc_is_rgb`).
# An RGB operand is a PICTURE, so it is taken AS THE WINDOW SHOWS IT: the row's own 8-bit object, never
# the 16-bit original kept aside for re-stretching (`_IMG_ORIG`). A Landsat RGB composition's original
# is raw UInt16 DNs; squeezing those into 0-255 again gave back a near-black picture, not the colours
# on screen.
function _gridcalc_rgbs(scene::Ptr{Cvoid}, basenm::AbstractString)
	out = Tuple{String,GMTimage}[]
	for (kind, nm, data) in get(_SCENE_OBJS, scene, Tuple{Symbol,String,Any}[])
		(kind === :image && data isa GMTimage) || continue
		I = data
		_gc_is_rgb(I) || continue
		label = isempty(nm) ? String(basenm) : nm
		any(t -> t[1] == label, out) || push!(out, (label, I))
	end
	return out
end

# Print (one per line) the grids of `scene` usable in one expression: the base grid and every other
# grid sharing its limits/increments, then the RGB images on that same geometry. Called from the
# dialog through the g_juliaEval bridge, which hands back whatever this prints — hence `print`, and
# `nothing` as the value.
function _gridcalc_names(scene::Ptr{Cvoid}, basenm::AbstractString)
	objs = _gridcalc_objects(scene, basenm)
	isempty(objs) && return nothing
	ref = objs[1][2]
	names = String[nm for (nm, G) in objs if _gc_same_geom(G, ref)]
	for (nm, I) in _gridcalc_rgbs(scene, basenm)
		(_gc_same_geom(I, ref) && !(nm in names)) && push!(names, nm)
	end
	print(join(names, '\n'))
	return nothing
end

# Replace the BARE occurrences of the known grid labels in `code` by whatever `tovar(name)` returns.
# Longest label first (so `layer0.grd` is consumed before a grid also called `layer0`), and a label
# only counts when it stands alone — the characters on both sides must not be identifier characters,
# which is what keeps a grid named `a` out of the `a` in `atan`. Labels that ARE function names are
# skipped: those can only be written `&label` (see _GC_FUNCS).
function _gc_replace_bare(code::String, names::Vector{String}, tovar::Function)::String
	isident(c::Char) = isletter(c) || isdigit(c) || c == '_' || c == '.'
	out = code
	for nm in sort(unique(names), by = length, rev = true)
		(isempty(nm) || nm in _GC_FUNCS) && continue
		occursin(nm, out) || continue
		res = IOBuffer();  i = firstindex(out)
		while i <= lastindex(out)
			r = findnext(nm, out, i)
			r === nothing && break
			pre = first(r) > firstindex(out) ? out[prevind(out, first(r))] : ' '
			nxt = last(r)  < lastindex(out)  ? out[nextind(out, last(r))]  : ' '
			print(res, SubString(out, i, prevind(out, first(r))))
			print(res, (isident(pre) || isident(nxt)) ? nm : tovar(nm))
			i = nextind(out, last(r))
		end
		print(res, SubString(out, i))
		out = String(take!(res))
	end
	return out
end

# The operands with a REFERENCE geometry first. A window with no grid and no one-band image (an empty
# or an RGB-only one) still computes: the "Load Grid" file the expression names first is then the
# reference. Returns `objs` untouched when it already has one.
function _gc_with_reference(objs::Vector{Tuple{String,GMTgrid}}, files::Dict{String,String}, expr::AbstractString)
	isempty(objs) || return objs
	isempty(files) && error("no grid or one-band image to compute with: use Load Grid to add one")
	pos(k) = (r = findfirst(k, String(expr)); r === nothing ? typemax(Int) : first(r))
	nm = argmin(pos, collect(keys(files)))
	G = _gmtread_trb(files[nm])
	G isa GMTimage && (G = _gc_image_grid(G))
	G isa GMTgrid || error(nm * " is not a grid or a one-band image")
	return [(nm, G)]
end

# The maths, apart from any window: substitute every grid name in `expr` by the matching grid and
# evaluate the expression element-wise. `objs` is [(label, grid), …] (the window's grids, first one
# = the reference geometry), `files` maps a file NAME to its path (the "Load Grid" entries, read
# here — Mirone's button likewise only stores the name), `rgbs` the window's RGB images (operands of
# `ifelse` only). Returns the result array, or a GMTimage when the result is an RGB image.
# Kept separate from the callback so the whole compute path is testable without a live window.
function _gridcalc_eval(objs::Vector{Tuple{String,GMTgrid}}, files::Dict{String,String}, expr::AbstractString;
                        rgbs::Vector{Tuple{String,GMTimage}} = Tuple{String,GMTimage}[],
                        alias::Union{Nothing,Base.RefValue{Tuple{String,String}}} = nothing)
	objs = _gc_with_reference(objs, files, expr)
	ref = objs[1][2]
	# Each name becomes a generated variable, the grid behind it read (and geometry-checked) the first
	# time it appears. Same name twice = same variable, so a grid is never read twice. ONE resolver for
	# both spellings (`&name` and bare `name`) — they must never diverge in what they accept.
	vars = Dict{String,Symbol}()
	arrays = Pair{Symbol,Any}[]
	images = Dict{Symbol,GMTimage}()           # the RGB operands, by variable
	local badname = ""
	resolve = function (nm::AbstractString)
		haskey(vars, nm) && return string(vars[nm])
		local G = nothing
		for (label, g) in objs
			if label == nm
				G = g
				break
			end
		end
		if G === nothing
			for (label, I) in rgbs
				if label == nm
					G = I
					break
				end
			end
		end
		if G === nothing
			path = get(files, nm, "")
			if isempty(path)
				badname = "unknown grid: " * nm
				return "nothing"
			end
			G = _gmtread_trb(path)      # grids are READ in "TRB" — THE reader
			# a one-band image file (a Landsat band .TIF) is a grid operand; an RGB one stays a picture
			(G isa GMTimage && !_gc_is_rgb(G)) && (G = _gc_image_grid(G))
			if !isa(G, GMTgrid) && !isa(G, GMTimage)
				badname = nm * " is not a grid"
				return "nothing"
			end
		end
		if !_gc_same_geom(G, ref)
			badname = nm * ": limits/increments differ from " * objs[1][1]
			return "nothing"
		end
		v = Symbol("_gc", length(vars) + 1)
		vars[nm] = v
		if G isa GMTimage
			images[v] = G
			return string(v)
		end
		# One element order for the whole expression — `ref`'s: element k of every operand is the SAME
		# node. A grid that already lies that way (the normal case) is its own buffer, flattened; one
		# that does not is read through `_GcAs`, which maps ref's index k to that grid's own memory —
		# an index, never a re-ordered copy. The result comes out in `ref`'s order, which is the layout
		# `mat2grid(z, ref)` then labels it with.
		push!(arrays, v => _gc_operand(_gc_as(G, ref)))
		return string(v)
	end

	# Pass 1: the explicit `&name` / `&{name}` tokens (an `&` after an operand is the AND operator).
	code = _gc_sub_tokens(String(expr), resolve)
	isempty(badname) || throw(_GcRejected(badname))
	# Pass 2: the same names written bare. Only labels we actually know are looked for, so nothing
	# else in the expression can be touched.
	known = String[nm for (nm, _) in objs]
	append!(known, String[nm for (nm, _) in rgbs])
	append!(known, keys(files))
	code = _gc_replace_bare(code, known, resolve)
	isempty(badname) || throw(_GcRejected(badname))
	isempty(vars) && throw(_GcRejected("the expression uses no grid (nothing to compute)"))
	alias === nothing || (alias[] = _gc_alias_name(code, Dict{Symbol,String}(v => nm for (nm, v) in vars)))

	# CHECK before computing: parse, then give every piece a kind (grid / mask / number) and refuse
	# an operator handed the wrong one — `&` on a grid, `>` on a mask — with a message saying which
	# piece and what to write instead, never a Julia MethodError from the middle of the evaluation.
	ex = try
		Meta.parse(_gc_sub_transparency(code))
	catch
		throw(_GcRejected("the expression cannot be read: look for a missing operand, operator or parenthesis"))
	end
	(ex isa Expr && ex.head in (:incomplete, :error)) &&
		throw(_GcRejected("the expression cannot be read: look for a missing operand, operator or parenthesis"))
	(ex isa Expr && ex.head in (:toplevel, :block)) &&
		throw(_GcRejected("write ONE expression (no `;`, no second line)"))
	ex = _gc_lower(ex)
	labels = Dict{Symbol,String}(v => nm for (nm, v) in vars)
	kinds  = Dict{Symbol,Symbol}(v => (eltype(z) == Bool ? :mask : :grid) for (v, z) in arrays)
	for v in keys(images)
		kinds[v] = :rgb
	end
	ex = _gc_ifelse2(ex, kinds, labels)              # ifelse(mask, A) -> ifelse(mask, A, nothing-of-A's-kind)
	kind = _gc_kind(ex, kinds, labels)
	(kind === :rgb && ex isa Symbol) && _gc_pixel_only(kind, ex, labels)     # a bare image: nothing to compute

	# Evaluate in a throwaway module (nothing of the session reachable from the expression).
	m = Module(:GridCalcSandbox)
	kind === :rgb && return _gc_rgb_eval(ex, m, images, kinds, ref, arrays)
	# ONE pass, node by node, straight into the result's final array — Bool for a mask, Float32 for a
	# grid. No intermediate array of any kind: the result is the only allocation.
	kind in (:mask, :bool, :grid, :num) || error("the expression produced $(_gc_what(kind)), not a grid")
	z = Vector{_gc_mask_kind(kind) ? Bool : Float32}(undef, length(ref.z))
	_gc_run!(z, ex, m, arrays)
	return reshape(z, size(ref.z))                  # same memory, GMT.jl's (ny,nx) dims
end

_gc_mask_kind(k::Symbol) = k === :mask || k === :bool

# THE evaluator: the expression compiled ONCE into a scalar kernel — every operand `_gcN` read as
# `_gcN[k]` — run over the nodes k by every thread Julia has, each node computed and written straight
# into `out`. Element-wise by construction: whatever the expression (arithmetic, comparisons, `&`,
# `ifelse`, sin…), no node ever needs another one, so the threads split the nodes with no locking, and
# no temporary array is ever made. The operands arrive as ARGUMENTS of the compiled function, so it is
# specialised on their concrete types (a `_GcF32` view, a `_GcAs` index, a plain buffer) — a tight loop.
function _gc_run!(out::Vector, ex, m::Module, arrays::Vector{Pair{Symbol,Any}})
	vs   = Symbol[v for (v, _) in arrays]
	body = _gc_at(ex, Set(vs))
	f = Core.eval(m, :(function (_gc_out, $(vs...))
		Base.Threads.@threads for _gc_k in eachindex(_gc_out)
			@inbounds _gc_out[_gc_k] = $body
		end
		return _gc_out
	end))
	Base.invokelatest(f, out, (z for (_, z) in arrays)...)
	return out
end

# The expression with every operand variable read at node `_gc_k`.
_gc_at(ex::Symbol, vs::Set{Symbol}) = ex in vs ? :($ex[_gc_k]) : ex
_gc_at(ex::Expr, vs::Set{Symbol}) = Expr(ex.head, (_gc_at(a, vs) for a in ex.args)...)
_gc_at(ex, ::Set{Symbol}) = ex

# A grid's values as ONE flat vector in `ref`'s element order, never a copy: its own buffer when the
# two lie the same way (the normal case), else `_GcAs`, an index map onto its own memory.
function _gc_as(G::GMTgrid, ref::GMTgrid)
	c = _grid_layout_code(ref);  cg = _grid_layout_code(G)
	cg == c && return vec(G.z)
	nx, ny = _grid_dims(ref)
	(nx, ny) == _grid_dims(G) || error("grids of different sizes cannot be combined element-wise")
	return _GcAs(vec(G.z), nx, ny, (c & 1) != 0, (c & 2) != 0, (cg & 1) != 0, (cg & 2) != 0)
end

# Element k of `ref`'s memory order, read from a buffer laid out another way (`_grid_layout_code`:
# bit 0 row-major, bit 1 north row first).
struct _GcAs{T,A<:AbstractVector{T}} <: AbstractVector{T}
	a::A
	nx::Int
	ny::Int
	rowref::Bool
	northref::Bool
	rowsrc::Bool
	northsrc::Bool
end
Base.size(v::_GcAs) = (v.nx * v.ny,)
Base.@propagate_inbounds function Base.getindex(v::_GcAs, k::Int)
	nx, ny = v.nx, v.ny
	ix, j = v.rowref ? ((k - 1) % nx + 1, (k - 1) ÷ nx + 1) : ((k - 1) ÷ ny + 1, (k - 1) % ny + 1)
	iy = v.northref ? ny + 1 - j : j                 # row counted from the south
	js = v.northsrc ? ny + 1 - iy : iy
	return v.a[v.rowsrc ? (js - 1) * nx + ix : (ix - 1) * ny + js]
end

# A grid operand as the expression reads it. Float32/Float64/Bool buffers are used as they lie; an
# INTEGER buffer (a UInt16 Landsat band) is read through `_GcF32`, a no-copy Float32 view, so
# `(B5 - B4) / (B5 + B4)` is real arithmetic, never UInt16 wrap-around — and never a converted copy.
_gc_operand(z::AbstractArray{<:Union{AbstractFloat,Bool}}) = z
_gc_operand(z::AbstractArray{<:Integer}) = _GcF32(z)

struct _GcF32{N,A<:AbstractArray{<:Integer,N}} <: AbstractArray{Float32,N}
	a::A
end
Base.size(v::_GcF32) = size(v.a)
Base.IndexStyle(::Type{<:_GcF32}) = IndexLinear()       # operands are flat vectors (`_gc_as`)
Base.@propagate_inbounds Base.getindex(v::_GcF32, i::Int) = Float32(v.a[i])

# The result's NAME: the expression with each operand written as a one-letter alias — A, B, C … in the
# order the operands first appear — so a calculation on three long Landsat file names is called
# `(A - B) / (A + B)`, not a 200-character copy of the expression. `code` is the expression with every
# operand already replaced by its generated variable (`_gc1`, `_gc2`, …), `labels` maps each variable
# back to its operand's name. Returns (name, legend), the legend saying who is who: "A = …, B = …".
function _gc_alias_name(code::AbstractString, labels::Dict{Symbol,String})::Tuple{String,String}
	letters = Dict{String,String}()
	legend  = String[]
	letter(k) = k <= 26 ? string(Char('A' + k - 1)) : "X$k"
	name = replace(String(code), r"_gc\d+" => function (v)
		get!(letters, v) do
			a = letter(length(letters) + 1)
			push!(legend, a * " = " * get(labels, Symbol(v), v))
			a
		end
	end)
	return String(strip(name)), join(legend, ", ")
end

# A result name nobody in this window uses yet: `name`, else `name (2)`, `name (3)` … Aliases repeat
# across calculations (every two-operand difference is `A - B`), and an existing object of the same name
# would otherwise be REPLACED by the new result.
function _gc_unique_name(scene::Ptr{Cvoid}, name::String)::String
	used = Set{String}(nm for (_, nm, _) in get(_SCENE_OBJS, scene, Tuple{Symbol,String,Any}[]))
	name in used || return name
	k = 2
	while "$name ($k)" in used
		k += 1
	end
	return "$name ($k)"
end

# An `ifelse` whose result is an RGB image (`_gc_kind` said :rgb, so its shape is already checked):
# a band-planar UInt8 copy of the RGB side, the other side painted in wherever the mask picks it —
# a whole number on every colour band, a colour (r, g, b) band by band, or another RGB image pixel by
# pixel. A 4th (alpha) band is left as the image has it. Pixels are matched by (ix, iy) through each
# image's own row order (`_gc_rgb_index`), the mask's through `_zmat` — never by raw buffer position.
function _gc_rgb_eval(ex, m::Module, images::Dict{Symbol,GMTimage}, kinds::Dict{Symbol,Symbol}, ref::GMTgrid,
                      arrays::Vector{Pair{Symbol,Any}})::GMTimage
	ex isa Symbol && return _gc_rgb_copy(images[ex])
	c, A, B = ex.args[2], ex.args[3], ex.args[4]
	isrgb(x) = _gc_kind(x, kinds, Dict{Symbol,String}()) === :rgb
	# the side that stays is an RGB image; the other one is painted where the mask says so
	base, other, where = isrgb(A) ? (A, B, false) : (B, A, true)
	# `v@t`: the painted pixels get alpha 255·(1 - t/100), so the result carries an alpha band — built
	# with it from the start (`_gc_rgba_copy`), never added to a finished copy.
	tr = other isa Expr && other.head === :call && other.args[1] === :_gc_tr
	alpha = tr ? round(UInt8, 255 * (1 - other.args[3] / 100)) : nothing
	tr && (other = other.args[2])
	out = (tr && base isa Symbol) ? _gc_rgba_copy(images[base]) : _gc_rgb_eval(base, m, images, kinds, ref, arrays)
	(tr && size(out.image, 3) < 4) && (out = _gc_rgba_copy(out))   # a nested ifelse's picture
	nx, ny = _grid_dims(ref)
	# The mask as ONE concrete matrix read M[iy, ix] (iy from the south): the condition run by the same
	# node-by-node kernel as any expression, into one Bool per node — its only allocation — so the
	# loops below never branch on what kind of condition they were handed.
	cz = _gc_run!(Vector{Bool}(undef, length(ref.z)), c, m, arrays)
	M = _zmat(GMT.mat2grid(reshape(cz, size(ref.z)), ref))
	idx, npl, ncol = _gc_rgb_index(out)
	if other isa Integer || (other isa Expr && other.head === :tuple)
		col = other isa Integer ? ntuple(_ -> UInt8(other), 3) : ntuple(b -> UInt8(other.args[b]), 3)
		_gc_paint_color!(out.image, idx, npl, ncol, M, where, col, nx, ny,
		                 alpha === nothing ? -1 : Int(alpha))
	else
		# A second RGB image is only READ: its own buffer when it is already band-planar UInt8, never a
		# copy made just to be read from (a nested ifelse is the one case that builds a picture first).
		src = other isa Symbol ? _to_band_planar(images[other]) : _gc_rgb_eval(other, m, images, kinds, ref, arrays)
		eltype(src.image) == UInt8 || (src = _gc_rgb_copy(src))
		sidx, snpl, sncol = _gc_rgb_index(src)
		_gc_paint_image!(out.image, idx, npl, src.image, sidx, snpl, min(ncol, sncol), M, where, nx, ny)
	end
	return out
end

# The two paint loops, behind a function barrier: every argument arrives with its concrete type (the
# index closures, the mask view), so a 50-megapixel scene is one tight loop, not a dynamic call and an
# allocation per pixel.
function _gc_paint_color!(S::AbstractArray{UInt8}, idx::F, npl::Int, ncol::Int, M::AbstractMatrix{Bool},
                          where::Bool, col::NTuple{3,UInt8}, nx::Int, ny::Int, alpha::Int = -1) where {F}
	a = UInt8(clamp(alpha, 0, 255))          # alpha >= 0: also written into band 4 (the image has one)
	# FULLY transparent (`@100`): the colour is never seen, so the pixel KEEPS the picture's own colour
	# and only its alpha changes. Painting the number in instead (white, for 255@100) was not invisible:
	# the screen smooths the texture, so every edge pixel was blended with its white, alpha-0 neighbour
	# — a whitish ring around whatever stayed opaque (user's report, 2026-11-17).
	paint = alpha != 0
	Threads.@threads for iy in 1:ny          # rows write disjoint pixels: no locking
		@inbounds for ix in 1:nx
			M[iy, ix] == where || continue
			k = idx(ix, iy)
			if paint
				for b in 0:ncol-1
					S[k + b * npl] = col[b+1]
				end
			end
			alpha >= 0 && (S[k + 3 * npl] = a)
		end
	end
	return S
end

function _gc_paint_image!(S::AbstractArray{UInt8}, idx::F, npl::Int, T::AbstractArray{UInt8}, sidx::G,
                          snpl::Int, ncol::Int, M::AbstractMatrix{Bool}, where::Bool, nx::Int, ny::Int) where {F,G}
	Threads.@threads for iy in 1:ny          # rows write disjoint pixels: no locking
		@inbounds for ix in 1:nx
			M[iy, ix] == where || continue
			k = idx(ix, iy);  ks = sidx(ix, iy)
			for b in 0:ncol-1
				S[k + b * npl] = T[ks + b * snpl]
			end
		end
	end
	return S
end

# A LOGICAL expression (a comparison, `&`, `|`, `!` — anything whose result is true/false per node) is
# a MASK, and comes back as one: a Bool image, not a 0/1 Float32 grid. It sits on the reference grid's
# own geometry (`mat2img(mask, ref)` copies its x/y, increments, registration and CRS) and its buffer
# is the result as it lies, in `ref`'s element order (see `_gridcalc_eval`): a row-major `ref` holds its
# z as (ny,nx) dims over x-fastest memory, which an image of that layout describes as (nx,ny) -- the same
# bytes, only the dims change (the reverse of `_gc_image_grid`, so a mask is an operand again).
function _gc_mask_image(z::AbstractMatrix{Bool}, ref::GMTgrid)::GMTimage
	nx, ny = _grid_dims(ref)
	rowmajor = length(ref.layout) >= 2 && ref.layout[2] == 'R'
	m = z isa Array{Bool} ? vec(z) : Vector{Bool}(vec(z))   # no copy of a Bool Array; a BitMatrix unpacked once
	return GMT.mat2img(rowmajor ? reshape(m, nx, ny) : reshape(m, ny, nx), ref)
end

# A Bool GRID (what `G .> 100` gives in the console) is the same mask on a grid's nodes.
_gc_mask_image(G::GMTgrid{Bool,2})::GMTimage = _gc_mask_image(G.z, G)

# Does `expr` use a UInt16 band — a window image whose original is UInt16, or a "Load Grid" file
# GDAL stores as UInt16 (a Landsat / Sentinel band .TIF)? A name counts when it occurs in `expr`.
function _gc_from_uint16(scene::Ptr{Cvoid}, basenm::AbstractString, files::Dict{String,String},
                         expr::AbstractString)::Bool
	origs = get(_IMG_ORIG, scene, Dict{String,GMTimage}())
	for (kind, nm, data) in get(_SCENE_OBJS, scene, Tuple{Symbol,String,Any}[])
		(kind === :image && data isa GMTimage) || continue
		label = isempty(nm) ? String(basenm) : nm
		(!isempty(label) && occursin(label, expr) && eltype(get(origs, nm, data).image) == UInt16) && return true
	end
	for (nm, path) in files
		occursin(nm, expr) || continue
		t = try
			ds = GMT.Gdal.unsafe_read(path)
			tt = GMT.Gdal.GDALGetRasterDataType(GMT.Gdal.GDALGetRasterBand(ds.ptr, 1))
			GMT.Gdal.GDALClose(ds.ptr)
			tt
		catch
			UInt32(0)
		end
		t == GMT.Gdal.GDT_UInt16 && return true
	end
	return false
end

# An RGB result lands through the derived-image door every image tool uses (`_commit_derived_image!`);
# in a window with nothing in it yet (the operands all came from Load Grid files), the same add +
# transition with the window promoted — the two branches of the mask door (`_commit_mask!`).
function _gc_commit_rgb!(scene::Ptr{Cvoid}, I::GMTimage, name::String)::Cint
	if ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) != 0
		_commit_derived_image!(scene, I, name)
	else
		_add_image_to_scene(scene, I, name; promote=true) ||
			(_viewer_log_error(scene, "$name: window closed, image not added"); return Cint(0))
		_adopt_derived!(scene, name, I)
	end
	return Cint(1)
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGridCalcFn — expr / base / file1…fileN.
# Returns Cint 1 on success, 0 on failure — the dialog turns this into its own modal answer.
function _on_gridcalc(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		expr = strip(_get(d, "expr"))
		isempty(expr) && error("empty expression")
		occursin('`', expr) && error("backquotes are not allowed in an expression")

		files = Dict{String,String}()
		for k in 1:1000
			p = _get(d, "file$k");  isempty(p) && break
			files[basename(p)] = p
		end
		objs = _gc_with_reference(_gridcalc_objects(scene, _get(d, "base")), files, expr)

		alias = Ref(("", ""))
		z = _gridcalc_eval(objs, files, expr; rgbs = _gridcalc_rgbs(scene, _get(d, "base")), alias = alias)
		# Named by the operands' aliases (A, B, …), never by their full names; the legend goes to the
		# info console and, for a grid, into its recipe.
		short, legend = alias[]
		title = _gc_unique_name(scene, length(short) <= 60 ? short : first(short, 60) * "…")
		_viewer_log_info(scene, "Grid calculator: " * title * " — " * legend)
		z isa GMTimage && return _gc_commit_rgb!(scene, z, title)
		eltype(z) == Bool && return _commit_mask!(scene, _gc_mask_image(z, objs[1][2]), title)
		G = GMT.mat2grid(eltype(z) == Float32 ? z : Float32.(z), objs[1][2])   # already Float32: no copy
		_grid_command!(G, "InteractiveGMT grid calculator: " * String(expr) * "  (" * legend * ")")
		ok = _gm3d_deliver(scene, G, title, "", false, "gridcalc")
		# Computed from a Landsat / Sentinel band (UInt16 DNs): a measured quantity, not a relief —
		# born unshaded, the same rule as every RemoteS product (`_unshade_grid!`).
		(ok != 0 && _gc_from_uint16(scene, _get(d, "base"), files, expr)) && _unshade_grid!(scene, title)
		return ok
	catch e
		# Refused BEFORE computing: the user's expression, not a failure of the tool. Its message is the
		# answer, so it is shown as it is (2 = "already told", the dialog adds no box of its own).
		if e isa _GcRejected
			_viewer_log_info(scene, "Grid calculator: " * e.msg)
			_TEST_MODE[] || ccall(_fn(:gmtvtk_error_box), Cvoid, (Ptr{Cvoid}, Cstring, Cstring),
			                      scene, "Grid calculator", e.msg)
			return Cint(2)
		end
		_tool_failed(scene, "Grid calculator", e)
		return Cint(0)
	end
end

function _register_gridcalc()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_gridcalc, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_gridcalc_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
