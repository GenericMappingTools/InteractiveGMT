# CI-safe unit tests for GMT menu > Interpolate (src/interpolate.jl): the dialog->GMT.jl keyword
# mapping, the method table, the table branch of the geometry-prefill callback, and the inputs the
# wrapper must refuse. Everything here runs WITHOUT a window: `_interp_kwargs` / `_interp_module` are
# split out of the callback precisely so the mapping is testable, and a refusal never reaches the C
# side (it is logged through _viewer_log_error, which swallows a dead handle).
# The full Compute path (delivery into a live scene, "Plot pts") needs a window — :gui tier.

@testitem "Interpolate: callback is wired" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	for s in (:_on_interpolate, :_register_interpolate, :_interp_kwargs, :_interp_module, :_interp_read)
		@test isdefined(IG, s)
	end
	# A registration with no export in the DLL symbol list stays silently unwired at runtime.
	@test :gmtvtk_set_interpolate_callback in IG._LIB_SYMBOLS
end

@testitem "Interpolate: every method the dialog offers has a module" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	# The list must match cb_method in deps/ui/interpolation_dialog.ui / InterpolationDialog.
	@test IG._interp_module("surface")        === GMT.surface
	@test IG._interp_module("nearneighbor")   === GMT.nearneighbor
	@test IG._interp_module("triangulate")    === GMT.triangulate
	@test IG._interp_module("blockmean")      === GMT.blockmean
	@test IG._interp_module("blockmedian")    === GMT.blockmedian
	@test IG._interp_module("blockmode")      === GMT.blockmode
	@test IG._interp_module("greenspline")    === GMT.greenspline
	@test IG._interp_module("sphinterpolate") === GMT.sphinterpolate
	# Mirone's mbgrid is NOT a GMT module — it is ours (src/mbgrid.jl over deps/src/mbgrid.c) — but
	# it is in the same table all the same, because the dialog offers it and the callback runs it
	# through the same call. A method the combo offers and this table refuses is the bug.
	@test IG._interp_module("mbgrid") === IG.mbgrid
	@test_throws Exception IG._interp_module("no_such_method")
end

@testitem "Interpolate: the shared options map onto GMT.jl keywords" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	d = Dict("region" => "0/10/0/10", "inc" => "0.5", "pixel" => "1", "verbose" => "1")
	kw = IG._interp_kwargs(d, "surface", true)
	@test kw[:region] == "0/10/0/10"
	@test kw[:inc] == "0.5"
	@test kw[:registration] === :pixel
	@test kw[:verbose] === true
	@test kw[:f] === :g                      # geographic must be STATED: an in-memory table carries no flag
	@test !haskey(IG._interp_kwargs(d, "surface", false), :f)

	# The search radius lives in the main dialog and belongs to nearneighbor only.
	dn = Dict("region" => "0/10/0/10", "inc" => "0.5", "radius" => "25k")
	@test IG._interp_kwargs(dn, "nearneighbor", true)[:search_radius] == "25k"
	@test !haskey(IG._interp_kwargs(dn, "surface", true), :search_radius)

	# opt_ lines are GMT.jl keyword names: copied as they come, "1" being a ticked box.
	do_ = Dict("region" => "0/10/0/10", "inc" => "0.5",
	           "opt_sectors" => "8/4", "opt_empty" => "-9999", "opt_weights" => "true")
	kwn = IG._interp_kwargs(do_, "nearneighbor", false)
	@test kwn[:sectors] == "8/4"
	@test kwn[:empty] == "-9999"
	@test kwn[:weights] === true          # a ticked box says "true"; "1" would be a value
	@test IG._interp_kwargs(Dict("region" => "0/10/0/10", "inc" => "1", "opt_sectors" => "1"),
	                        "nearneighbor", false)[:sectors] == "1"
	# An empty box means "let the module default" — the keyword must not be sent at all.
	@test !haskey(IG._interp_kwargs(Dict("region" => "0/10/0/10", "inc" => "1", "opt_empty" => ""),
	                                "nearneighbor", false), :empty)
end

@testitem "Interpolate: surface's two tension boxes become one -T" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	base = Dict("region" => "0/10/0/10", "inc" => "0.5")
	both_same = IG._interp_kwargs(merge(base, Dict("opt_tension_i" => "0.25", "opt_tension_b" => "0.25")), "surface", false)
	@test both_same[:tension] == "0.25"          # plain -T sets interior AND boundary
	both_diff = IG._interp_kwargs(merge(base, Dict("opt_tension_i" => "0.25", "opt_tension_b" => "0.5")), "surface", false)
	@test both_diff[:tension] == "i0.25 -Tb0.5"  # GMT takes -T twice when they differ
	only_i = IG._interp_kwargs(merge(base, Dict("opt_tension_i" => "0.35")), "surface", false)
	@test only_i[:tension] == "i0.35"
	only_b = IG._interp_kwargs(merge(base, Dict("opt_tension_b" => "0.7")), "surface", false)
	@test only_b[:tension] == "b0.7"
	# The raw box names never reach GMT.jl.
	for k in (:tension_i, :tension_b)
		@test !haskey(both_diff, k)
	end
end

@testitem "Interpolate: Clip cells is the cell form of -M" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	base = Dict("region" => "0/10/0/10", "inc" => "0.5")
	kw = IG._interp_kwargs(merge(base, Dict("opt_maskcells" => "2")), "surface", false)
	@test kw[:mask] == "2c"
	@test !haskey(kw, :maskcells)
	# Already written as cells: not doubled.
	@test IG._interp_kwargs(merge(base, Dict("opt_maskcells" => "0c")), "surface", false)[:mask] == "0c"
	# A radius given in the Max radius box wins; the two boxes are the same GMT option.
	kw2 = IG._interp_kwargs(merge(base, Dict("opt_mask" => "50k", "opt_maskcells" => "2")), "surface", false)
	@test kw2[:mask] == "50k"
end

@testitem "Interpolate: greenspline glues spline type and tension" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	base = Dict("region" => "0/10/0/10", "inc" => "0.5")
	kw = IG._interp_kwargs(merge(base, Dict("opt_splines" => "t", "opt_tension" => "0.7",
	                                        "opt_distmode" => "1")), "greenspline", false)
	@test kw[:splines] == "t0.7"
	@test kw[:distmode] == "1"        # a numeric option whose value is 1 is NOT a ticked box
	@test !haskey(kw, :tension)
	# sphinterpolate's `tension` IS a keyword of its own — it must survive untouched.
	kws = IG._interp_kwargs(merge(base, Dict("opt_tension" => "s")), "sphinterpolate", false)
	@test kws[:tension] == "s"
end

@testitem "Interpolate: block* must be asked for a grid, not a table" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	base = Dict("region" => "0/10/0/10", "inc" => "1")
	# Without -A the block modules hand back a per-block TABLE, so a field is always sent.
	@test IG._interp_kwargs(base, "blockmean", false)[:field] == "mean"
	@test IG._interp_kwargs(base, "blockmedian", false)[:field] == "median"
	@test IG._interp_kwargs(base, "blockmode", false)[:field] == "mode"
	# ...but never over the user's own pick.
	@test IG._interp_kwargs(merge(base, Dict("opt_field" => "highest")), "blockmean", false)[:field] == "highest"
	@test !haskey(IG._interp_kwargs(base, "surface", false), :field)
end

@testitem "Interpolate: a data file's own limits prefill the geometry" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	D = GMT.mat2ds([0.0 0.0 1.0; 4.0 0.0 2.0; 0.0 3.0 3.0; 4.0 3.0 4.0; 2.0 1.5 5.0])
	s = IG._gridmeta_string(D)                       # the same callback the "OR Ref grid" row uses
	f = split(s, '/')
	@test length(f) == 8
	w, e, sth, n = parse.(Float64, f[1:4])
	inc = parse(Float64, f[5])
	@test inc > 0
	# The prefilled region must COVER the data it was derived from — a sparse table used to come back
	# with a zero-height region because GMT's estimate rounds the node count.
	@test w == 0.0 && sth == 0.0
	@test e >= 4.0 && n >= 3.0
	@test parse(Int, f[7]) == round(Int, (e - w) / inc) + 1
	@test parse(Int, f[8]) == round(Int, (n - sth) / inc) + 1
	@test IG._gridmeta_string("not a GMT object") == ""
end

@testitem "Interpolate: refuses what it cannot grid" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	scene = Ptr{Cvoid}(UInt(0x9D71C0DE))
	call(kv) = IG._on_interpolate(scene, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n")))) |> (r -> IG._errored(r, "Interpolate"))
	@test call(["method=surface", "region=0/10/0/10", "inc=1"]) == 0                  # no input file
	@test call(["method=surface", "infile=no_such_file.xyz", "region=0/10/0/10", "inc=1"]) == 0
end

# Runs the real modules on a small synthetic table — no window, no DLL: just the mapping this file
# builds handed to GMT.jl exactly as the callback does.
@testitem "Interpolate: the mapping actually grids" tags=[:unit] begin
	IG = InteractiveGMT; GMT = IG.GMT
	xy = [(x, y) for x in 0:0.5:10 for y in 0:0.5:10]
	D = GMT.mat2ds([Float64[p[1] for p in xy] Float64[p[2] for p in xy] Float64[sin(p[1]) + cos(p[2]) for p in xy]])
	d = Dict("region" => "0/10/0/10", "inc" => "1")
	# mbgrid is deliberately NOT in this loop: it is the one method whose numerics live in the DLL,
	# so it cannot run in a DLL-less checkout. Its own end-to-end grid is in test-mbgrid-unit.jl,
	# gated on the library having loaded.
	for m in ("surface", "triangulate", "blockmean", "blockmedian", "blockmode", "nearneighbor", "greenspline")
		dd = copy(d)
		(m == "nearneighbor") && (dd["radius"] = "2")
		(m == "greenspline")  && (dd["opt_distmode"] = "1")
		G = IG._interp_module(m)(D; IG._interp_kwargs(dd, m, false)...)
		@test isa(G, GMT.GMTgrid)
		@test size(G.z) == (11, 11)
	end
end
