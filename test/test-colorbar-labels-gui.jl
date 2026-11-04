# A colour bar's X / Y labels (its Scene Objects "Color Bar" row > "Labels…"): drawn on the bar, and
# handed to the GMT.jl script's colorbar call.

@testitem "colour bar labels: set from the Color Bar row, drawn, and in the GMT.jl script" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; tf = GmtvtkTest._test_fn
	labels(h) = (n = ccall(IG._fn(:gmtvtk_colorbar_labels_h), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, C_NULL, 0);
		n <= 0 ? "" : (b = zeros(UInt8, n + 1);
			ccall(IG._fn(:gmtvtk_colorbar_labels_h), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, b, n + 1);
			unsafe_string(pointer(b))))
	set(h, x, y) = ccall(tf(:gmtvtk_colorbar_labels_test), Cint, (Ptr{Cvoid}, Cstring, Cstring, Cstring), h, "", x, y)
	G = IG.GMT.peaks()
	fig = IG.viz(G)
	try
		IG._pump_once()
		@test IG._scene_state(fig.h)["bar"] == 1
		@test labels(fig.h) == ""                                       # none until asked for
		@test set(fig.h, "Elevation", "m") == 1                         # the row's own menu and dialog
		IG._pump_once()
		@test labels(fig.h) == "Elevation\x1fm"                         # the bar on screen wears them
		@test IG._scene_state(fig.h)["bar"] == 1
		txt = IG.gmtscript(fig)
		@test occursin("xlabel = \"Elevation\"", txt) || occursin("xlabel=\"Elevation\"", txt)
		@test occursin("ylabel = \"m\"", txt) || occursin("ylabel=\"m\"", txt)
		# the labels belong to the layer: re-drawing the bar (a colormap change) keeps them
		IG._recolor_grid(fig.h, "turbo", Float64(minimum(G.z)), Float64(maximum(G.z)), -1)
		IG._pump_once()
		@test labels(fig.h) == "Elevation\x1fm"
		# emptied again: the bar and the script carry none
		@test set(fig.h, "", "") == 1
		IG._pump_once()
		@test labels(fig.h) == ""
		@test !occursin("xlabel", IG.gmtscript(fig))
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), fig.h)
		IG._pump_once()
	end
end
