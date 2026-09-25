# Fixture + helpers for test-aquamoto-arrays-gui.jl.
#
# THE FIXTURE IS A REAL FILE OF THE REAL SHAPE. Every mask bug this suite guards was a bug in what
# the Aquamoto OPEN does with the arrays a tsunami file carries, so the tests must hand it such a
# file: ONE netCDF holding a 2-D Float32 `bathymetry`, a 3-D Float32 quantity cube `z`, and a 2-D
# **UInt8** companion MASK (`LongBeach`) — the exact layout NSWING writes and `tsu_time.nc` has
# (verified against it: bathymetry [385,640] Float32, z [76,385,640] Float32, LongBeach/ShortBeach
# [385,640] UInt8).
#
# It is built through GDAL's multidimensional API (GMT.libgdal — the same entry points shapenc.jl
# uses, no new dependency), because nothing else here can write a BYTE variable into a netCDF
# alongside float ones: `gmtwrite(file?var, …)` writes grids only, and a GDAL CreateCopy makes a
# single-variable file. The handles MUST be released before the dataset is closed or the file is
# left unreadable — that is not a detail, it is the difference between a fixture and 16 kB of
# garbage (found the hard way while writing this).
#
# Everything lands in a caller-supplied temporary directory and dies with it.

const AQF_LG = InteractiveGMT.GMT.libgdal

aqf_ck(h, what) = (h == C_NULL && error("aquamoto fixture: could not $what"); h)
aqf_edt(code::Int) = aqf_ck(ccall((:GDALExtendedDataTypeCreate, AQF_LG), Ptr{Cvoid}, (UInt32,), UInt32(code)),
                            "create the GDAL data type $code")

"""
    aqf_make_tsunami_nc(path; nx, ny, nt, mask_rows, mask_cols) -> path

Write a tsunami-shaped netCDF: `bathymetry` (Float32), `z` (Float32 cube, `nt` steps) and the UInt8
mask `LongBeach`, whose 1s fill the `mask_rows` x `mask_cols` block (everything else 0).
"""
#
# THE FIXTURE IS WIDER THAN 100 COLUMNS ON PURPOSE. `GMT.gdalread` sends a single-band netCDF down
# its VECTOR branch unless the raster is wider than that (gdal_utils.jl: `nraster > 1 || width >
# 100`), and the mask reader (`_aqua_mask_image`) is a gdalread call — a narrow fixture comes back
# as an empty GMTdataset and every item below fails for a reason that has nothing to do with the
# code under test. Real tsunami files are thousands of columns wide.
#
# `coast = true` gives the file a REAL COASTLINE instead of an all-wet tank: the bathymetry runs from
# deep water in the west to +200 m in the east across a wavy shoreline, and the quantity cube stores
# THE LAND ELEVATION ITSELF on every dry node — which is what an NSWING file does, and what
# `_aqua_isdry` (|bathymetry - stage| < 1e-2) recognises as dry. Without it the fixture has two dry
# nodes in the whole grid, so anything measured "on the land side" is measured on two points.
# `xrange`/`yrange` + `geog=false` build the same file on a CARTESIAN, METRE-SCALE grid instead of
# the default little geographic patch off Portugal. A benchmark's flume is kilometres of x in metres,
# and a test that wants the analytic reference curve to EXIST has to ask for it over a span the
# solution actually covers — over half a degree of longitude it comes back empty, for real reasons.
# Defaults are exactly what they were, so no existing fixture user sees a change.
function aqf_make_tsunami_nc(path::String; nx::Int = 128, ny::Int = 96, nt::Int = 4,
                             mask_rows = 30:45, mask_cols = 20:40, coast::Bool = false,
                             geog::Bool = true,
                             xrange::Tuple{Float64,Float64} = (-9.5, -9.0),
                             yrange::Tuple{Float64,Float64} = (38.5, 38.9))
	ccall((:GDALAllRegister, AQF_LG), Cvoid, ())
	drv = aqf_ck(ccall((:GDALGetDriverByName, AQF_LG), Ptr{Cvoid}, (Cstring,), "netCDF"), "find the netCDF driver")
	ds  = aqf_ck(ccall((:GDALCreateMultiDimensional, AQF_LG), Ptr{Cvoid},
	                   (Ptr{Cvoid}, Cstring, Ptr{Ptr{UInt8}}, Ptr{Ptr{UInt8}}), drv, path, C_NULL, C_NULL),
	             "create $path")
	grp = aqf_ck(ccall((:GDALDatasetGetRootGroup, AQF_LG), Ptr{Cvoid}, (Ptr{Cvoid},), ds), "open the root group")
	dims = Ptr{Cvoid}[]
	mkdim(n, sz) = (h = aqf_ck(ccall((:GDALGroupCreateDimension, AQF_LG), Ptr{Cvoid},
	                                 (Ptr{Cvoid}, Cstring, Cstring, Cstring, UInt64, Ptr{Ptr{UInt8}}),
	                                 grp, n, "", "", UInt64(sz), C_NULL), "create the dimension $n");
	                push!(dims, h); h)
	dt, dy, dx = mkdim("time", nt), mkdim("y", ny), mkdim("x", nx)
	f64, f32, u8 = aqf_edt(7), aqf_edt(6), aqf_edt(1)
	edtstr = aqf_ck(ccall((:GDALExtendedDataTypeCreateString, AQF_LG), Ptr{Cvoid}, (Csize_t,), Csize_t(0)),
	                "create the GDAL string type")
	function setattr(a, name::String, val::String)
		at = ccall((:GDALMDArrayCreateAttribute, AQF_LG), Ptr{Cvoid},
		           (Ptr{Cvoid}, Cstring, Csize_t, Ptr{UInt64}, Ptr{Cvoid}, Ptr{Ptr{UInt8}}),
		           a, name, Csize_t(0), C_NULL, edtstr, C_NULL)
		(at == C_NULL) && return
		ccall((:GDALAttributeWriteString, AQF_LG), Cint, (Ptr{Cvoid}, Cstring), at, val)
		ccall((:GDALAttributeRelease, AQF_LG), Cvoid, (Ptr{Cvoid},), at)
	end
	function arr(name, ds_, edt, data, shape; attrs::Vector{Pair{String,String}} = Pair{String,String}[])
		a = aqf_ck(ccall((:GDALGroupCreateMDArray, AQF_LG), Ptr{Cvoid},
		                 (Ptr{Cvoid}, Cstring, Csize_t, Ptr{Ptr{Cvoid}}, Ptr{Cvoid}, Ptr{Ptr{UInt8}}),
		                 grp, name, Csize_t(length(ds_)), collect(ds_), edt, C_NULL), "create the array $name")
		start = zeros(UInt64, length(shape));  cnt = UInt64.(collect(shape))
		r = ccall((:GDALMDArrayWrite, AQF_LG), Cint,
		          (Ptr{Cvoid}, Ptr{UInt64}, Ptr{UInt64}, Ptr{Int64}, Ptr{Int64}, Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Cvoid}, Csize_t),
		          a, start, cnt, C_NULL, C_NULL, edt, data, C_NULL, 0)
		(r == 0) && error("aquamoto fixture: writing '$name' failed")
		for (k, v) in attrs; setattr(a, k, v); end
		ccall((:GDALMDArrayRelease, AQF_LG), Cvoid, (Ptr{Cvoid},), a)
	end
	# Geographic, like the real thing, so the window opens with the same xfac/axes machinery.
	xs = collect(Float64, range(xrange[1], xrange[2], length = nx))
	ys = collect(Float64, range(yrange[1], yrange[2], length = ny))
	xattrs = geog ? ["axis" => "X", "long_name" => "longitude", "standard_name" => "longitude",
	                 "units" => "degrees_east"] :
	                ["axis" => "X", "long_name" => "x", "standard_name" => "projection_x_coordinate",
	                 "units" => "m"]
	yattrs = geog ? ["axis" => "Y", "long_name" => "latitude", "standard_name" => "latitude",
	                 "units" => "degrees_north"] :
	                ["axis" => "Y", "long_name" => "y", "standard_name" => "projection_y_coordinate",
	                 "units" => "m"]
	# The CF attributes are not decoration: without them GDAL gives the variables no geotransform,
	# and a raster with no georeference is not something the readers here will take.
	arr("x", (dx,), f64, xs, (nx,); attrs = xattrs)
	arr("y", (dy,), f64, ys, (ny,); attrs = yattrs)
	arr("time", (dt,), f64, collect(Float64, 1:nt), (nt,); attrs = ["axis" => "T", "units" => "seconds"])
	# Sea in the west, land rising to the east, so the composite really has two sides.
	# THE SHORELINE, when `coast` is on: depth/elevation across a wavy coast, and the cube carrying the
	# land's own elevation on the dry side (see the note above the signature).
	# On the metre-scale grid the shapes are written in NORMALISED coordinates (0..1 across the grid),
	# so the same sea-to-land ramp comes out whatever the extent is. The geographic case keeps its
	# original literals untouched: several items measure that fixture's own numbers.
	u(v, a, b) = (b > a) ? (v - a) / (b - a) : 0.0
	bathy = geog ?
		(coast ?
			Float32[-120 + 400 * ((x + 9.5) / 0.5) + 60 * sin(12 * (y - 38.5)) - 130 for x in xs, y in ys] :
			Float32[-40 + 160 * (x + 9.5) + 20 * (y - 38.5) for x in xs, y in ys]) :
		Float32[-2000 + 2100 * u(x, xs[1], xs[end]) + 20 * u(y, ys[1], ys[end]) for x in xs, y in ys]
	arr("bathymetry", (dy, dx), f32, vec(bathy), (ny, nx); attrs = ["long_name" => "bathymetry"])
	cube = coast ?
		Float32[bathy[i, j] > 0 ? bathy[i, j] :                       # DRY: the stage IS the ground
		        Float32(2.5 * sin(0.6 * k + 26 * (xs[i] + 9.5)) * exp(-((ys[j] - 38.7) / 0.12)^2))
		        for i in 1:nx, j in 1:ny, k in 1:nt] :
		(geog ?
			Float32[0.1f0 * sin(Float32(k + 20 * (x + 9.5) + 10 * (y - 38.5))) for x in xs, y in ys, k in 1:nt] :
			# A wave that MOVES: the phase walks with k, so consecutive timesteps are genuinely
			# different profiles — which is what "the figure followed the slice" is measured on.
			Float32[0.8f0 * sin(Float32(0.7k + 12 * u(x, xs[1], xs[end]))) for x in xs, y in ys, k in 1:nt])
	arr("z", (dt, dy, dx), f32, vec(cube), (nt, ny, nx); attrs = ["long_name" => "z"])
	msk = zeros(UInt8, nx, ny)
	msk[mask_cols, mask_rows] .= 0x01
	arr("LongBeach", (dy, dx), u8, vec(msk), (ny, nx); attrs = ["long_name" => "LongBeach"])
	for h in dims; ccall((:GDALDimensionRelease, AQF_LG), Cvoid, (Ptr{Cvoid},), h); end
	ccall((:GDALGroupRelease, AQF_LG), Cvoid, (Ptr{Cvoid},), grp)   # release BEFORE the close, or the
	ccall((:GDALClose, AQF_LG), Cvoid, (Ptr{Cvoid},), ds)           # file is written unreadable
	return path
end

# How many nodes the fixture's mask has set — what the loaded picture must still contain.
aqf_mask_count(mask_rows = 30:45, mask_cols = 20:40) = length(mask_rows) * length(mask_cols)

aqf_pump(n::Int = 25) = for _ in 1:n; InteractiveGMT._pump_once(); sleep(0.02); end
aqf_close(h) = (ccall(InteractiveGMT._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h); aqf_pump(5))

# Show / hide a Scene Objects handle by name — the viewer's own setter, the one the panel's rows and
# every tool go through. Returns true if the handle was found.
aqf_show(h, name::AbstractString, on::Bool) =
	ccall(InteractiveGMT._fn(:gmtvtk_set_object_visible), Cint,
	      (Ptr{Cvoid}, Cstring, Cint), h, String(name), Cint(on ? 1 : 0)) != 0

# Grab the whole window to a PNG (the viewer's own screenshot export).
aqf_shoot(h, path::AbstractString) =
	ccall(InteractiveGMT._fn(:gmtvtk_window_screenshot), Cint,
	      (Ptr{Cvoid}, Cstring), h, String(path)) != 0

# HOW MANY OF THE MASK'S OWN PIXELS ARE ON SCREEN, counted on its BLACK state.
#
# A mask is a black-and-white picture, and PURE BLACK is the one tone nothing else in this window
# draws: the chrome is light grey (0xf3), the composite is a colour ramp, and the axes' text is
# antialiased grey. So a frame's pure-black pixels are the mask's off-state and nothing else —
# measured 0 in every frame taken before a mask is checked. A layer that is registered, checked and
# invisible therefore still cannot satisfy this, which is the bug these tests exist for.
#
# IT USED TO COUNT WHITE, AND THAT WAS NOT A MEASUREMENT OF THE MASK. `> 0xf2 on all three channels`
# also matches the window chrome and the WATER PALETTE'S OWN WHITE BAND (:polar is white at zero, and
# a calm stage is mostly zero), which together are ~300k pixels of a 4.4 Mpx frame. Checking the mask
# then LOWERED the count — its opaque black covers more of that white band than its own white block
# adds — so the assertion "the mask put pixels on screen" failed while the mask was plainly on screen
# (verified by eye: white block on a black field). Never a whole-image comparison either: the tsunami
# surface moves between frames all by itself.
function aqf_mask_pixels(png::AbstractString)
	I = InteractiveGMT.GMT.gmtread(String(png))
	A = I.image
	(ndims(A) == 2) && return count(==(0x00), A)
	n = 0
	@inbounds for k in 1:size(A, 1), j in 1:size(A, 2)
		(A[k, j, 1] == 0x00 && A[k, j, 2] == 0x00 && A[k, j, 3] == 0x00) && (n += 1)
	end
	return n
end

# Total pixels in that frame, so a count can be judged as a FRACTION of the view.
function aqf_pixel_count(png::AbstractString)
	A = InteractiveGMT.GMT.gmtread(String(png)).image
	return ndims(A) == 2 ? length(A) : size(A, 1) * size(A, 2)
end

# The kinds the host registry holds for this window: [(:grid|:image, name), …].
aqf_objects(h) = [(k, n) for (k, n, _) in get(InteractiveGMT._SCENE_OBJS, h, Tuple{Symbol,String,Any}[])]
aqf_object(h, kind::Symbol, name::AbstractString) = InteractiveGMT._find_object_exact(h, kind, String(name))

# (Used by test-aquamoto-transport-gui.jl; needs GmtvtkTest, i.e. setup=[GmtvtkTest].)
# REAL OS MOUSE INPUT. A synthetic event sent to the button bypasses Qt's mouse grab, and the bug
# this item exists for was a mouse-up that never REACHED Qt with Sat img on: the arrow stayed down
# and auto-repeated through the layers after the finger was off. Only a real press/release shows it.
# The ALT tap lets the window take the foreground (Windows refuses SetForegroundWindow otherwise),
# and the item checks the arrow really is the window under the cursor before it trusts anything.
function aqf_realpress(h, dir, down)
	if down
		ccall((:keybd_event, "user32"), Cvoid, (UInt8, UInt8, UInt32, UInt), 0x12, 0, 0, 0)
		ccall((:keybd_event, "user32"), Cvoid, (UInt8, UInt8, UInt32, UInt), 0x12, 0, 2, 0)
		x = Ref{Cint}(0); y = Ref{Cint}(0)
		@assert ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_arrow_screen_test), Cint,
		              (Ptr{Cvoid}, Cint, Ptr{Cint}, Ptr{Cint}), h, Cint(dir), x, y) == 1
		aqf_pump(10)
		ccall((:SetCursorPos, "user32"), Cint, (Cint, Cint), x[], y[]);  sleep(0.2)
		buf = zeros(UInt16, 256)
		hw = ccall((:WindowFromPoint, "user32"), Ptr{Cvoid}, (NTuple{2,Cint},), (x[], y[]))
		ccall((:GetWindowTextW, "user32"), Cint, (Ptr{Cvoid}, Ptr{UInt16}, Cint),
		      ccall((:GetAncestor, "user32"), Ptr{Cvoid}, (Ptr{Cvoid}, UInt32), hw, 2), buf, 256)
		under = transcode(String, buf[1:findfirst(==(0), buf)-1])
		@test under == "Aquamoto"                 # the click lands on the arrow, not on another app
		v = Int(ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_slider_value_test), Cint, (Ptr{Cvoid},), h))
		ccall((:mouse_event, "user32"), Cvoid, (UInt32, UInt32, UInt32, UInt32, UInt), 0x0002, 0, 0, 0, 0)
		return v
	end
	ccall((:mouse_event, "user32"), Cvoid, (UInt32, UInt32, UInt32, UInt32, UInt), 0x0004, 0, 0, 0, 0)
	sleep(0.3)                                  # the up is queued: let the pump deliver it
	return Int(ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_slider_value_test), Cint, (Ptr{Cvoid},), h))
end
