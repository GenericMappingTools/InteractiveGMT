# setopacity! and setvisible!(…; layer=…): a layer's opacity lives in its own look (so tiles meshed
# later inherit it), and a row every layer carries under the same name ("Axes") is reached inside
# ONE layer's group.

@testitem "setopacity!: the window's grid and an added layer, unknown layer is an error" tags=[:gui] begin
	IG = InteractiveGMT; GMT = IG.GMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	G  = ve_grid(zspan = 1000.0)
	G2 = ve_grid(zspan = 500.0)
	fig = IG.view_grid(G); ve_pump()
	h = getfield(fig, :h)
	try
		@test IG.setopacity!(fig, 0.5) === fig
		IG.add!(fig, G2; name = "Second")
		ve_pump()
		@test IG.setopacity!(fig, 0.3; layer = "Second") === fig
		@test_throws ErrorException IG.setopacity!(fig, 0.3; layer = "No such layer")
	finally
		ve_close(h)
	end
end

@testitem "setvisible!: one layer's own Axes row" tags=[:gui] begin
	IG = InteractiveGMT; GMT = IG.GMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	G  = ve_grid(zspan = 1000.0)
	G2 = ve_grid(zspan = 500.0)
	fig = IG.view_grid(G); ve_pump()
	h = getfield(fig, :h)
	try
		IG.add!(fig, G2; name = "Second")
		ve_pump()
		@test IG.setvisible!(fig, "Axes", false; layer = "Second") === fig
		@test_throws ErrorException IG.setvisible!(fig, "Axes", false; layer = "No such layer")
	finally
		ve_close(h)
	end
end
