# Satellite > Landsat scenes (src/landsat.jl): names, band tables, the band panel and the browse pane,
# and (tag :net) live searches on both sources.

@testitem "Landsat: names, products and band tables" tags=[:unit, :landsat] begin
	IG = InteractiveGMT
	@test IG._ls_pathrow("LC09_L2SP_203034_20261031_20261101_02_T1") == (203, 34)
	@test IG._ls_pathrow("LT05_L1TP_204034_19900824_20200916_02_T1") == (204, 34)
	@test IG._ls_pathrow("whatever") == (0, 0)
	@test IG._ls_jstr("a\"b\\c") == "\"a\\\"b\\\\c\""
	for (src, list) in IG._LS_PRODUCTS, p in list
		bands = IG._LS_BANDS[(p[3], p[4])]
		@test length(bands) <= IG._LS_NSLOT                       # every product fits the panel's slots
		@test all(b -> b in first.(bands), IG._LS_DEFAULT[(p[3], p[4])])
	end
	@test [b[1] for b in IG._LS_BANDS[(:ot, 1)] if b[4] === :own] == ["B8"]   # the 15 m band is never a cube layer
end

@testitem "Landsat: the band panel and the browse pane" tags=[:unit, :landsat] begin
	IG = InteractiveGMT
	h = Ptr{Cvoid}(UInt(0x1234))                                   # a window key only: nothing is drawn
	s = IG.LsScene(:usgs, 1, "LC92030342026304LGN00", "LC09_L2SP_203034_20261031_20261101_02_T1",
	               IG.DateTime(2026, 10, 31), 0.29, 0.0, "", Matrix{Float64}[], 203, 34, Dict{String,String}())
	dir = mktempdir()
	io = IOBuffer()
	try
		# A new product: its band names on the slots, its default bands ticked, the slots past them locked.
		IG._ls_fill_bands!(io, h, Dict{String,String}(), 1, 1, s, dir)
		r = String(take!(io))
		@test occursin("label:p_band_01=SR B1", r) && occursin("label:p_band_11=QA AEROSOL", r)
		@test occursin("set:p_band_02=1", r) && occursin("set:p_band_05=1", r) && occursin("set:p_band_01=0", r)
		@test occursin("label:p_band_12=\nenable:p_band_12=0", r)
		@test occursin("label:lbl_bsize=4 band(s), whole scene: 4 new", r)
		# A refresh keeps the user's ticks: nothing is set, the count follows the boxes.
		IG._ls_fill_bands!(io, h, Dict("p_band_04" => "1", "p_band_08" => "1"), 1, 1, s, dir)
		r = String(take!(io))
		@test !occursin("set:p_band_02", r) && occursin("label:lbl_bsize=2 band(s), whole scene: 2 new", r)
		mkpath(IG._ls_band_dir(s, dir));  write(IG._ls_band_file(s, "SR_B4", dir), "x");  write(IG._ls_band_file(s, "ST_B10", dir), "x")
		IG._ls_fill_bands!(io, h, Dict("p_band_04" => "1", "p_band_08" => "1"), 1, 1, s, dir)
		@test occursin("label:lbl_bsize=2 band(s), whole scene: on disk", String(take!(io)))
		# Another source and product (USGS, TM Level-1): its bands, and its defaults.
		IG._ls_fill_bands!(io, h, Dict{String,String}(), 2, 6, nothing, dir)
		r = String(take!(io))
		@test occursin("label:p_band_06=B6", r) && occursin("set:p_band_01=1", r) && occursin("enable:run_bands=0", r)

		IG._ls_show_thumb!(io, s, dir, Dict{String,String}())
		r = String(take!(io))
		@test occursin("label:lbl_thumb=The source has no browse picture", r)
		@test occursin("label:lbl_thumbinfo=2026-10-31  LC09  path 203 row 034  cloud 0.3 %", r)
		mkpath(joinpath(dir, "quicklooks"));  write(joinpath(dir, "quicklooks", s.name * ".jpg"), UInt8[0xff, 0xd8, 0xff])
		sb = IG.LsScene(s.src, s.prod, s.id, s.name, s.t0, s.cloud, 0.0, "https://never.asked/x", s.rings, 203, 34, s.files)
		IG._ls_show_thumb!(io, sb, dir, Dict{String,String}())      # already on disk: never asked for
		@test occursin("<img src=\"file:///$(replace(dir, '\\' => '/'))/quicklooks/$(s.name).jpg\"", String(take!(io)))
		@test startswith(IG._ls_label(s, dir), "2026-10-31  LC09  203/034  cloud 0 %")
	finally
		delete!(IG._LS_LASTPROD, h)
	end
end

@testitem "Landsat: live searches, Planetary Computer (with a band) and USGS" tags=[:net, :landsat] begin
	IG = InteractiveGMT
	D = IG.GMT.Dates
	found, more = IG._ls_pc_search(1, -8.5, -7.5, 37.0, 37.5, D.Date(2026, 10, 1), D.Date(2026, 11, 24), 30)
	@test !isempty(found) && all(s -> s.cloud <= 30 && s.path > 0 && !isempty(s.rings) && !isempty(s.browse), found)
	s = found[1]
	@test haskey(s.files, "$(s.name)_SR_B4.TIF") && haskey(s.files, "$(s.name)_QA_PIXEL.TIF")
	dir = mktempdir()
	k = findfirst(b -> b[1] == "QA_PIXEL", IG._ls_bands(s))
	f = IG._ls_fetch_bands(s, [k], dir, Dict{String,String}())[1][2]
	@test isfile(f) && open(io -> read(io, 4), f) in IG._LS_TIF_MAGIC     # anonymous, signed, a real TIFF
	mtl = joinpath(dirname(f), "$(s.name)_MTL.txt")                         # the calibration file came with it
	@test isfile(mtl) && occursin("REFLECTANCE_MULT_BAND_4", read(mtl, String))
	@test isfile(IG._ls_quicklook(s, dir, Dict{String,String}()))
	if !isempty(IG._rs_netrc_get(IG._rs_netrc_path(), IG._LS_MACHINE)[1])      # needs the USGS login
		found, tot = IG._ls_usgs_search(1, -8.5, -7.5, 37.0, 37.5, D.Date(2026, 10, 1), D.Date(2026, 11, 24), 30, Dict{String,String}())
		@test !isempty(found) && all(s -> s.cloud <= 30 && !isempty(s.browse), found)
		@test issorted(found, by = s -> s.t0, rev = true)
		@test isfile(IG._ls_quicklook(found[1], mktempdir(), Dict{String,String}()))
	end
end
