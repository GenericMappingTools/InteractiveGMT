# CI-safe unit tests for the GMT-module dialogs added to the GMT menu (grdtrend, grdlandmask,
# grdfilter, gravfft, grdrotater, talwani2d/3d, greenspline and the rest). These cover what can be
# checked WITHOUT a window: the callbacks are registered, and
# every input the dialog can send that the wrapper must REFUSE comes back as 0 rather than running
# GMT with a nonsense option. A refusal never reaches the C side (the failure is logged through
# _viewer_log_error, which swallows a dead/absent handle), so no DLL is needed.
# The success paths need a live window and live GMT — those are the :gui items in
# test-gmtmodules-gui.jl.

@testitem "GMT-module callbacks are wired" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	for s in (:_on_grdtrend, :_register_grdtrend, :_on_grdlandmask, :_register_grdlandmask,
	          :_on_grdfilter, :_register_grdfilter, :_on_gridcalc, :_register_gridcalc,
	          :_on_grdfft, :_register_grdfft, :_on_grdhisteq, :_register_grdhisteq,
	          :_on_xyz2grd, :_register_xyz2grd, :_on_grdfill, :_register_grdfill,
	          :_on_trend2d, :_register_trend2d, :_on_cptbuild, :_register_cptbuild,
	          :_on_gravfft, :_register_gravfft, :_on_grdrotater, :_register_grdrotater,
	          :_on_talwani2d, :_register_talwani2d, :_on_talwani3d, :_register_talwani3d,
	          :_on_greenspline, :_register_greenspline,
	          :_on_gmtflexure, :_register_gmtflexure, :_on_grdflexure, :_register_grdflexure,
	          :_on_grdvolume, :_register_grdvolume, :_on_gravprisms, :_register_gravprisms,
	          :_on_grdvector, :_register_grdvector,
	          :_on_earthregions, :_register_earthregions)
		@test isdefined(IG, s)
	end
	# Every registration must have its export in the DLL symbol list, or the feature silently stays
	# "not wired" at runtime (new-c-export-needs-lib-symbols).
	for sym in (:gmtvtk_set_grdtrend_callback, :gmtvtk_set_grdlandmask_callback,
	            :gmtvtk_set_grdfilter_callback, :gmtvtk_set_gridcalc_callback,
	            :gmtvtk_set_grdfft_callback, :gmtvtk_set_grdhisteq_callback,
	            :gmtvtk_set_xyz2grd_callback, :gmtvtk_set_grdfill_callback,
	            :gmtvtk_set_trend2d_callback, :gmtvtk_set_cptbuild_callback,
	            :gmtvtk_set_gravfft_callback, :gmtvtk_set_grdrotater_callback,
	            :gmtvtk_set_talwani2d_callback, :gmtvtk_set_talwani3d_callback,
	            :gmtvtk_set_greenspline_callback, :gmtvtk_set_gmtflexure_callback,
	            :gmtvtk_set_grdflexure_callback, :gmtvtk_set_grdvolume_callback,
	            :gmtvtk_set_gravprisms_callback, :gmtvtk_set_grdvector_callback,
	            :gmtvtk_set_earthregions_callback, :gmtvtk_earthregions_set_listing)
		@test sym in IG._LIB_SYMBOLS
	end
end

# One fake window holding one grid: enough for every wrapper to get past its grid lookup and reach
# the validation we want to see fire.
@testitem "grdtrend: refuses what the module cannot do" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[2 + 0.3ix + 0.2iy for iy in 0:9, ix in 0:9];
	                 x = collect(range(0, 9, length = 10)), y = collect(range(0, 9, length = 10)))
	scene = Ptr{Cvoid}(UInt(0x9D712E4D))
	IG._SCENE_OBJS[scene] = Tuple{Symbol,String,Any}[(:grid, "topo", G)]
	call(kv) = IG._on_grdtrend(scene, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n"))))
	try
		# The weights are a by-product of the ROBUST fit; asking for them without it is refused.
		@test call(["what=weights", "model=3", "robust=0", "axis=", "grid=topo"]) == 0
		# A 1-D (+x/+y) fit only has 4 terms; a surface has 10.
		@test call(["what=trend", "model=7", "robust=0", "axis=y", "grid=topo"]) == 0
		@test call(["what=trend", "model=11", "robust=0", "axis=", "grid=topo"]) == 0
		@test call(["what=trend", "model=0", "robust=0", "axis=", "grid=topo"]) == 0
		# A weight grid that is not there.
		@test call(["what=trend", "model=3", "robust=0", "axis=", "wfile=no_such_grid.nc", "grid=topo"]) == 0
		# No grid in the window at all.
		@test IG._on_grdtrend(Ptr{Cvoid}(UInt(0xDEADDEAD)),
		                      Base.unsafe_convert(Cstring, Base.cconvert(Cstring, "what=trend\nmodel=3"))) == 0
	finally
		delete!(IG._SCENE_OBJS, scene)
	end
end

@testitem "grdlandmask: refuses an incomplete geometry" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(fill(1.0f0, 10, 10); x = collect(range(-10, -6, length = 10)),
	                                       y = collect(range(36, 39, length = 10)))
	scene = Ptr{Cvoid}(UInt(0x9D714A5C))
	IG._SCENE_OBJS[scene] = Tuple{Symbol,String,Any}[(:grid, "topo", G)]
	call(kv) = IG._on_grdlandmask(scene, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n"))))
	try
		# A half-filled Griding Line Geometry leaves an empty field in the region string.
		@test call(["clip=0", "region=-10//36/39", "inc=0.1", "res=l", "grid=topo"]) == 0
		@test call(["clip=0", "region=", "inc=0.1", "res=l", "grid=topo"]) == 0
		@test call(["clip=0", "region=-10/-6/36/39", "inc=", "res=l", "grid=topo"]) == 0
		# Masking "this window's grid" needs one.
		@test IG._on_grdlandmask(Ptr{Cvoid}(UInt(0xDEADBEE5)),
		                         Base.unsafe_convert(Cstring, Base.cconvert(Cstring, "clip=1\nres=l"))) == 0
	finally
		delete!(IG._SCENE_OBJS, scene)
	end
end

@testitem "grdfill / grdhisteq / grdfft: refuse what the module cannot do" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9];
	                 x = collect(range(0, 9, length = 10)), y = collect(range(0, 9, length = 10)))
	scene = Ptr{Cvoid}(UInt(0x9D71F111))
	IG._SCENE_OBJS[scene] = Tuple{Symbol,String,Any}[(:grid, "topo", G)]
	cs(kv) = Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n")))
	try
		# grdfill: the algorithm is the whole question, and a constant needs its constant.
		@test IG._on_grdfill(scene, cs(["mode=fill", "algo=", "grid=topo"])) == 0
		@test IG._on_grdfill(scene, cs(["mode=fill", "algo=c", "value=", "grid=topo"])) == 0
		@test IG._on_grdfill(scene, cs(["mode=fill", "algo=c", "value=abc", "grid=topo"])) == 0
		@test IG._on_grdfill(scene, cs(["mode=fill", "algo=s", "tension=1.5", "grid=topo"])) == 0
		@test IG._on_grdfill(scene, cs(["mode=fill", "algo=g", "gridfile=no_such_grid.nc", "grid=topo"])) == 0
		@test IG._on_grdfill(Ptr{Cvoid}(UInt(0xDEADF111)), cs(["mode=fill", "algo=n"])) == 0
		# grdhisteq: normal scores are exclusive with the quadratic cell distribution.
		@test IG._on_grdhisteq(scene, cs(["mode=grid", "flavour=gaussian", "quadratic=1", "grid=topo"])) == 0
		@test IG._on_grdhisteq(scene, cs(["mode=grid", "flavour=gaussian", "norm=-1", "grid=topo"])) == 0
		@test IG._on_grdhisteq(scene, cs(["mode=grid", "flavour=cells", "ncells=0", "grid=topo"])) == 0
		@test IG._on_grdhisteq(scene, cs(["mode=nonsense", "grid=topo"])) == 0
		# grdfft: -Q means "no operation", so it cannot carry one; and an empty dialog does nothing.
		@test IG._on_grdfft(scene, cs(["mode=grid", "noop=1", "upward=1000", "grid=topo"])) == 0
		@test IG._on_grdfft(scene, cs(["mode=grid", "grid=topo"])) == 0
		@test IG._on_grdfft(scene, cs(["mode=grid", "upward=abc", "grid=topo"])) == 0
	finally
		delete!(IG._SCENE_OBJS, scene)
	end
end

@testitem "xyz2grd / trend2d / Make CPT: refuse what the module cannot do" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	scene = Ptr{Cvoid}(UInt(0x9D71F222))
	cs(kv) = Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n")))
	# xyz2grd / trend2d read their INPUT FILE first, so the callback refuses a missing table before it
	# ever looks at the rest. The option checks themselves are therefore asked of the helpers that own
	# them — the same functions the callback calls one line later.
	@test IG._on_xyz2grd(scene, cs(["infile=", "region=0/9/0/9", "inc=1"])) == 0
	@test IG._on_xyz2grd(scene, cs(["infile=no_such.xyz", "region=0/9/0/9", "inc=1"])) == 0
	@test_throws Exception IG._x2g_geometry(Dict("region" => "0//0/9", "inc" => "1"))
	@test_throws Exception IG._x2g_geometry(Dict("region" => "0/9/0/9", "inc" => ""))
	@test IG._x2g_geometry(Dict("region" => "0/9/0/9", "inc" => "1")) == ("0/9/0/9", "1")
	# trend2d: 1..10 terms, and at least one output column.
	@test IG._on_trend2d(scene, cs(["infile=no_such.xy", "model=3", "col_x=1"])) == 0
	@test_throws Exception IG._trend2d_N(Dict("model" => "0"))
	@test_throws Exception IG._trend2d_N(Dict("model" => "11"))
	@test IG._trend2d_N(Dict("model" => "3", "robust" => "1")) == "3+r"
	@test_throws Exception IG._trend2d_F(Dict{String,String}())
	@test IG._trend2d_F(Dict("col_x" => "1", "col_z" => "1", "col_m" => "1")) == "xzm"
	# Make CPT: neither applied nor saved is nothing to do; and the option values are checked.
	@test IG._on_cptbuild(scene, cs(["mode=make", "master=turbo", "apply=0", "outfile="])) == 0
	@test IG._on_cptbuild(scene, cs(["mode=nonsense", "outfile=x.cpt"])) == 0
	@test IG._on_cptbuild(scene, cs(["mode=make", "invert=q", "outfile=x.cpt"])) == 0
	@test IG._on_cptbuild(scene, cs(["mode=make", "alpha=120", "outfile=x.cpt"])) == 0
	@test IG._on_cptbuild(scene, cs(["mode=make", "tmin=0", "outfile=x.cpt"])) == 0
	# grd mode builds FROM the data, so it needs the window's grid.
	@test IG._on_cptbuild(scene, cs(["mode=grd", "outfile=x.cpt"])) == 0
	@test !isfile("x.cpt")
end

# gravfft has four modes and each one has its own required set; every refusal below happens BEFORE
# any GMT call, so none of these run an FFT.
@testitem "gravfft: refuses an incomplete model" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[-2000 + 10ix + 5iy for iy in 0:9, ix in 0:9];
	                 x = collect(range(0, 9, length = 10)), y = collect(range(0, 9, length = 10)))
	scene = Ptr{Cvoid}(UInt(0x9D71FF71))
	IG._SCENE_OBJS[scene] = Tuple{Symbol,String,Any}[(:grid, "bat", G)]
	call(kv) = IG._on_gravfft(scene, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n"))))
	plate = ["te=7000", "rhol=2700", "rhom=3300", "rhow=1035"]
	try
		# The geopotential of a surface needs the density contrast (-D).
		@test call(["mode=surface", "field=f", "grid=bat"]) == 0
		# The Parker expansion is limited to 10 terms.
		@test call(["mode=surface", "field=f", "density=1665", "terms=11", "grid=bat"]) == 0
		# An elastic plate needs Te AND the three densities, and its Moho effect the Moho depth.
		@test call(["mode=flexure", "field=f", "te=7000", "rhom=3300", "zm=9000", "grid=bat"]) == 0
		@test call(vcat(["mode=flexure", "field=f", "moho=1", "grid=bat"], plate)) == 0
		# "Loading from below" (here the subplate load) also needs the load depth.
		@test call(vcat(["mode=flexure", "field=f", "subplate=1", "zm=9000", "grid=bat"], plate)) == 0
		# The flexural topography and the subplate load are two different results, not a combination.
		@test call(vcat(["mode=flexure", "field=f", "flextopo=1", "subplate=1", "zm=9000",
		                 "zl=40000", "grid=bat"], plate)) == 0
		# The admittance needs a second grid, and only one theoretical model.
		@test call(["mode=admitt", "field=f", "iflags=w", "grid2=no_such_grid.nc", "grid=bat"]) == 0
		@test call(vcat(["mode=admitt", "field=f", "iflags=tb", "grid2=" * @__FILE__, "zm=9000",
		                 "grid=bat"], plate)) == 0
		# A theoretical admittance exists only for the free-air anomaly and the geoid.
		@test call(vcat(["mode=admitt", "field=v", "iflags=t", "grid2=" * @__FILE__, "zm=9000",
		                 "grid=bat"], plate)) == 0
		# The curve-only mode needs its three numbers, a model, and again FAA or geoid.
		@test call(vcat(["mode=theo", "field=f", "clambda=5000", "cdepth=3000", "cmodel=b",
		                 "zm=9000", "zl=40000"], plate)) == 0
		@test call(vcat(["mode=theo", "field=f", "cn=400", "clambda=5000", "cdepth=3000", "cmodel=",
		                 "zm=9000"], plate)) == 0
		@test call(vcat(["mode=theo", "field=g", "cn=400", "clambda=5000", "cdepth=3000", "cmodel=b",
		                 "zm=9000"], plate)) == 0          # "from below" without the load depth
		@test call(["mode=whatever", "field=f", "grid=bat"]) == 0
		# No grid in the window at all (and this one is not the curve-only mode).
		@test IG._on_gravfft(Ptr{Cvoid}(UInt(0xDEADFF71)),
		                     Base.unsafe_convert(Cstring, Base.cconvert(Cstring,
		                         "mode=surface\nfield=f\ndensity=1665"))) == 0
	finally
		delete!(IG._SCENE_OBJS, scene)
	end
end

# grdrotater takes its rotation in three quite different ways, and getting -E wrong is the mistake
# that silently reconstructs the wrong thing. All of this is checked before the module runs.
@testitem "grdrotater: the -E string it builds" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test IG._grot_E(Dict("emode" => "pole", "elon" => "-40.8", "elat" => "32.8",
	                      "eangle" => "-12.9")) == "-40.8/32.8/-12.9"
	@test IG._grot_E(Dict("emode" => "pole", "elon" => "0", "elat" => "0", "eangle" => "10",
	                      "invert" => "1")) == "0/0/10+i"
	@test IG._grot_E(Dict("emode" => "file", "efile" => @__FILE__)) == @__FILE__
	@test IG._grot_E(Dict("emode" => "plates", "eplates" => "PAC-MBL")) == "PAC-MBL"
	@test_throws ErrorException IG._grot_E(Dict("emode" => "pole", "elon" => "0", "elat" => "0"))
	@test_throws ErrorException IG._grot_E(Dict("emode" => "pole", "elon" => "west", "elat" => "0",
	                                            "eangle" => "10"))
	@test_throws ErrorException IG._grot_E(Dict("emode" => "file", "efile" => "no_such.rot"))
	@test_throws ErrorException IG._grot_E(Dict("emode" => "plates", "eplates" => "PAC"))
	@test_throws ErrorException IG._grot_E(Dict("emode" => "somehow"))
	# What came back is picked by KIND: a run that asked for the outline gets a tuple.
	G = IG.GMT.mat2grid(fill(1.0f0, 4, 4); x = collect(0.0:3), y = collect(0.0:3))
	D = IG.GMT.mat2ds([0.0 0.0; 1.0 1.0; 0.0 0.0])
	@test IG._grot_pick((G, D), IG.GMTgrid) === G
	@test IG._grot_pick((G, D), IG.GMTdataset) === D
	@test IG._grot_pick(G, IG.GMTdataset) === nothing
	@test IG._grot_pick(nothing, IG.GMTgrid) === nothing
end

@testitem "grdrotater: refuses an unusable request" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9];
	                 x = collect(range(-5, 5, length = 10)), y = collect(range(-5, 5, length = 10)))
	scene = Ptr{Cvoid}(UInt(0x9D710807))
	IG._SCENE_OBJS[scene] = Tuple{Symbol,String,Any}[(:grid, "topo", G)]
	call(kv) = IG._on_grdrotater(scene, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n"))))
	pole = ["emode=pole", "elon=-40.8", "elat=32.8", "eangle=-12.9"]
	try
		@test call(["emode=pole", "elon=-40.8", "grid=topo"]) == 0            # half a pole
		@test call(["emode=file", "efile=no_such.rot", "grid=topo"]) == 0
		@test call(["emode=plates", "eplates=PAC", "grid=topo"]) == 0         # not a pair
		@test call(vcat(pole, ["polyfile=no_such_polygon.dat", "grid=topo"])) == 0
		# One time per run: a range would make the module write one file per reconstruction time.
		@test call(vcat(pole, ["time=0/50/10", "grid=topo"])) == 0
		# No grid in the window at all.
		@test IG._on_grdrotater(Ptr{Cvoid}(UInt(0xDEAD0807)),
		                        Base.unsafe_convert(Cstring, Base.cconvert(Cstring,
		                            "emode=pole\nelon=0\nelat=0\neangle=10"))) == 0
	finally
		delete!(IG._SCENE_OBJS, scene)
	end
end

# The two talwani dialogs build a GMT command STRING (GMT.jl wraps neither module), so what is
# tested here is that string — and the model file it refuses to hand over.
@testitem "talwani: the option pieces both dialogs share" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# A path travels inside the command string, so it is quoted — that is what survives a space.
	@test IG._gmt_quote_path("/a/b c.txt") == "\"/a/b c.txt\""
	@test IG._tal_F(Dict("field" => "f")) == "f"
	@test IG._tal_F(Dict("field" => "v")) == "v"
	@test IG._tal_F(Dict("field" => "n")) == "n"                    # module's own default latitude
	@test IG._tal_F(Dict("field" => "n", "lat" => "60")) == "n60"
	@test IG._tal_F(Dict("field" => "f", "lat" => "60")) == "f"     # a latitude only means the geoid
	@test_throws ErrorException IG._tal_F(Dict("field" => "g"))
	@test_throws ErrorException IG._tal_F(Dict("field" => "n", "lat" => "95"))
	@test_throws ErrorException IG._tal_F(Dict("field" => "n", "lat" => "north"))
	@test IG._tal_M(Dict("hkm" => "1")) == "h"
	@test IG._tal_M(Dict("vkm" => "1")) == "v"
	@test IG._tal_M(Dict("hkm" => "1", "vkm" => "1")) == "hv"
	@test IG._tal_M(Dict{String,String}()) == ""
	@test IG._tal_D(Dict("density" => "1700")) == "1700"
	@test IG._tal_D(Dict("density" => "1.7")) == "1.7"              # GMT reads |rho|<10 as g/cm^3
	@test IG._tal_D(Dict{String,String}()) == ""
	@test_throws ErrorException IG._tal_D(Dict("density" => "heavy"))
end

@testitem "talwani: the model file it will accept" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	mk(txt) = (f = tempname() * ".txt"; write(f, txt); f)
	# 2-D: one body, its density in the segment header.
	good2 = mk("# a comment\n> 1700\n0 0\n1 0\n1 -1\n0 -1\n")
	# No density anywhere in the headers.
	bare2 = mk("> body one\n0 0\n1 0\n1 -1\n")
	# No segment header at all.
	nohdr = mk("0 0\n1 0\n1 -1\n")
	# 3-D: "depth density" per slice.
	good3 = mk("> -5 1700\n0 0\n1 0\n1 1\n> -8 1700\n0 0\n1 0\n1 1\n")
	# A depth but no density.
	depth3 = mk("> -5\n0 0\n1 0\n1 1\n")
	try
		@test IG._tal_scan_model(good2) == (1, 1, true)
		@test IG._tal_scan_model(good3) == (2, 2, true)
		@test IG._tal_scan_model(bare2) == (1, 0, true)
		@test IG._tal_scan_model(nohdr) == (0, 0, true)

		@test IG._tal2d_check_model(good2, false) === nothing
		@test IG._tal2d_check_model(bare2, true) === nothing        # -D answers for every body
		@test_throws ErrorException IG._tal2d_check_model(bare2, false)
		@test_throws ErrorException IG._tal2d_check_model(nohdr, false)
		@test IG._tal2d_check_model(nohdr, true) === nothing
		@test_throws ErrorException IG._tal2d_check_model("no_such_model.txt", true)

		@test IG._tal3d_check_model(good3, false) === nothing
		@test IG._tal3d_check_model(depth3, true) === nothing       # the depth is there, -D gives rho
		# The DEPTH always comes from the header, so -D cannot rescue a file without one.
		@test_throws ErrorException IG._tal3d_check_model(depth3, false)
		@test_throws ErrorException IG._tal3d_check_model(nohdr, true)
		@test_throws ErrorException IG._tal3d_check_model(bare2, true)
	finally
		for f in (good2, bare2, nohdr, good3, depth3);  rm(f; force = true);  end
	end
end

# A 2-column -N track file with no constant level does not make GMT complain: it CRASHES the process
# inside gmtlib_read_table (talwani3d and gravprisms both). This check is what stands in front of it.
@testitem "talwani/gravprisms: the -N track file both 3-D modules will accept" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	mk(txt) = (f = tempname() * ".txt"; write(f, txt); f)
	xyz = mk("# a comment\n0 0 0\n1000 0 -50\n")
	xy  = mk("0 0\n1000 0\n")
	none = mk("# nothing but comments\n")
	try
		@test IG._tal_check_track(xyz, false) === nothing
		@test IG._tal_check_track(xy, true) === nothing        # -Z states the level: 2 columns is fine
		@test_throws ErrorException IG._tal_check_track(xy, false)
		@test_throws ErrorException IG._tal_check_track(none, false)
		@test_throws ErrorException IG._tal_check_track("no_such_track.txt", true)
	finally
		for f in (xyz, xy, none);  rm(f; force = true);  end
	end
end

@testitem "talwani2d: the -T and -Z it builds" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test IG._tal2d_T(Dict("tmin" => "-200", "tmax" => "200", "tinc" => "2")) == "-200/200/2"
	@test IG._tal2d_T(Dict("tmin" => "-200", "tmax" => "200", "tinc" => "101",
	                       "tnum" => "1")) == "-200/200/101+n"
	@test_throws ErrorException IG._tal2d_T(Dict("tmin" => "-200", "tmax" => "200"))
	@test_throws ErrorException IG._tal2d_T(Dict("tmin" => "200", "tmax" => "-200", "tinc" => "2"))
	@test_throws ErrorException IG._tal2d_T(Dict("tmin" => "-200", "tmax" => "200", "tinc" => "0"))
	@test_throws ErrorException IG._tal2d_T(Dict("tmin" => "-200", "tmax" => "200", "tinc" => "1",
	                                             "tnum" => "1"))
	@test_throws ErrorException IG._tal2d_T(Dict("tmin" => "left", "tmax" => "200", "tinc" => "2"))

	@test IG._tal2d_Z(Dict{String,String}(), "f") == ""
	@test IG._tal2d_Z(Dict("level" => "-2"), "f") == "-2"
	@test IG._tal2d_Z(Dict("level" => "0", "y25min" => "-50", "y25max" => "50"), "f") == "0/-50/50"
	# -Z with no level but a strike extent still needs the level slot filled.
	@test IG._tal2d_Z(Dict("y25min" => "-50", "y25max" => "50"), "f") == "0/-50/50"
	# A finite extent along strike is the 2.5-D correction: free-air only, and both ends or neither.
	@test_throws ErrorException IG._tal2d_Z(Dict("y25min" => "-50", "y25max" => "50"), "v")
	@test_throws ErrorException IG._tal2d_Z(Dict("y25min" => "-50"), "f")
	@test_throws ErrorException IG._tal2d_Z(Dict("y25min" => "50", "y25max" => "-50"), "f")
	@test_throws ErrorException IG._tal2d_Z(Dict("level" => "deep"), "f")

	@test IG._tal2d_colnames("f", 2) == ["x", "Free-air anomaly (mGal)"]
	@test IG._tal2d_colnames("v", 2)[2] == "Vertical gravity gradient (Eotvos)"
	@test IG._tal2d_colnames("n", 3) == ["x", "Geoid (m)", "column 3"]
	@test IG._tal3d_colnames("f", 4) == ["x", "y", "Observation level", "Free-air anomaly (mGal)"]
end

@testitem "talwani2d: refuses an unusable request" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	scene = Ptr{Cvoid}(UInt(0x9D712D00))
	model = tempname() * ".txt";  write(model, "> 1700\n0 0\n1 0\n1 -1\n0 -1\n")
	call(kv) = IG._on_talwani2d(scene, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n"))))
	base = ["infile=" * model, "field=f"]
	try
		@test call(["field=f", "tmin=-1", "tmax=1", "tinc=0.1"]) == 0          # no model at all
		@test call(["infile=no_such_model.txt", "tmin=-1", "tmax=1", "tinc=0.1"]) == 0
		@test call(vcat(base, ["tmin=-1", "tmax=1"])) == 0                     # half a profile
		@test call(vcat(base, ["mode=track"])) == 0                            # no locations file
		@test call(vcat(base, ["mode=track", "trackfile=no_such_track.txt"])) == 0
		@test call(vcat(base, ["mode=sideways", "tmin=-1", "tmax=1", "tinc=0.1"])) == 0
		@test call(vcat(base, ["tmin=-1", "tmax=1", "tinc=0.1", "density=lead"])) == 0
		# A 2.5-D strike extent asked for alongside a VGG: the module has no such thing.
		@test call(["infile=" * model, "field=v", "tmin=-1", "tmax=1", "tinc=0.1",
		            "y25min=-5", "y25max=5"]) == 0
	finally
		rm(model; force = true)
	end
end

@testitem "talwani3d: refuses an unusable request" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	scene = Ptr{Cvoid}(UInt(0x9D713D00))
	model = tempname() * ".txt";  write(model, "> -5 1700\n0 0\n1 0\n1 1\n0 1\n")
	call(kv) = IG._on_talwani3d(scene, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n"))))
	base = ["infile=" * model, "field=f"]
	try
		@test call(["field=f", "mode=grid", "region=0/1/0/1", "inc=0.1"]) == 0   # no model at all
		@test call(["infile=no_such_model.txt", "mode=grid", "region=0/1/0/1", "inc=0.1"]) == 0
		@test call(vcat(base, ["mode=grid", "inc=0.1"])) == 0                    # no region
		@test call(vcat(base, ["mode=grid", "region=0/1//", "inc=0.1"])) == 0    # half a region
		@test call(vcat(base, ["mode=grid", "region=0/1/0/1"])) == 0             # no increment
		@test call(vcat(base, ["mode=track"])) == 0                              # no locations file
		@test call(vcat(base, ["mode=track", "trackfile=no_such_track.txt"])) == 0
		# A 2-column track with no constant level is the one that CRASHES GMT, so it must be refused
		# here; with a level it is legal, and this callback then goes on to run the module.
		xy = tempname() * ".txt";  write(xy, "0 0\n1 0\n")
		try
			@test call(vcat(base, ["mode=track", "trackfile=" * xy])) == 0
		finally
			rm(xy; force = true)
		end
		@test call(vcat(base, ["mode=obsgrid"])) == 0                            # no levels grid
		@test call(vcat(base, ["mode=obsgrid", "zgrid=no_such_levels.grd"])) == 0
		@test call(vcat(base, ["mode=elsewhere"])) == 0
		@test call(vcat(base, ["mode=grid", "region=0/1/0/1", "inc=0.1", "level=high"])) == 0
	finally
		rm(model; force = true)
	end
end

@testitem "greenspline: the option strings it builds" tags=[:unit, :fast] begin
	IG = InteractiveGMT

	# -D decides the DIMENSION, and nearly every other rule keys off that: 0 is a line, 4 is a
	# volume, everything else is a surface.
	@test IG._gs_dim(0) == 1
	@test all(IG._gs_dim(m) == 2 for m in (1, 2, 3, 5))
	@test IG._gs_dim(4) == 3
	@test IG._gs_mode(Dict("dmode" => "3")) == 3
	@test_throws Exception IG._gs_mode(Dict{String,String}())
	@test_throws Exception IG._gs_mode(Dict("dmode" => "6"))
	@test_throws Exception IG._gs_mode(Dict("dmode" => "flat"))

	# -S. The three splines in tension take one; the spherical pair only exists at -D5, and -D5
	# takes only them.
	@test IG._gs_S(Dict("spline" => "c"), 1) == "c"
	@test IG._gs_S(Dict("spline" => "l"), 4) == "l"
	@test IG._gs_S(Dict("spline" => "t", "tension" => "0.5"), 1) == "t0.5"
	@test IG._gs_S(Dict("spline" => "r", "tension" => "0.25/10"), 2) == "r0.25/10"
	@test IG._gs_S(Dict("spline" => "q", "tension" => "0.9"), 5) == "q0.9"
	@test IG._gs_S(Dict("spline" => "p"), 5) == "p"
	@test_throws Exception IG._gs_S(Dict("spline" => "p"), 3)            # spherical spline, flat mode
	@test_throws Exception IG._gs_S(Dict("spline" => "c"), 5)            # spherical mode, flat spline
	@test_throws Exception IG._gs_S(Dict("spline" => "t"), 1)            # no tension given
	@test_throws Exception IG._gs_S(Dict("spline" => "t", "tension" => "1"), 1)
	@test_throws Exception IG._gs_S(Dict("spline" => "t", "tension" => "-0.1"), 1)
	@test_throws Exception IG._gs_S(Dict("spline" => "t", "tension" => "stiff"), 1)
	@test_throws Exception IG._gs_S(Dict("spline" => "z"), 1)
	@test_throws Exception IG._gs_S(Dict{String,String}(), 1)

	# -C, in its three ways of saying how many eigenvalues survive.
	@test IG._gs_C(Dict{String,String}()) === nothing                    # not asked for
	@test IG._gs_C(Dict("approx" => "1", "ckind" => "r", "cvalue" => "0.05")) == "0.05"
	@test IG._gs_C(Dict("approx" => "1", "ckind" => "n", "cvalue" => "12")) == "n12"
	@test IG._gs_C(Dict("approx" => "1", "ckind" => "v", "cvalue" => "0.9")) == "v0.9"
	@test IG._gs_C(Dict("approx" => "1", "ckind" => "r", "cvalue" => "0.05",
	                    "cfile" => "eig.txt")) == "0.05+f\"eig.txt\""
	@test_throws Exception IG._gs_C(Dict("approx" => "1", "ckind" => "r"))
	@test_throws Exception IG._gs_C(Dict("approx" => "1", "ckind" => "r", "cvalue" => "2"))
	@test_throws Exception IG._gs_C(Dict("approx" => "1", "ckind" => "n", "cvalue" => "0"))
	@test_throws Exception IG._gs_C(Dict("approx" => "1", "ckind" => "n", "cvalue" => "1.5"))
	@test_throws Exception IG._gs_C(Dict("approx" => "1", "ckind" => "x", "cvalue" => "1"))

	# -L: the two halves of "remove the trend, then put it back".
	@test IG._gs_L(Dict{String,String}()) === nothing
	@test IG._gs_L(Dict("notrend" => "1")) == "t"
	@test IG._gs_L(Dict("norestore" => "1")) == "r"
	@test IG._gs_L(Dict("notrend" => "1", "norestore" => "1")) == "tr"

	# -E: a bare tick reports to GMT's message stream, a file gets the table.
	@test IG._gs_E(Dict{String,String}()) === nothing
	@test IG._gs_E(Dict("misfit" => "1")) === true
	@test IG._gs_E(Dict("misfit" => "1", "misfitfile" => "m.txt")) == "\"m.txt\""
	@test IG._gs_E(Dict("misfit" => "1", "reportfile" => "r.txt")) == "+r\"r.txt\""

	# -Q: one azimuth on a surface, three direction cosines in a volume — and never the other way.
	@test IG._gs_Q(Dict{String,String}(), 2) === nothing
	@test IG._gs_Q(Dict("deriv" => "1", "derivdir" => "45"), 2) == "45"
	@test IG._gs_Q(Dict("deriv" => "1", "derivdir" => "1/0/0"), 3) == "1/0/0"
	@test_throws Exception IG._gs_Q(Dict("deriv" => "1"), 2)
	@test_throws Exception IG._gs_Q(Dict("deriv" => "1", "derivdir" => "1/0/0"), 2)
	@test_throws Exception IG._gs_Q(Dict("deriv" => "1", "derivdir" => "45"), 3)
	@test_throws Exception IG._gs_Q(Dict("deriv" => "1", "derivdir" => "north"), 2)

	# -A: the record layout travels as the number the manual lists, not as words that could drift.
	@test IG._gs_A(Dict{String,String}()) === nothing
	grad = tempname() * ".txt";  write(grad, "0 0 0 90 1\n")
	try
		@test IG._gs_A(Dict("gradfile" => grad, "gradformat" => "2")) == "\"" * grad * "\"+f2"
		@test IG._gs_A(Dict("gradfile" => grad)) == "\"" * grad * "\"+f1"
		@test_throws Exception IG._gs_A(Dict("gradfile" => grad, "gradformat" => "9"))
		@test_throws Exception IG._gs_A(Dict("gradfile" => grad, "gradformat" => "x"))
		@test_throws Exception IG._gs_A(Dict("gradfile" => "no_such_gradients.txt"))
	finally
		rm(grad; force = true)
	end

	@test IG._gs_colnames(2, 3) == ["x", "y", "value"]
	@test IG._gs_colnames(1, 2) == ["x", "value"]
	@test IG._gs_colnames(3, 4) == ["x", "y", "z", "value"]
end

@testitem "greenspline: refuses an unusable request" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	tbl = tempname() * ".txt";  write(tbl, "0 0 1\n1 0 2\n0 1 3\n1 1 4\n")
	scene = Ptr{Cvoid}(UInt(0x9D71A500))
	call(kv) = IG._on_greenspline(scene, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n"))))
	# Every case below is refused BEFORE the table is read, so no GMT module ever runs here.
	base = ["infile=" * tbl, "dmode=1", "spline=t", "tension=0.5", "what=grid"]
	geom = ["region=0/1/0/1", "inc=0.1"]
	try
		@test call(["dmode=1", "spline=c"]) == 0                            # no table
		@test call(["infile=no_such_table.txt", "dmode=1", "spline=c"]) == 0
		@test call(["infile=" * tbl, "spline=c"]) == 0                      # no distance mode
		@test call(["infile=" * tbl, "dmode=1"]) == 0                       # no spline
		@test call(["infile=" * tbl, "dmode=5", "spline=c"]) == 0           # flat spline, spherical mode
		@test call(["infile=" * tbl, "dmode=1", "spline=q", "tension=0.5"]) == 0
		@test call(["infile=" * tbl, "dmode=1", "spline=t"]) == 0           # tension missing
		# A grid is a surface: the 1-D and 3-D modes have none to give.
		@test call(vcat(["infile=" * tbl, "dmode=0", "spline=c", "what=grid"], geom)) == 0
		@test call(vcat(["infile=" * tbl, "dmode=4", "spline=c", "what=grid"], geom)) == 0
		# Grid mode with nowhere to put the grid.
		@test call(base) == 0                                               # no region, no mask
		@test call(vcat(base, ["region=0/1/0/1"])) == 0                     # no spacing
		@test call(vcat(base, ["maskgrid=no_such_mask.nc"])) == 0
		# Node mode with no locations.
		@test call(["infile=" * tbl, "dmode=1", "spline=c", "what=nodes"]) == 0
		@test call(["infile=" * tbl, "dmode=1", "spline=c", "what=nodes", "nodefile=no_such_pts.txt"]) == 0
		@test call(vcat(base, geom, ["what=sideways"])) == 0
		# The option builders, reached through the callback.
		@test call(vcat(base, geom, ["approx=1", "ckind=r", "cvalue=7"])) == 0
		@test call(vcat(base, geom, ["gradfile=no_such_gradients.txt"])) == 0
		@test call(vcat(base, geom, ["deriv=1", "derivdir=1/0/0"])) == 0
		@test call(vcat(base, geom, ["deriv=1"])) == 0
	finally
		rm(tbl; force = true)
	end
end

# The two flexure dialogs build GMT command strings (GMT.jl wraps neither module), so what is tested
# is those strings and the module's own rules about which options may travel together.
@testitem "gmtflexure: the option strings it builds" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	me = @__FILE__
	@test IG._flex_num_k("10") && IG._flex_num_k("10k") && IG._flex_num_k("1e10")
	@test !IG._flex_num_k("") && !IG._flex_num_k("k") && !IG._flex_num_k("thick")

	rho = Dict("rhom" => "3300", "rhol" => "2700", "rhow" => "1035")
	@test IG._gf2_D(rho) == "3300/2700/1035"
	@test IG._gf2_D(merge(rho, Dict("rhoi" => "2400"))) == "3300/2700/2400/1035"
	@test_throws ErrorException IG._gf2_D(Dict("rhom" => "3300", "rhol" => "2700"))
	@test_throws ErrorException IG._gf2_D(merge(rho, Dict("rhom" => "heavy")))
	@test_throws ErrorException IG._gf2_D(merge(rho, Dict("rhoi" => "soft")))

	@test IG._gf2_E(Dict("te" => "10k")) == ("10k", false)
	@test IG._gf2_E(Dict("te" => me)) == ("\"" * me * "\"", true)
	@test_throws ErrorException IG._gf2_E(Dict{String,String}())
	@test_throws ErrorException IG._gf2_E(Dict("te" => "no_such_rigidity.txt"))

	@test IG._gf2_Q(Dict("qmode" => "t", "loadfile" => me), false) == "t\"" * me * "\""
	@test IG._gf2_Q(Dict("qmode" => "q", "loadfile" => me), false) == "q\"" * me * "\""
	# -Qn with a variable-rigidity file already knows where the nodes are; without one it does not.
	@test IG._gf2_Q(Dict("qmode" => "n"), true) == "n"
	@test_throws ErrorException IG._gf2_Q(Dict("qmode" => "n"), false)
	@test IG._gf2_Q(Dict("qmode" => "n", "qmin" => "-200", "qmax" => "200", "qinc" => "2"), false) ==
	      "n-200/200/2"
	@test_throws ErrorException IG._gf2_Q(Dict("qmode" => "n", "qmin" => "-200", "qmax" => "200"), false)
	@test_throws ErrorException IG._gf2_Q(Dict("qmode" => "n", "qmin" => "200", "qmax" => "-200",
	                                           "qinc" => "2"), false)
	@test_throws ErrorException IG._gf2_Q(Dict("qmode" => "t", "loadfile" => "no_such_load.txt"), false)
	@test_throws ErrorException IG._gf2_Q(Dict("qmode" => "sideways"), false)

	@test IG._gf2_A(Dict{String,String}(), "l") == ""              # not asked for
	@test IG._gf2_A(Dict("lbc" => "0"), "l") == "l0"
	@test IG._gf2_A(Dict("rbc" => "2", "rargs" => "0"), "r") == "r2/0"
	@test IG._gf2_A(Dict("rbc" => "3", "rargs" => "0/0"), "r") == "r3/0/0"
	@test_throws ErrorException IG._gf2_A(Dict("lbc" => "4"), "l")
	@test_throws ErrorException IG._gf2_A(Dict("lbc" => "1", "largs" => "5"), "l")   # takes no value
	@test_throws ErrorException IG._gf2_A(Dict("lbc" => "3", "largs" => "5"), "l")   # needs moment/force

	@test IG._gf2_colnames(false, 2) == ["x", "Deflection (m)"]
	@test IG._gf2_colnames(true, 3) == ["x", "Deflection (km)", "Curvature"]
end

@testitem "gmtflexure: refuses an unusable request" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	scene = Ptr{Cvoid}(UInt(0x9D71F200))
	load = tempname() * ".txt";  write(load, "-100 0\n0 2000\n100 0\n")
	call(kv) = IG._on_gmtflexure(scene, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n"))))
	base = ["te=10k", "rhom=3300", "rhol=2700", "rhow=1035", "qmode=t", "loadfile=" * load]
	try
		@test call(["rhom=3300", "rhol=2700", "rhow=1035", "qmode=t", "loadfile=" * load]) == 0  # no -E
		@test call(["te=10k", "rhom=3300", "qmode=t", "loadfile=" * load]) == 0                 # half -D
		@test call(["te=10k", "rhom=3300", "rhol=2700", "rhow=1035", "qmode=t"]) == 0           # no load
		@test call(["te=10k", "rhom=3300", "rhol=2700", "rhow=1035", "qmode=t",
		            "loadfile=no_such_load.txt"]) == 0
		@test call(["te=10k", "rhom=3300", "rhol=2700", "rhow=1035", "qmode=n"]) == 0           # nowhere to compute
		@test call(vcat(base, ["lbc=3", "largs=5"])) == 0                                       # moment without force
		@test call(vcat(base, ["poisson=squishy"])) == 0
		@test call(vcat(base, ["force=hard"])) == 0
		@test call(vcat(base, ["water=-500"])) == 0                                             # must be positive
		@test call(vcat(base, ["zobs=deep"])) == 0
		@test call(vcat(base, ["wfile=no_such_deformation.txt"])) == 0
	finally
		rm(load; force = true)
	end
end

@testitem "grdflexure: the option strings it builds" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test IG._flex_time("5M") && IG._flex_time("100k") && IG._flex_time("1000")
	@test !IG._flex_time("soon") && !IG._flex_time("")

	rho = Dict("rhom" => "3300", "rhol" => "2700", "rhow" => "1035")
	@test IG._gflx_D(rho, false) == "3300/2700/1035"
	@test IG._gflx_D(merge(rho, Dict("rhoi" => "2400", "rhoroot" => "2800")), false) ==
	      "3300/2700/2400/1035+r2800"
	# With a load-density grid the module wants a dash where the fixed density would be.
	@test IG._gflx_D(rho, true) == "3300/-/1035"
	@test_throws ErrorException IG._gflx_D(Dict("rhom" => "3300", "rhow" => "1035"), false)
	@test_throws ErrorException IG._gflx_D(merge(rho, Dict("rhoroot" => "dense")), false)

	@test IG._gflx_E(Dict("te" => "10k")) == ("10k", false)
	@test IG._gflx_E(Dict("te" => "10k", "te2" => "5k")) == ("10k/5k", true)
	@test IG._gflx_E(Dict{String,String}()) == ("", false)          # no plate: purely viscous
	@test_throws ErrorException IG._gflx_E(Dict("te2" => "5k"))
	@test_throws ErrorException IG._gflx_E(Dict("te" => "thick"))

	@test IG._gflx_F(Dict("nua" => "1e19")) == "1e19"
	@test IG._gflx_F(Dict("nua" => "1e19", "ha" => "100k", "num" => "1e21")) == "1e19/100k/1e21"
	@test IG._gflx_F(Dict{String,String}()) == ""
	@test_throws ErrorException IG._gflx_F(Dict("nua" => "1e19", "ha" => "100k"))
	@test_throws ErrorException IG._gflx_F(Dict("nua" => "thick"))

	@test IG._gflx_A(Dict{String,String}()) == ""
	@test IG._gflx_A(Dict("nx" => "-1e12", "ny" => "0", "nxy" => "0")) == "-1e12/0/0"
	@test_throws ErrorException IG._gflx_A(Dict("nx" => "-1e12"))

	@test IG._gflx_T(Dict{String,String}()) == ""
	@test IG._gflx_T(Dict("t0" => "5M")) == "5M"
	@test IG._gflx_T(Dict("t0" => "0", "t1" => "5M", "dt" => "1M")) == "0/5M/1M"
	@test IG._gflx_T(Dict("t0" => "1k", "t1" => "5M", "dt" => "20", "tlog" => "1")) == "1k/5M/20+l"
	@test_throws ErrorException IG._gflx_T(Dict("t0" => "0", "t1" => "5M"))
	@test_throws ErrorException IG._gflx_T(Dict("t0" => "yesterday"))
	@test_throws ErrorException IG._gflx_T(Dict("tfile" => "no_such_times.txt"))

	@test IG._gflx_transfer_name(10) == "grdflexure_transfer_function_te_010_km.txt"
	@test IG._gflx_transfer_name(0) == "grdflexure_transfer_function_te_000_km.txt"

	# The -L list: "time file timetag", and a file name with a space in it still comes back whole.
	lis = tempname() * ".lis"
	write(lis, "# a comment\n0\tflex_000k.nc\t0\n1000000 flex 1M.nc 1M\n")
	try
		got = IG._gflx_read_list(lis)
		@test length(got) == 2
		@test got[1] == (0.0, "flex_000k.nc", "0")
		@test got[2] == (1.0e6, "flex 1M.nc", "1M")
	finally
		rm(lis; force = true)
	end
end

@testitem "grdflexure: refuses an unusable request" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	scene = Ptr{Cvoid}(UInt(0x9D71F300))
	grid = tempname() * ".grd";  write(grid, "not a grid, but it EXISTS")
	call(kv) = IG._on_grdflexure(scene, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n"))))
	base = ["loadgrid=" * grid, "rhom=3300", "rhol=2700", "rhow=1035", "te=10k"]
	try
		@test call(["rhom=3300", "rhol=2700", "rhow=1035", "te=10k"]) == 0            # no load grid
		@test call(["loadgrid=no_such_load.nc", "rhom=3300", "rhol=2700", "rhow=1035", "te=10k"]) == 0
		@test call(["loadgrid=" * grid, "rhom=3300", "rhow=1035", "te=10k"]) == 0     # no load density
		@test call(["loadgrid=" * grid, "rhom=3300", "rhol=2700", "rhow=1035"]) == 0  # no plate, no viscosity
		# The two time-dependent rheologies are alternatives, and both need times.
		@test call(vcat(base, ["nua=1e19", "maxwell=1M"])) == 0
		@test call(vcat(base, ["nua=1e19"])) == 0
		@test call(vcat(base, ["maxwell=1M"])) == 0
		@test call(vcat(base, ["nua=1e19", "nx=-1e12", "ny=0", "nxy=0", "t0=1M"])) == 0
		# A general linear model (two thicknesses) needs a Maxwell time.
		@test call(vcat(base, ["te2=5k", "t0=1M"])) == 0
		# With a time the output name has to be a template the module can fill in.
		@test call(vcat(base, ["t0=1M", "maxwell=1M"])) == 0
		@test call(vcat(base, ["t0=1M", "maxwell=1M", "outfile=flex.nc"])) == 0
		# Plain refusals.
		@test call(vcat(base, ["beta=2"])) == 0
		@test call(vcat(base, ["water=-1"])) == 0
		@test call(vcat(base, ["rhogrid=no_such_density.nc"])) == 0
		@test call(vcat(base, ["t0=yesterday"])) == 0
	finally
		rm(grid; force = true)
	end
end

# grdvolume's -C has five shapes and a malformed one is the mistake that matters: the module would
# happily report a plausible number for a question nobody asked. All of this is checked before it runs.
@testitem "grdvolume: the -C string it builds" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test IG._gvol_C(Dict("cmode" => "all")) == ""                 # the whole grid: no -C at all
	@test IG._gvol_C(Dict("cmode" => "above", "cval" => "0")) == "0"
	@test IG._gvol_C(Dict("cmode" => "below", "cval" => "-100")) == "r-100"
	@test IG._gvol_C(Dict("cmode" => "between", "clow" => "-4000", "chigh" => "-2000")) == "r-4000/-2000"
	@test IG._gvol_C(Dict("cmode" => "range", "clow" => "0", "chigh" => "500", "cdelta" => "50")) ==
	      "0/500/50"
	@test_throws ErrorException IG._gvol_C(Dict("cmode" => "above"))                     # no level
	@test_throws ErrorException IG._gvol_C(Dict("cmode" => "above", "cval" => "sea"))
	@test_throws ErrorException IG._gvol_C(Dict("cmode" => "between", "clow" => "10", "chigh" => "5"))
	@test_throws ErrorException IG._gvol_C(Dict("cmode" => "range", "clow" => "0", "chigh" => "5",
	                                            "cdelta" => "0"))
	@test_throws ErrorException IG._gvol_C(Dict("cmode" => "sideways"))
	# What the module reports; with -D the last column is the slice thickness, not a mean height.
	@test IG._gvol_colnames(false, 4) == ["Contour", "Area", "Volume", "Mean height"]
	@test IG._gvol_colnames(true, 4)[4] == "Slice thickness"
end

@testitem "grdvolume: refuses what the module cannot do" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[100 - (ix - 5)^2 - (iy - 5)^2 for iy in 0:9, ix in 0:9];
	                 x = collect(range(0, 9, length = 10)), y = collect(range(0, 9, length = 10)))
	scene = Ptr{Cvoid}(UInt(0x9D710101))
	IG._SCENE_OBJS[scene] = Tuple{Symbol,String,Any}[(:grid, "topo", G)]
	call(kv) = IG._on_grdvolume(scene, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n"))))
	try
		# Slices measure the gaps BETWEEN contours, so they need a range of them.
		@test call(["cmode=above", "cval=0", "slices=1", "grid=topo"]) == 0
		# The maximum-curvature rule has nothing to look at without contours.
		@test call(["cmode=all", "tmax=c", "grid=topo"]) == 0
		@test call(["cmode=all", "tmax=q", "grid=topo"]) == 0
		# A shift without a scale factor is half an option.
		@test call(["cmode=all", "zshift=10", "grid=topo"]) == 0
		@test call(["cmode=all", "base=floor", "grid=topo"]) == 0
		@test call(["cmode=all", "unit=parsec", "grid=topo"]) == 0
		# No grid in the window at all.
		@test IG._on_grdvolume(Ptr{Cvoid}(UInt(0xDEAD0101)),
		                       Base.unsafe_convert(Cstring, Base.cconvert(Cstring, "cmode=all"))) == 0
	finally
		delete!(IG._SCENE_OBJS, scene)
	end
end

# gravprisms has more ways to be asked for something impossible than any other dialog here — the
# module itself carries a dozen mutual-exclusion checks — so what is tested is that each of those
# comes back as a refusal, and that the -D, -H and -C strings it builds are the module's own.
@testitem "gravprisms: the -D, -H and -C strings it builds" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	me = @__FILE__
	@test IG._gpr_D(Dict("density" => "1700")) == ("1700", false)
	@test IG._gpr_D(Dict("density" => "1700", "contrast" => "1")) == ("1700+c", false)
	@test IG._gpr_D(Dict{String,String}()) == ("", false)
	# Anything that is not a number is a grid, and it has to be there.
	@test IG._gpr_D(Dict("density" => me)) == ("\"" * me * "\"", true)
	@test_throws ErrorException IG._gpr_D(Dict("density" => "no_such_density.grd"))

	base = Dict("radial" => "1", "href" => "6000", "rholo" => "2400", "rhohi" => "2700")
	@test IG._gpr_H(base) == "6000/2400/2700"
	@test IG._gpr_H(merge(base, Dict("densify" => "0.5", "power" => "2"))) == "6000/2400/2700+d0.5+p2"
	@test IG._gpr_H(merge(base, Dict("boost" => "1.2"))) == "6000/2400/2700+b1.2"
	@test IG._gpr_H(Dict{String,String}()) == ""                      # the model was not asked for
	@test_throws ErrorException IG._gpr_H(Dict("radial" => "1", "href" => "6000"))
	@test_throws ErrorException IG._gpr_H(merge(base, Dict("rholo" => "light")))
	@test_throws ErrorException IG._gpr_H(merge(base, Dict("power" => "steep")))

	@test IG._gpr_C(Dict{String,String}(), false) == ""                # plain -C
	@test IG._gpr_C(Dict("saveprisms" => "p.txt"), false) == "+w\"p.txt\""
	@test IG._gpr_C(Dict("saveprisms" => "p.txt", "quit" => "1"), false) == "+w\"p.txt\"+q"
	@test IG._gpr_C(Dict("dz" => "100"), true) == "+z100"
	# The module's own three rules about -C's modifiers.
	@test_throws ErrorException IG._gpr_C(Dict("dz" => "100"), false)   # +z without -H
	@test_throws ErrorException IG._gpr_C(Dict{String,String}(), true)  # -H without +z
	@test_throws ErrorException IG._gpr_C(Dict("dz" => "0"), true)      # +z must be positive
	@test_throws ErrorException IG._gpr_C(Dict("quit" => "1"), false)   # +q without +w

	@test IG._gpr_colnames("f", false, 4) == ["x", "y", "Observation level", "Free-air anomaly (mGal)"]
	@test IG._gpr_colnames("v", true, 4)[1:2] == ["lon", "lat"]
	@test IG._gpr_colnames("n", false, 5)[end] == "column 5"
end

@testitem "gravprisms: refuses an unusable request" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	scene = Ptr{Cvoid}(UInt(0x9D719000))
	table = tempname() * ".txt";  write(table, "0 0 0 1000 2000 2000 1700\n")
	grid  = tempname() * ".grd";  write(grid, "not really a grid, but it EXISTS")
	call(kv) = IG._on_gravprisms(scene, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n"))))
	where = ["mode=grid", "region=-1/1/-1/1", "inc=0.1"]
	rad = ["radial=1", "href=6000", "rholo=2400", "rhohi=2700"]
	try
		# --- the prisms themselves
		@test call(vcat(["source=table"], where)) == 0                      # no table
		@test call(vcat(["source=table", "infile=no_such_prisms.txt"], where)) == 0
		@test call(vcat(["source=table", "infile=" * table, "dxdy=wide"], where)) == 0
		@test call(vcat(["source=elsewhere", "infile=" * table], where)) == 0
		# --- creating them
		@test call(vcat(["source=create", "shape=" * grid], where)) == 0    # no density at all
		@test call(vcat(["source=create", "density=1700"], where)) == 0     # no surface to build from
		@test call(vcat(["source=create", "density=1700", "shape=no_such_heights.grd"], where)) == 0
		@test call(vcat(["source=create", "density=1700", "top=no_such_top.grd"], where)) == 0
		# The radial model needs the heights grid, and a sub-prism height.
		@test call(vcat(["source=create"], rad, ["top=0"], where)) == 0
		@test call(vcat(["source=create"], rad, ["shape=" * grid], where)) == 0   # no +z
		@test call(vcat(["source=create"], rad, ["shape=" * grid, "dz=0"], where)) == 0
		# ... and it will not take a density GRID, nor a fixed density that is not subtracted.
		@test call(vcat(["source=create"], rad,
		                ["shape=" * grid, "dz=100", "density=" * grid], where)) == 0
		@test call(vcat(["source=create"], rad,
		                ["shape=" * grid, "dz=100", "density=1700"], where)) == 0
		# +q needs a file to write the prisms to.
		@test call(vcat(["source=create", "density=1700", "shape=" * grid, "quit=1"], where)) == 0
		# --- the mean-density grid belongs to a created, radially-varying model
		@test call(vcat(["source=table", "infile=" * table, "avedens=w.grd"], where)) == 0
		@test call(vcat(["source=create", "density=1700", "shape=" * grid, "avedens=w.grd"], where)) == 0
		# --- where to compute
		@test call(vcat(["source=table", "infile=" * table, "mode=grid", "inc=0.1"])) == 0
		@test call(vcat(["source=table", "infile=" * table, "mode=grid", "region=0/1//", "inc=0.1"])) == 0
		@test call(vcat(["source=table", "infile=" * table, "mode=grid", "region=-1/1/-1/1"])) == 0
		@test call(vcat(["source=table", "infile=" * table, "mode=track"])) == 0
		@test call(vcat(["source=table", "infile=" * table, "mode=track", "trackfile=no_such.txt"])) == 0
		# Same GMT crash as talwani3d's: a 2-column track with no constant level, refused here.
		xy = tempname() * ".txt";  write(xy, "0 0\n1 0\n")
		try
			@test call(vcat(["source=table", "infile=" * table, "mode=track", "trackfile=" * xy])) == 0
		finally
			rm(xy; force = true)
		end
		@test call(vcat(["source=table", "infile=" * table, "mode=obsgrid"])) == 0
		@test call(vcat(["source=table", "infile=" * table, "mode=obsgrid", "zgrid=no_such.grd"])) == 0
		@test call(vcat(["source=table", "infile=" * table, "mode=nowhere"])) == 0
		@test call(vcat(["source=table", "infile=" * table], where, ["level=high"])) == 0
		# --- the field
		@test call(vcat(["source=table", "infile=" * table, "field=g"], where)) == 0
		@test call(vcat(["source=table", "infile=" * table, "field=n", "lat=95"], where)) == 0
		@test call(vcat(["source=table", "infile=" * table, "density=heavy"], where)) == 0
	finally
		rm(table; force = true);  rm(grid; force = true)
	end
end

# grdvector's arithmetic is all local (the module makes PostScript, so nothing about the arrows comes
# back from GMT), which means every piece below is checkable with no window and no DLL.
@testitem "grdvector: the pieces it computes itself" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT

	# -A / -Z. Without -A the pair already IS (u, v). With it, theta is counter-clockwise from +x;
	# with -Z on top it is an azimuth, so 0 points north and 90 points east.
	@test IG._gv_uv(3.0, 4.0, false, false) == (3.0, 4.0)
	u, v = IG._gv_uv(2.0, 0.0, true, false);   @test isapprox(u, 2.0; atol = 1e-12) && isapprox(v, 0.0; atol = 1e-12)
	u, v = IG._gv_uv(2.0, 90.0, true, false);  @test isapprox(u, 0.0; atol = 1e-12) && isapprox(v, 2.0; atol = 1e-12)
	u, v = IG._gv_uv(2.0, 0.0, true, true);    @test isapprox(u, 0.0; atol = 1e-12) && isapprox(v, 2.0; atol = 1e-12)
	u, v = IG._gv_uv(2.0, 90.0, true, true);   @test isapprox(u, 2.0; atol = 1e-12) && isapprox(v, 0.0; atol = 1e-12)

	# -I in its three shapes.
	@test IG._gv_step(Dict("incmode" => "auto"), 200, 100, 0.1, 0.1) == (10, 5)
	@test IG._gv_step(Dict("incmode" => "x", "incx" => "4"), 200, 100, 0.1, 0.1) == (4, 4)
	@test IG._gv_step(Dict("incmode" => "x", "incx" => "4", "incy" => "2"), 200, 100, 0.1, 0.1) == (4, 2)
	@test IG._gv_step(Dict("incmode" => "inc", "incx" => "0.5"), 200, 100, 0.1, 0.1) == (5, 5)
	@test_throws Exception IG._gv_step(Dict("incmode" => "x"), 200, 100, 0.1, 0.1)
	@test_throws Exception IG._gv_step(Dict("incmode" => "x", "incx" => "-1"), 200, 100, 0.1, 0.1)
	@test_throws Exception IG._gv_step(Dict("incmode" => "x", "incx" => "wide"), 200, 100, 0.1, 0.1)

	# -S. Automatic gives the longest arrow 0.9 of the spacing; "direct" is a length per unit of
	# magnitude and "inverse" is GMT's own bare -S, its reciprocal; "fixed" is -Sl.
	@test IG._gv_factor(Dict("scalemode" => "auto"), 2.0, 1.0) == (0.45, 0.0)
	@test IG._gv_factor(Dict("scalemode" => "direct", "scale" => "3"), 2.0, 1.0) == (3.0, 0.0)
	@test IG._gv_factor(Dict("scalemode" => "inverse", "scale" => "4"), 2.0, 1.0) == (0.25, 0.0)
	@test IG._gv_factor(Dict("scalemode" => "fixed", "scale" => "2"), 2.0, 1.0) == (0.0, 2.0)
	@test_throws Exception IG._gv_factor(Dict("scalemode" => "auto"), 0.0, 1.0)      # every vector is zero
	@test_throws Exception IG._gv_factor(Dict("scalemode" => "direct"), 2.0, 1.0)
	@test_throws Exception IG._gv_factor(Dict("scalemode" => "direct", "scale" => "0"), 2.0, 1.0)
	@test_throws Exception IG._gv_factor(Dict("scalemode" => "sideways", "scale" => "1"), 2.0, 1.0)

	# -Q.
	heads, len, beta, norm = IG._gv_head(Dict{String,String}())
	@test heads == "e" && len == 0.33 && isapprox(beta, tand(18)) && norm == 0.0
	@test IG._gv_head(Dict("heads" => ""))[1] == ""
	@test IG._gv_head(Dict("heads" => "be", "norm" => "0.5"))[4] == 0.5
	@test_throws Exception IG._gv_head(Dict("heads" => "x"))
	@test_throws Exception IG._gv_head(Dict("headlen" => "0"))
	@test_throws Exception IG._gv_head(Dict("headlen" => "2"))
	@test_throws Exception IG._gv_head(Dict("headang" => "0"))
	@test_throws Exception IG._gv_head(Dict("headang" => "90"))
	@test_throws Exception IG._gv_head(Dict("norm" => "-1"))

	# One arrow: the shaft, plus a 3-point polyline for each end that carries a head.
	segs = Matrix{Float64}[]
	IG._gv_arrow!(segs, 0.0, 0.0, 1.0, 0.0, "", 0.33, tand(18), 1.0)
	@test length(segs) == 1 && segs[1] == [0.0 0.0; 1.0 0.0]
	segs = Matrix{Float64}[]
	IG._gv_arrow!(segs, 0.0, 0.0, 1.0, 0.0, "e", 0.33, tand(18), 1.0)
	@test length(segs) == 2 && size(segs[2]) == (3, 2) && segs[2][2, :] == [1.0, 0.0]
	segs = Matrix{Float64}[]
	IG._gv_arrow!(segs, 0.0, 0.0, 1.0, 0.0, "be", 0.33, tand(18), 1.0)
	@test length(segs) == 3 && segs[3][2, :] == [0.0, 0.0]
	# A head shrunk to nothing (+n with a very short arrow) leaves the shaft alone.
	segs = Matrix{Float64}[]
	IG._gv_arrow!(segs, 0.0, 0.0, 1.0, 0.0, "e", 0.33, tand(18), 0.0)
	@test length(segs) == 1

	# The -C stand-in: equal-width classes on the app's own blue-to-red ramp.
	@test IG._gv_class_color(1, 7)[3] > IG._gv_class_color(1, 7)[1]
	@test IG._gv_class_color(7, 7)[1] > IG._gv_class_color(7, 7)[3]
	@test IG._gv_colnames(true)[6] == "theta"
	@test IG._gv_colnames(false)[6] == "direction"

	# Node coordinates come off the range and the increment, honouring pixel registration.
	G = GMT.mat2grid(Float32[ix + iy for iy in 0:3, ix in 0:4];
	                 x = collect(0.0:4.0), y = collect(0.0:3.0))
	x, y, Z, dx, dy = IG._gv_axes(G)
	@test length(x) == 5 && length(y) == 4 && size(Z) == (4, 5)
	@test isapprox(x[1], 0.0) && isapprox(x[end], 4.0) && isapprox(dx, 1.0) && isapprox(dy, 1.0)

	# The courtesy command line written to the console. It is not run — it is what a GMT script
	# would need to draw the same field.
	cmd = IG._gv_command(Dict("polar" => "1", "azimuth" => "1", "color" => "red"),
	                     "u.grd", "v.grd", 2, 3, 0.5, 4.0, "be", 0.33, "18", 0.25)
	@test occursin("grdvector u.grd v.grd", cmd) && occursin(" -A", cmd) && occursin(" -Z", cmd)
	@test occursin("-Ix2/3", cmd) && occursin("-Si", cmd) && occursin("+a18", cmd)
	@test occursin("+b", cmd) && occursin("+e", cmd) && occursin("+n0.25", cmd) && occursin("-Wred", cmd)
	@test occursin("-Sl", IG._gv_command(Dict{String,String}(), "u.grd", "v.grd", 1, 1, 0.0, 4.0, "", 0.33, "18", 0.0))
end

# The arrow-field IMPORT (File > Open xy(z)) draws the same shape through the same builder. This is
# the guard on that: two callers, one function — no second spelling of the geometry.
@testitem "grdvector: the arrow import shares the one arrow builder" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# loc_quiver is exactly _gv_arrow! with a head at the tip and no taper.
	segs = Matrix{Float64}[]
	IG._gv_arrow!(segs, 2.0, 1.0, 0.6, 0.8, "e", 0.33, 0.33, 1.0)
	@test length(segs) == 2
	@test segs[1] == [2.0 1.0; 2.6 1.8]                   # the shaft
	# the two arms meet AT the tip, and the tip is the middle vertex of the head polyline
	@test segs[2][2, :] == [2.6, 1.8]
	@test size(segs[2]) == (3, 2)
	# The arms are symmetric about the shaft direction: equal length, mirrored across it.
	d = [0.6, 0.8];  a1 = segs[2][1, :] .- segs[2][2, :];  a2 = segs[2][3, :] .- segs[2][2, :]
	@test isapprox(sum(abs2, a1), sum(abs2, a2); rtol = 1e-12)
	@test isapprox(sum(a1 .* d), sum(a2 .* d); rtol = 1e-12)
end

@testitem "grdvector: refuses an unusable request" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	mk(n) = GMT.mat2grid(Float32[ix + iy for iy in 0:(n-1), ix in 0:(n-1)];
	                     x = collect(0.0:(n-1)), y = collect(0.0:(n-1)))
	U = mk(10);  V = mk(10);  W = mk(6)
	Z0 = GMT.mat2grid(zeros(Float32, 10, 10); x = collect(0.0:9.0), y = collect(0.0:9.0))
	scene = Ptr{Cvoid}(UInt(0x9D71C700))
	IG._SCENE_OBJS[scene] = Tuple{Symbol,String,Any}[(:grid, "u", U), (:grid, "v", V),
	                                                 (:grid, "small", W), (:grid, "flat", Z0)]
	call(kv) = IG._on_grdvector(scene, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n"))))
	base = ["usescene=0", "grid1=u", "grid2=v", "incmode=auto", "scalemode=auto", "heads=e"]
	try
		@test call(["usescene=0", "grid1=u", "grid2="]) == 0                 # a field is TWO grids
		@test call(["usescene=0", "grid1=", "grid2=v"]) == 0                 # ...and the first one too
		@test call(["usescene=0", "grid1=u", "grid2=no_such_grid.nc"]) == 0  # not a layer, not a file
		@test call(["usescene=0", "grid1=u", "grid2=small"]) == 0            # different dimensions
		@test call(vcat(base, ["scalemode=direct", "scale=-1"])) == 0
		@test call(vcat(base, ["scalemode=direct"])) == 0
		@test call(vcat(base, ["incmode=x", "incx=0"])) == 0
		@test call(vcat(base, ["headlen=3"])) == 0
		@test call(vcat(base, ["headang=180"])) == 0
		@test call(vcat(base, ["norm=-2"])) == 0
		@test call(vcat(base, ["heads=sideways"])) == 0
		# A region has to be a region.
		@test call(vcat(base, ["xmin=5", "xmax=1", "ymin=0", "ymax=9"])) == 0
		@test call(vcat(base, ["xmin=north", "xmax=1", "ymin=0", "ymax=9"])) == 0
		# A region outside the grid, and a field of zero vectors, both leave nothing to draw.
		@test call(vcat(base, ["xmin=100", "xmax=200", "ymin=100", "ymax=200"])) == 0
		@test call(["usescene=0", "grid1=flat", "grid2=flat", "incmode=auto", "scalemode=auto", "heads=e"]) == 0
	finally
		delete!(IG._SCENE_OBJS, scene)
	end
end

# Earth regions leans on GMT.jl for everything a region IS, so what is checkable without a window is
# the rounding argument, the -R read-back, and the refusals that happen before any download.
@testitem "earthregions: the pieces it checks itself" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test IG._er_round("") == 0
	@test IG._er_round("  ") == 0
	@test IG._er_round("2") == "2"
	@test IG._er_round("2/1") == "2/1"
	@test IG._er_round("1/1/1/1") == "1/1/1/1"
	@test IG._er_round("+r2") == "+r2"               # GMT's own syntax travels whole
	@test IG._er_round("+e") == "+e"
	@test_throws ErrorException IG._er_round("coarse")
	@test_throws ErrorException IG._er_round("2/wide")

	# The Region block: all four numbers, and a box that is really a box.
	@test IG._er_region("") == ""
	@test IG._er_region("  ") == ""
	@test IG._er_region("-10/-6/36/39") == "-10/-6/36/39"
	@test_throws ErrorException IG._er_region("-10/-6/36")           # three is not a box
	@test_throws ErrorException IG._er_region("-10/-6/36/north")
	@test_throws ErrorException IG._er_region("-6/-10/36/39")        # West east of East
	@test_throws ErrorException IG._er_region("-10/-6/39/36")        # South north of North
	@test_throws ErrorException IG._er_region("-10/-6/-100/39")      # not a latitude

	# The seven collections and the dataset list are the ones the function itself knows.
	@test "DCW" in IG._ER_COLLECTIONS && "Lakes" in IG._ER_COLLECTIONS
	@test length(IG._ER_COLLECTIONS) == 7
	@test "earth_relief" in IG._ER_DATASETS && "earth_day" in IG._ER_DATASETS
	@test IG._ER_RESOLUTIONS[1] == "01d" && IG._ER_RESOLUTIONS[end] == "01s"

	# EVERY collection must list — including DCW and IHO, the two GMT.jl's own pretty-printer throws
	# on ("header (7) must be equal to that of the table (5)"), which is why the rows are laid out
	# here. A listing that only works for the small collections is no listing at all.
	for coll in IG._ER_COLLECTIONS
		txt = IG._er_listing(coll)
		lines = split(strip(txt), '\n')
		@test length(lines) > 4                       # a header plus real regions
		@test startswith(lines[1], "Code")
		@test all(l -> length(l) > 20, lines)         # every row carries a code, a name and 4 numbers
	end
	# Spot-check two rows whose values are fixed by the shipped tables.
	@test occursin("AD        Andorra", IG._er_listing("DCW"))
	@test occursin("IHO1      Baltic Sea", IG._er_listing("IHO"))
	@test_throws ErrorException IG._er_listing("Atlantis")
end

@testitem "earthregions: refuses an unusable request" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	scene = Ptr{Cvoid}(UInt(0x9D71E770))
	# The second argument is the DIALOG that asked (it is where a listing goes back to); every case
	# here refuses before that pointer is ever used, so a null one is exactly right.
	call(kv) = IG._on_earthregions(scene, C_NULL,
	                               Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n"))))
	# Every one of these returns before GMT.jl is asked for anything, so nothing is downloaded here.
	@test call(["mode=list", "collection=Atlantis"]) == 0
	@test call(["mode=raster", "code="]) == 0                       # no code and no region
	@test call(["mode=raster", "code=IHO31", "dataset=earth_cheese"]) == 0
	# A registration without a resolution is the function's own refusal, said here first.
	@test call(["mode=raster", "code=IHO31", "dataset=earth_relief", "registration=pixel"]) == 0
	@test call(["mode=raster", "code=IHO31", "round=coarse"]) == 0
	@test call(["mode=sideways", "code=IHO31"]) == 0
	# A malformed Region block is refused before anything is fetched — and it is checked even when a
	# code is also present, because the region is what would actually be used.
	@test call(["mode=raster", "code=IHO31", "region=-10/-6/36"]) == 0
	@test call(["mode=raster", "code=IHO31", "region=-6/-10/36/39"]) == 0
	@test call(["mode=raster", "region=-10/-6/39/36"]) == 0
	# The border lines need a country CODE; four coordinates name no country.
	@test call(["mode=raster", "region=-10/-6/36/39", "country=1",
	            "dataset=earth_relief", "res=10m", "name=nowhere"]) == 0
	@test_throws ErrorException IG._er_draw_border(scene, "", "x")
	# Looking a code up has to have a code.
	@test call(["mode=limits", "code="]) == 0
end

# Asking twice for the same thing must not fetch it twice. The check is on the LAYER NAME the request
# makes — region name + dataset + resolution — so it fires before anything is downloaded.
@testitem "earthregions: a dataset already in the window is not fetched again" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9];
	                 x = collect(range(-10, -6, length = 10)), y = collect(range(36, 39, length = 10)))
	scene = Ptr{Cvoid}(UInt(0x9D71E771))
	# The layer the dialog would have made for this exact request.
	IG._SCENE_OBJS[scene] = Tuple{Symbol,String,Any}[(:grid, "PT (earth_relief 10m)", G)]
	call(kv) = IG._on_earthregions(scene, C_NULL,
	                               Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n"))))
	try
		# Same name, same dataset, same resolution: recognised, and NOTHING is downloaded (this test
		# has no network and would hang or fail if it were).
		@test call(["mode=raster", "region=-10/-6/36/39", "name=PT",
		            "dataset=earth_relief", "res=10m"]) == 1
		# A different resolution, or a different dataset, is a different layer — those are not caught
		# here, so they would go to the network; only the option check is exercised.
		@test IG._find_object_exact(scene, :grid, "PT (earth_relief 06m)") === nothing
		@test IG._find_object_exact(scene, :grid, "PT (earth_geoid 10m)") === nothing
		# An IMAGE layer of that name counts just as much as a grid.
		IG._SCENE_OBJS[scene] = Tuple{Symbol,String,Any}[(:image, "Azov (earth_day 30s)", G)]
		@test call(["mode=raster", "region=34/39/45/47", "name=Azov",
		            "dataset=earth_day", "res=30s"]) == 1
	finally
		delete!(IG._SCENE_OBJS, scene)
	end
end

@testitem "grdfilter: filter and distance are both required" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9];
	                 x = collect(range(0, 9, length = 10)), y = collect(range(0, 9, length = 10)))
	scene = Ptr{Cvoid}(UInt(0x9D71F117))
	IG._SCENE_OBJS[scene] = Tuple{Symbol,String,Any}[(:grid, "topo", G)]
	call(kv) = IG._on_grdfilter(scene, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n"))))
	try
		@test call(["filter=", "distance=0", "grid=topo"]) == 0
		@test call(["filter=g0.4", "distance=", "grid=topo"]) == 0
		@test IG._on_grdfilter(Ptr{Cvoid}(UInt(0xDEADF117)),
		                       Base.unsafe_convert(Cstring, Base.cconvert(Cstring, "filter=g1\ndistance=0"))) == 0
	finally
		delete!(IG._SCENE_OBJS, scene)
	end
end
