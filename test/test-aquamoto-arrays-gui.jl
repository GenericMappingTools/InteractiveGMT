# THE AQUAMOTO LOADED-ARRAYS REGRESSION SUITE.
#
# Every item here exists because the behaviour it asserts broke on a real tsunami file (tsu_time.nc:
# a Float32 `bathymetry`, a Float32 `z` cube, and the UInt8 masks LongBeach / ShortBeach) and cost a
# day. They use ONLY what the viewer already exports — nothing in deps/src or src was added, changed
# or instrumented for them.
#
# THE TEST THAT USED TO GUARD THIS AREA WAS DELETED because it proved nothing: it built a mask by
# hand, applied a palette by hand, and asserted the result was black and white. It could not fail
# while the loading path, the placement and the panel were all broken, and it stayed green through
# every one of them. Two rules came out of that:
#
#   1. LOAD THE FILE. Every item opens a REAL multi-variable netCDF of the real shape
#      (aquamoto_fixture.jl) through the SAME door a user drops a file on, and then looks at the
#      window. Nothing is assembled in the test and handed to a helper.
#   2. MEASURE THE RESULT, not the intent. "The mask is visible" is decided by RENDERING the window
#      and counting the mask's own white pixels — the one measurement that cannot be satisfied by a
#      layer that is registered, checked, and invisible, which is exactly what the bug was.
#
# WHAT IS GUARDED
#   A. A byte companion arrives as a two-state UInt8 MASK IMAGE on the simulation's own ground, and
#      its bytes survive the trip. A mask read as a grid — a field of numbers with a colour scale —
#      is the violation.
#   B. A checked mask IS ON SCREEN. It used to be parked at a height decided before the Aquamoto
#      composite existed, and the surface then stood over it: checked in the panel, nothing drawn.
#   C. Checking and unchecking the companions, over and over, mixed with slice changes, leaves the
#      window ALIVE and still working. The panel used to be rebuilt from inside a row's own handler,
#      freeing the widgets that were mid-dispatch.
#
# NOT COVERED HERE, and deliberately not faked: the row WIDGETS themselves (the checkbox cascade is
# reachable only through Qt, and no export drives it), and the mask's colour-bar row state (no export
# reports it). Both were measured by hand during the fix; a test for either needs a viewer hook, and
# that is a code change nobody asked for.

@testitem "Aquamoto arrays A: a byte companion loads as a two-state UInt8 MASK IMAGE, bytes intact" tags=[:gui, :aquamoto] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))
	mktempdir() do dir
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_fixture.nc"))
		# The fixture must really have the shape this suite claims: a fixture that silently degrades
		# (a float "mask", say) would make every assertion below vacuous.
		subs = Dict(v.name => v for v in IG._netcdf_subdatasets(nc))
		@test subs["bathymetry"].typ == "Float32" && length(subs["bathymetry"].dims) == 2
		@test subs["z"].typ == "Float32"          && length(subs["z"].dims) == 3
		@test subs["LongBeach"].typ == "UInt8"    && length(subs["LongBeach"].dims) == 2

		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(40)
			objs = aqf_objects(f.h)
			@test (:image, "LongBeach")  in objs        # a MASK is a picture…
			@test (:grid,  "bathymetry") in objs        # …a field of numbers is a grid
			@test !((:grid, "LongBeach") in objs)       # never the wrong kind, never both
			I = aqf_object(f.h, :image, "LongBeach")
			@test I isa IG.GMT.GMTimage
			@test eltype(I.image) == UInt8                       # a mask is UInt8 at most
			@test sort(unique(I.image)) == UInt8[0x00, 0x01]     # two states, nothing rescaled
			@test count(==(0x01), I.image) == aqf_mask_count()   # every flagged node survived
			@test IG._img_is_indexed(I)                          # its values are indices into a palette
			# It covers the SAME ground as the simulation. The image is pixel-registered (its range is
			# cell EDGES) and the grid node-registered (cell CENTRES), so they differ by exactly half a
			# cell — more than that means the mask landed somewhere else.
			G = aqf_object(f.h, :grid, "bathymetry")
			dx = Float64(G.inc[1]);  dy = Float64(G.inc[2])
			@test isapprox(Float64(I.range[1]), Float64(G.range[1]); atol = 0.5dx + 1e-9)
			@test isapprox(Float64(I.range[2]), Float64(G.range[2]); atol = 0.5dx + 1e-9)
			@test isapprox(Float64(I.range[3]), Float64(G.range[3]); atol = 0.5dy + 1e-9)
			@test isapprox(Float64(I.range[4]), Float64(G.range[4]); atol = 0.5dy + 1e-9)
			@test sort(collect(size(I.image))) == sort(collect(size(G.z)))   # same lattice, not a resample
		finally
			aqf_close(f.h)
		end
	end
end

@testitem "Aquamoto arrays B: a checked mask is ON SCREEN, measured in pixels" tags=[:gui, :aquamoto] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))
	mktempdir() do dir
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_fixture.nc"))
		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(40)
			shot = joinpath(dir, "before.png")
			@test aqf_shoot(f.h, shot)
			before = aqf_mask_pixels(shot)       # the mask is unchecked: its whites are not drawn yet

			@test aqf_show(f.h, "LongBeach", true);  aqf_pump(15)
			after = joinpath(dir, "after.png")
			@test aqf_shoot(f.h, after)
			whites = aqf_mask_pixels(after)
			# THE REGRESSION, in the only terms that cannot lie: checking the mask must put its white
			# region on the screen. It used to add nothing at all — the plane was parked at a height
			# fixed before the composite existed, and the composite then stood over it.
			@test whites > before + 200
			# …and the picture drawn is the MASK, not a tinted copy of something else: its whites are
			# white (a black-and-white picture, opaque), and they cover a small part of the view.
			@test whites < 0.5 * aqf_pixel_count(after)

			# It must STAY on screen when the window's geometry moves under it — a new slice, a
			# view-mode switch. Each of those re-runs the whole placement machinery.
			IG._aquamoto_slice(f.h, 2, true, false, 0.0, true, true);  aqf_pump(10)
			ccall(IG._fn(:gmtvtk_set_view_mode_h), Cint, (Ptr{Cvoid}, Cint), f.h, Cint(0));  aqf_pump(10)
			ccall(IG._fn(:gmtvtk_set_view_mode_h), Cint, (Ptr{Cvoid}, Cint), f.h, Cint(1));  aqf_pump(10)
			later = joinpath(dir, "later.png")
			@test aqf_shoot(f.h, later)
			@test aqf_mask_pixels(later) > before + 200
		finally
			aqf_close(f.h)
		end
	end
end

@testitem "Aquamoto arrays C: toggling the companions, over and over, never kills the window" tags=[:gui, :aquamoto] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))
	mktempdir() do dir
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_fixture.nc"))
		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(40)
			# The companions a tsunami window carries, each shown and hidden every round. ("z" is the
			# composited quantity, addressed as the window's own surface, so it is toggled through the
			# same setter under the file's name.)
			rounds = 25
			names  = ("LongBeach", "bathymetry")
			toggles = 0
			for round in 1:rounds
				for name in names, on in (true, false)
					aqf_show(f.h, name, on) && (toggles += 1)
				end
				(round % 5 == 0) && (IG._aquamoto_slice(f.h, round % 4, true, false, 0.0, true, true); aqf_pump(3))
			end
			aqf_pump(20)
			# EVERY toggle must have landed: a name that stops resolving is itself a regression (a
			# companion silently dropped from the panel), not a reason to loosen the count.
			@test toggles == rounds * length(names) * 2
			@test IG.isalive(f)                      # …and the window survived every one of them
			# Still a WORKING window, not a corpse that merely answers: it draws another slice, shows
			# the mask again, and the mask's pixels are back on screen.
			IG._aquamoto_slice(f.h, 1, true, false, 0.0, true, true);  aqf_pump(10)
			@test aqf_show(f.h, "LongBeach", true);  aqf_pump(10)
			shot = joinpath(dir, "after_hammer.png")
			@test aqf_shoot(f.h, shot)
			@test aqf_mask_pixels(shot) > 200
		finally
			aqf_close(f.h)
		end
	end
end
