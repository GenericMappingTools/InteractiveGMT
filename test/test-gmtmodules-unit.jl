# CI-safe unit tests for the GMT-module dialogs added to the GMT menu (grdtrend, grdlandmask,
# grdfilter). These cover what can be checked WITHOUT a window: the callbacks are registered, and
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
	          :_on_trend2d, :_register_trend2d, :_on_cptbuild, :_register_cptbuild)
		@test isdefined(IG, s)
	end
	# Every registration must have its export in the DLL symbol list, or the feature silently stays
	# "not wired" at runtime (new-c-export-needs-lib-symbols).
	for sym in (:gmtvtk_set_grdtrend_callback, :gmtvtk_set_grdlandmask_callback,
	            :gmtvtk_set_grdfilter_callback, :gmtvtk_set_gridcalc_callback,
	            :gmtvtk_set_grdfft_callback, :gmtvtk_set_grdhisteq_callback,
	            :gmtvtk_set_xyz2grd_callback, :gmtvtk_set_grdfill_callback,
	            :gmtvtk_set_trend2d_callback, :gmtvtk_set_cptbuild_callback)
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
