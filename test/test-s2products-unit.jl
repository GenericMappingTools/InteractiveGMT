# Satellite > Copernicus > Sentinel-2 MSI products (src/s2products.jl): the catalogue filter, the parser
# of a catalogue answer (cloud cover, quicklook), the band files per spacing, the band panel, the
# quicklook pane, the cut to a tile, and (tag :net) one live search.

@testitem "Sentinel-2: catalogue filter and answer" tags=[:unit, :s2] begin
	IG = InteractiveGMT
	D = IG.GMT.Dates
	f = IG._s2_filter("MSIL2A", -8.5, -7.5, 37.0, 37.5, D.Date(2026, 10, 1), D.Date(2026, 11, 24), 30)
	@test occursin("Collection/Name eq 'SENTINEL-2'", f)
	@test occursin("POLYGON((-8.5 37.0,-7.5 37.0,-7.5 37.5,-8.5 37.5,-8.5 37.0))", f)
	@test occursin("contains(Name,'_MSIL2A_')", f)
	@test occursin("att/Name eq 'cloudCover' and att/OData.CSC.DoubleAttribute/Value le 30", f)
	@test !occursin("cloudCover", IG._s2_filter("MSIL1C", 0.0, 1.0, 0.0, 1.0, D.Date(2026, 1, 1), D.Date(2026, 1, 2), 100))

	# One entry of a real catalogue answer (2026-11-24), trimmed to what the parser reads.
	js = """{"value":[{"Id":"af3d4369-04ea-400e-a0af-4d846eebdb68",
	  "Name":"S2C_MSIL2A_20261114T112321_N0513_R037_T29SPA_20261114T131303.SAFE","ContentLength":422748648,
	  "ContentDate":{"Start":"2026-11-14T11:23:21.025000Z","End":"2026-11-14T11:23:21.025000Z"},
	  "GeoFootprint":{"type":"Polygon","coordinates":[[[-7.05,37.03],[-7.88,37.04],[-7.89,36.05],[-7.37,36.04],[-7.05,37.03]]]},
	  "Attributes":[{"Name":"cloudCover","Value":1.672865310137,"ValueType":"Double"}],
	  "Assets":[{"Type":"QUICKLOOK","Id":"f8c75dd2-6b00-4e31-bc9f-90f262955069",
	    "DownloadLink":"https://catalogue.dataspace.copernicus.eu/odata/v1/Assets(f8c75dd2-6b00-4e31-bc9f-90f262955069)/\$value"}]}]}"""
	s = IG._s2_parse(IG._shub_json(js)["value"][1])
	@test s.p.id == "af3d4369-04ea-400e-a0af-4d846eebdb68"
	@test s.p.t0 == IG.DateTime(2026, 11, 14, 11, 23, 21)
	@test (s.tile, s.orbit, round(s.cloud, digits = 2), length(s.p.rings)) == ("29SPA", 37, 1.67, 1)
	@test endswith(s.ql, "Assets(f8c75dd2-6b00-4e31-bc9f-90f262955069)/\$value")
	@test !IG._s2_isl1c(s)
	@test IG._s2_base(s) == "S2C_MSIL2A_20261114T112321_N0513_R037_T29SPA_20261114T131303"
	@test startswith(IG._s2_label(s, mktempdir()), "2026-11-14 11:23  S2C  29SPA  cloud 2 %  403.2 MB")

	@test IG._s2_key("T29SPA_20261114T112321_B8A_20m.jp2") == "B8A"
	@test IG._s2_key("T29SPA_20261114T112321_B10.jp2") == "B10"
	@test IG._s2_key("T29SPA_20261114T112321_SCL_60m.jp2") == "SCL"
	@test IG._s2_key("MTD_TL.xml") == ""
end

@testitem "Sentinel-2: band files per spacing, the band panel and the quicklook pane" tags=[:unit, :s2] begin
	IG = InteractiveGMT
	mk(name) = IG.S2Scene(IG.S1Product("id-" * name, name, IG.DateTime(2026, 11, 14, 11, 23, 21), 4.2e8, "", 0, Matrix{Float64}[]),
	                      12.5, "", "29SPA", 37)
	l2 = mk("S2C_MSIL2A_20261114T112321_N0513_R037_T29SPA_20261114T131303.SAFE")
	l1 = mk("S2C_MSIL1C_20261114T112321_N0513_R037_T29SPA_20261114T115242.SAFE")
	t = "T29SPA_20261114T112321"
	# The trees the Nodes listing gave (2026-11-24), as _s2_nodes caches them.
	IG._S2_NODES[l2.p.id] = ("L2A_T29SPA_A011447_20261114T112820",
		[("R10m", "$(t)_B04_10m.jp2", 4.5e7), ("R10m", "$(t)_B08_10m.jp2", 4.4e7), ("R10m", "$(t)_TCI_10m.jp2", 6.6e7),
		 ("R20m", "$(t)_B04_20m.jp2", 1.2e7), ("R20m", "$(t)_SCL_20m.jp2", 2.8e5), ("R60m", "$(t)_B09_60m.jp2", 1.5e6)])
	IG._S2_NODES[l1.p.id] = ("L1C_T29SPA_A011447_20261114T112820",
		[("", "$(t)_B04.jp2", 4.3e7), ("", "$(t)_B8A.jp2", 1.1e7), ("", "$(t)_B10.jp2", 6.0e5)])
	try
		@test sort!(collect(keys(IG._s2_files_at(l2, 10)))) == ["B04", "B08", "TCI"]
		@test sort!(collect(keys(IG._s2_files_at(l2, 20)))) == ["B04", "SCL"]
		@test collect(keys(IG._s2_files_at(l1, 10))) == ["B04"]          # L1C: a band only at its own spacing
		@test collect(keys(IG._s2_files_at(l1, 20))) == ["B8A"]
		@test collect(keys(IG._s2_files_at(l1, 60))) == ["B10"]
		@test IG._s2_node_url(l2, ["GRANULE", "g", "IMG_DATA", "R10m", "f.jp2"]) ==
		      "https://download.dataspace.copernicus.eu/odata/v1/Products(id-$(l2.p.name))/Nodes($(l2.p.name))" *
		      "/Nodes(GRANULE)/Nodes(g)/Nodes(IMG_DATA)/Nodes(R10m)/Nodes(f.jp2)"

		dir = mktempdir()
		d = Dict("p_bres_idx" => "0", "p_b_b04" => "1", "p_b_b08" => "1", "p_b_scl" => "1", "p_b_b02" => "1")
		io = IOBuffer()
		IG._s2_fill_bands!(io, d, l2, dir)
		r = String(take!(io))
		@test occursin("enable:p_b_b04=1", r) && occursin("enable:p_b_tci=1", r) && occursin("enable:p_b_scl=0", r)
		@test occursin("enable:p_b_b02=0", r)                       # not in this (trimmed) tree
		@test occursin("label:lbl_bsize=2 at 10 m: whole tile, 84.9 MB", r)   # B04 + B08: SCL is not at 10 m
		# Only the region: its size in pixels on the tile's own UTM grid, nothing downloaded whole.
		dc = merge(d, Dict("p_cut" => "1", "p_rg_w" => "-8.0", "p_rg_e" => "-7.9", "p_rg_s" => "37.0", "p_rg_n" => "37.1"))
		IG._s2_fill_bands!(io, dc, l2, dir)
		@test occursin(r"label:lbl_bsize=2 at 10 m: region only, \d+x\d+ px", String(take!(io)))
		@test IG._s2_epsg(l2) == 32629
		# ONE file per request; once it is in the folder nothing is fetched again.
		outs = IG._s2_outs(l2, ["B04", "B08"], 10, dir, nothing)
		@test length(outs) == 1 && endswith(outs[1][2], "_L2A_10m_B04-B08.tiff")
		mkpath(IG._s2_band_dir(l2, dir));  write(outs[1][2], "x")
		IG._s2_fill_bands!(io, d, l2, dir)
		@test occursin("label:lbl_bsize=2 at 10 m: on disk", String(take!(io)))
		IG._s2_fill_bands!(io, d, nothing, dir)
		@test occursin("enable:run_bands=0", String(take!(io)))

		# The quicklook pane: a scene with no quicklook says so; an image on disk goes in as rich text.
		IG._s2_show_thumb!(io, l2, dir)
		r = String(take!(io))
		@test occursin("label:lbl_thumb=The catalogue has no quicklook", r)
		@test occursin("label:lbl_thumbinfo=2026-11-14 11:23  S2C  tile 29SPA  orbit R037  cloud 12.5 %", r)
		q = IG.S2Scene(l2.p, 12.5, "https://never.asked/x", "29SPA", 37)
		mkpath(joinpath(dir, "quicklooks"));  write(joinpath(dir, "quicklooks", IG._s2_base(q) * ".jpg"), UInt8[0xff, 0xd8, 0xff])
		IG._s2_show_thumb!(io, q, dir)
		@test occursin("label:lbl_thumb=<img src=\"file:///$(replace(dir, '\\' => '/'))/quicklooks/$(IG._s2_base(q)).jpg\"",
		               String(take!(io)))
	finally
		delete!(IG._S2_NODES, l2.p.id);  delete!(IG._S2_NODES, l1.p.id)
	end
end

@testitem "Sentinel-2: a lon/lat region cut to the tile, in the tile's UTM" tags=[:unit, :s2] begin
	IG = InteractiveGMT
	# A 10 km piece of tile 29SPB's corner (UTM 29N, x from 600 km), written like a band file.
	G = IG.GMT.mat2grid(rand(Float32, 100, 100); x = collect(range(600005.0, 609995.0, length = 100)),
	                    y = collect(range(4190205.0, 4200195.0, length = 100)), proj4 = "+proj=utm +zone=29 +datum=WGS84 +units=m +no_defs")
	f = joinpath(mktempdir(), "T29SPB_band.tiff")
	IG.GMT.gmtwrite(f, G)
	# A box that hangs over the tile's west edge is cut to it (cutcube pads past the edge with zeros).
	box = (-7.9, -7.85, 37.88, 37.9)
	u = IG._box_to(IG._LONLAT, "+proj=utm +zone=29 +datum=WGS84 +units=m +no_defs", box...)
	b, _ = IG._rs_file_box(f)
	@test u[1] < b[1] < u[2]                                    # the box does hang over the edge
	r = IG._s2_cut_box(f, box)
	@test r == [b[1], min(u[2], b[2]), max(u[3], b[3]), min(u[4], b[4])]
	@test IG._s2_cut_box(f, nothing) === nothing
	@test_throws ErrorException IG._s2_cut_box(f, (-6.0, -5.9, 37.0, 37.1))   # nowhere near the tile
end

@testitem "Sentinel-2: one live search (L2A), its quicklook and its band tree" tags=[:net, :s2] begin
	IG = InteractiveGMT
	D = IG.GMT.Dates
	found, total = IG._s2_search("MSIL2A", -8.5, -7.5, 37.0, 37.5, D.Date(2026, 10, 25), D.Date(2026, 11, 24), 30)
	@test !isempty(found) && total >= length(found)
	@test all(s -> s.cloud <= 30 && !isempty(s.ql) && !isempty(s.p.rings), found)
	@test issorted(found, by = s -> s.p.t0, rev = true)
	s = found[1]
	q = IG._s2_quicklook(s, mktempdir())
	@test isfile(q) && open(io -> read(io, 2), q) == UInt8[0xff, 0xd8]
	@test issubset(["B02", "B03", "B04", "B08", "TCI"], collect(keys(IG._s2_files_at(s, 10))))
	@test "SCL" in keys(IG._s2_files_at(s, 20))
end
