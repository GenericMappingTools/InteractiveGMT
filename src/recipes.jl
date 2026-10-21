# Recipes: how iview shows a type it does not know.
#
# A recipe is ONE method of ONE function. It turns the foreign object into something iview already
# shows -- a GMTgrid, GMTimage, GMTdataset (or a Vector of them), GMTfv -- and the object then goes
# through iview's own doors like any other: own axes, Scene Objects rows, the lot. A recipe never
# draws anything itself.
#
#     InteractiveGMT.iview_recipe(x::MyType) = mat2grid(x.z, x=x.lon, y=x.lat)
#
# Keywords the recipe wants passed to iview come back in a NamedTuple; the user's own keywords
# win over the recipe's:
#
#     InteractiveGMT.iview_recipe(x::MyType) = (mat2grid(x.z), (; cmap=:batlow))
#
# Several layers in one window: return a Vector. The first is the window, the rest are add!-ed.
#
#     InteractiveGMT.iview_recipe(s::MyStack) = [mat2grid(l) for l in s.layers]

"""
	iview_recipe(x) -> data   or   (data, kwargs::NamedTuple)   or   Vector of either

Tell [`iview`](@ref) how to show an object of a type it does not know: return what iview
already shows (`GMTgrid`, `GMTimage`, `GMTdataset`, `Vector{GMTdataset}`, `GMTfv`). Add a method
for your type and `iview(x)` works. Returning a `Vector` puts every element in the same window.

!!! warning "Experimental"
    The recipe system is experimental: its name, signature and accepted returns may change.
"""
function iview_recipe(x)
	f = _pkg_recipe(x)                         # a recipe iGMT carries for x's package (package_recipes.jl)
	r = f === nothing ? nothing : f(Base.moduleroot(parentmodule(typeof(x))), x)
	r === nothing || return r
	throw(ArgumentError("iview does not know how to show a $(typeof(x)). Teach it with one method: " *
	                    "InteractiveGMT.iview_recipe(x::$(nameof(typeof(x)))) = <a GMTgrid, GMTimage or GMTdataset>"))
end

# Has `x`'s type a recipe: a method of its own (not the fallback above), or one for its package?
_has_recipe(x) = which(iview_recipe, Tuple{typeof(x)}) !== which(iview_recipe, Tuple{Any}) || _pkg_recipe(x) !== nothing

const _RecipeOut =Union{GMTgrid, GMTimage, GMTdataset, AbstractVector{<:GMTdataset}, GMTfv}

iview(x; kwargs...) = _iview_recipe(iview_recipe(x), values(kwargs), typeof(x))

_iview_recipe(d::_RecipeOut, kw::NamedTuple, ::Type) = iview(d; kw...)
_iview_recipe((d, rkw)::Tuple{Any,NamedTuple}, kw::NamedTuple, T::Type) = _iview_recipe(d, merge(rkw, kw), T)
function _iview_recipe(v::Vector, kw::NamedTuple, T::Type)
	isempty(v) && throw(ArgumentError("the iview_recipe for $T returned no layers"))
	fig = _iview_recipe(first(v), kw, T)
	for e in v[2:end]
		d, ekw = e isa Tuple ? e : (e, (;))
		d isa _RecipeOut || _recipe_bad(d, T)
		add!(fig, d; ekw...)
	end
	return fig
end
_iview_recipe(d, ::NamedTuple, T::Type) = _recipe_bad(d, T)
_recipe_bad(d, T::Type) = throw(ArgumentError("the iview_recipe for $T returned a $(typeof(d)); it must return a " *
                                              "GMTgrid, GMTimage, GMTdataset, Vector{GMTdataset} or GMTfv"))
