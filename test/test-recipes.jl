# iview_recipe (src/recipes.jl): one method teaches iview a type it does not know.

@testitem "recipes: no recipe is an error that says how to write one" tags=[:unit] begin
	struct NoRecipe end
	err = try iview(NoRecipe()); nothing catch e; e end
	@test err isa ArgumentError
	@test occursin("iview_recipe(x::NoRecipe)", err.msg)
end

@testitem "recipes: a recipe must return something iview shows" tags=[:unit] begin
	struct BadRecipe end
	InteractiveGMT.iview_recipe(::BadRecipe) = 42
	err = try iview(BadRecipe()); nothing catch e; e end
	@test err isa ArgumentError
	@test occursin("returned a Int64", err.msg)
	struct EmptyRecipe end
	InteractiveGMT.iview_recipe(::EmptyRecipe) = []
	@test_throws ArgumentError iview(EmptyRecipe())
end

@testitem "recipes: a matrix-like type with a recipe reaches it; a plain matrix does not" tags=[:unit] begin
	IG = InteractiveGMT
	struct GridMat <: AbstractMatrix{Float64}; z::Matrix{Float64}; end
	Base.size(g::GridMat) = size(g.z)
	Base.getindex(g::GridMat, i::Int, j::Int) = g.z[i, j]
	@test !IG._has_recipe(rand(5, 3))
	@test !IG._has_recipe(GridMat(rand(5, 3)))
	IG.iview_recipe(::GridMat) = 42                    # a bad recipe proves the route without a window
	@test IG._has_recipe(GridMat(rand(5, 3)))
	err = try iview(GridMat(rand(5, 3))); nothing catch e; e end
	@test err isa ArgumentError && occursin("returned a Int64", err.msg)
end

@testitem "recipes: a recipe's grid opens in a window, its keywords passed on" tags=[:gui] begin
	IG = InteractiveGMT
	struct Bowl; n::Int; end
	x = collect(range(-1.0, 1.0, length=41))
	IG.iview_recipe(b::Bowl) = (IG.GMT.mat2grid(Float32[xi^2 + yi^2 for yi in x, xi in x], x=x, y=x),
	                            (; title="bowl $(b.n)"))
	f = iview(Bowl(1))
	try
		@test f isa QtFigure
		@test isalive(f)
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h); IG._pump_once()
	end
	# several layers: one window, every grid added to it
	IG.iview_recipe(b::Vector{Bowl}) = [IG.GMT.mat2grid(Float32[bi.n*(xi^2 + yi^2) for yi in x, xi in x], x=x, y=x) for bi in b]
	f = iview([Bowl(1), Bowl(2)])
	try
		@test f isa QtFigure
		@test isalive(f)
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h); IG._pump_once()
	end
end

@testitem "matrix: iview(M) is a grid (mat2grid), a plain Array or not" tags=[:gui] begin
	IG = InteractiveGMT
	for M in (Float32[i + j for i in 1:30, j in 1:40], transpose(rand(40, 30)))
		f = iview(M)
		try
			@test f isa QtFigure
			@test isalive(f)
		finally
			ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h); IG._pump_once()
		end
	end
end
