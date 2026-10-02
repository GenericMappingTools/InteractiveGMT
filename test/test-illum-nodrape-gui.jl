# A SHADED GRID HAS NO DRAPE.
#
# Every Illumination method lights the grid on its OWN surface (hillshadeMapper). None may rebuild the
# grid as a flat image with an "Image drape" texture. Two regressions this guards:
#   * methods 2/3/4: gmtvtk_set_shade_intensity_h rebuilt the base as a flat image on every reflectance
#     push (rebuildBaseFromStored(asImage=true));
#   * method 7: the dialog switched the window into the flat-image geometry.
# On a grid with holes the flat picture also ended up UNDER the NaN plane, so the data vanished. The
# grid here is mostly NaN on purpose (an L2 SST swath gridded to a box looks like this), and the
# methods go through `_on_hillshade`, the one door every dialog / session request goes through.

@testitem "illumination: no method puts an Image drape on a shaded grid" tags=[:gui, :illum] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	using GMT
	n = 120
	z = Float32[10.0 + 5.0 * sin(i / 9) * cos(j / 7) + 0.3 * (i + j) / n for j in 1:n, i in 1:n]
	for j in 1:n, i in 1:n
		((i - n / 2)^2 + (j - n / 2)^2 < (0.38n)^2) && (z[j, i] = NaN32)   # a big hole, like a swath gap
	end
	G = mat2grid(z, x = collect(range(-16.0, 8.0, length = n)), y = collect(range(32.0, 49.0, length = n)))
	f = view_grid(G, geographic = true)
	png = joinpath(tempdir(), "ig_illum_nodrape_$(getpid()).png")
	# Share of sampled pixels that are coloured (the CPT), not grey (NaN fill / background).
	function coloured(h)
		ccall(IG._fn(:gmtvtk_save_png_h), Cint, (Ptr{Cvoid}, Cstring, Cint), h, png, 1)
		A = gmtread(png); v = vec(A.image); W, H = size(A.image, 1), size(A.image, 2)
		col = 0; tot = 0
		for y in round(Int, 0.2H):8:round(Int, 0.8H), x in round(Int, 0.2W):8:round(Int, 0.8W)
			k = ((y - 1) * W + (x - 1)) * 3
			r, g, b = Int(v[k+1]), Int(v[k+2]), Int(v[k+3])
			tot += 1;  (max(r, g, b) - min(r, g, b) > 25) && (col += 1)
		end
		return col / tot
	end
	try
		ve_pump(); h = f.h
		@test ve_state(h)["drape"] == 0
		base = coloured(h)
		@test base > 0.05                                   # the data really is on screen to start with
		for m in 1:7
			@test IG._on_hillshade(h, "model=$m\ngrid=\nazim=315\nelev=30") == 1
			ve_pump()
			@test ve_state(h)["drape"] == 0                          # no "Image drape"
			@test parse(Int, string(ve_state_full(h)["imgmode"])) == 0   # still the grid's own surface
			@test coloured(h) > 0.5 * base                      # the data did not vanish under the NaN plane
		end
	finally
		ve_close(f.h)
		rm(png, force = true)
	end
end
