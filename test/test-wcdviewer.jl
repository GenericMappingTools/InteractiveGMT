# Geophysics > MB-System > Water column viewer (src/wcdviewer.jl, port of kmwcd_viewer.py): the KMALL
# reading, on a synthetic file written here to the datagram layouts of the KMALL.py reader the viewer
# ships with (#MWC, #SPO, #SKM in the .kmwcd; #MRZ in its companion .kmall) with known values.

@testmodule WcdFixture begin
export write_wcd_pair
# little-endian datagram: [size][type][version][sysid][emid u16][sec][nsec] body [size]
function _dg(type::String, t::Float64, body::Vector{UInt8})
	io = IOBuffer()
	n = 20 + length(body) + 4
	write(io, htol(UInt32(n)), codeunits(type), UInt8(1), UInt8(0), htol(UInt16(304)),
	      htol(UInt32(floor(t))), htol(UInt32(round((t - floor(t)) * 1e9))))
	write(io, body, htol(UInt32(n)))
	return take!(io)
end
_b(x...) = (io = IOBuffer(); for v in x; write(io, htol(v)); end; take!(io))

# one ping: 3 beams at -30, 0, +30 degrees, 4 / 5 / 3 samples; c = 1500, fs = 750 -> dr = 1 m
function _mwc(t)
	cmn = _b(UInt16(12), UInt16(7), zeros(UInt8, 8)...)
	tx = vcat(_b(UInt16(12), UInt16(1), UInt16(16), Int16(0), 0.5f0), _b(0f0, 12000f0, 1f0, UInt16(0), Int16(0)))
	rx = _b(UInt16(16), UInt16(3), UInt8(16), UInt8(0), UInt8(20), Int8(0), 750f0, 1500f0)
	beams = UInt8[]
	for (a, amp) in ((-30f0, Int8[-10, 0, 10, 20]), (0f0, Int8[1, 2, 3, 4, 5]), (30f0, Int8[-128, 127, 7]))
		append!(beams, _b(a, UInt16(0), UInt16(0), UInt16(0), UInt16(length(amp)), 0f0), reinterpret(UInt8, amp))
	end
	return _dg("#MWC", t, vcat(_b(UInt16(1), UInt16(1)), cmn, tx, rx, beams))
end
function _spo(t, lat, lon, sog, cog)
	cmn = _b(UInt16(8), UInt16(0), UInt16(0), UInt16(0))
	blk = vcat(_b(UInt32(floor(t)), UInt32(0), 1f0), _b(lat, lon), _b(Float32(sog), Float32(cog), 0f0), codeunits("GGA"))
	return _dg("#SPO", t, vcat(cmn, blk))
end
function _skm(t, headings)
	info = _b(UInt16(12), UInt8(0), UInt8(0), UInt16(1), UInt16(length(headings)), UInt16(132), UInt16(0))
	s = UInt8[]
	for h in headings
		append!(s, codeunits("#KMB"), _b(UInt16(120), UInt16(1), UInt32(floor(t)), UInt32(0), UInt32(0)),
		        _b(0.0, 0.0), _b(0f0, 0f0, 0f0, Float32(h)), _b(zeros(Float32, 17)...), _b(UInt32(0), UInt32(0), 0f0))
	end
	return _dg("#SKM", t, vcat(info, s))
end
# two soundings: amplitude detection (solid, y = 40) and an extra phase detection (hollow, y = -25)
function _mrz(t)
	cmn = _b(UInt16(12), UInt16(7), zeros(UInt8, 8)...)
	ping = zeros(UInt8, 92); ping[1:2] = _b(UInt16(92)); ping[85:86] = _b(UInt16(0)); ping[87:88] = _b(UInt16(0))
	rx = _b(UInt16(32), UInt16(1), UInt16(1), UInt16(120), 0f0, 0f0, 0f0, 0f0, UInt16(0), UInt16(1), UInt16(0), UInt16(4))
	snd = UInt8[]
	for (dt, dm, dc, y, z) in ((0, 1, 0, 40f0, 95f0), (1, 2, 3, -25f0, 50f0))
		s = zeros(UInt8, 120)
		s[3] = dt; s[4] = dm; s[8] = dc
		s[93:96] = _b(z); s[97:100] = _b(y)
		append!(snd, s)
	end
	return _dg("#MRZ", t, vcat(_b(UInt16(1), UInt16(1)), cmn, ping, rx, snd))
end
function write_wcd_pair(dir)
	t0 = 1.7e9
	wcd = joinpath(dir, "line.kmwcd")
	write(wcd, vcat(_spo(t0, 38.5, -9.0, 5.0, 90.0), _skm(t0, [10.0, 20.0]), _mwc(t0 + 0.1), _mwc(t0 + 1.1)))
	write(joinpath(dir, "line.kmall"), vcat(_mrz(t0 + 0.1), _mrz(t0 + 1.1)))
	return wcd, t0
end
end

@testitem "wcd viewer: KMALL water column, nav and bottom detections" setup=[WcdFixture] begin
	IG = InteractiveGMT
	mktempdir() do d
		wcd, t0 = write_wcd_pair(d)
		idx = IG._km_index(wcd)
		@test [x.type for x in idx] == ["#SPO", "#SKM", "#MWC", "#MWC"]
		@test idx[3].time ≈ t0 + 0.1
		amp, ang, dr = IG._km_mwc(IG._km_read(wcd, idx[3]))
		@test size(amp) == (3, 5)
		@test ang == [-30.0, 0.0, 30.0]
		@test dr ≈ 1.0
		@test amp[1, 1:4] == Float32[-10, 0, 10, 20] && isnan(amp[1, 5])
		@test amp[3, 1:3] == Float32[-128, 127, 7]
		@test IG._km_spo(IG._km_read(wcd, idx[1]))[2:5] == (38.5, -9.0, 90.0, 5.0)
		@test IG._km_skm_heading(IG._km_read(wcd, idx[2])) ≈ 15.0

		# open: two pings, the amplitude range, depth max = cos(0) x 4 samples x 1 m
		n, amin, amax, dmax = IG._wcd_open(wcd)
		@test (n, amin, amax) == (2, -128.0, 127.0)
		@test dmax ≈ 4.0
		# the companion .kmall supplies the bottom: solid one flipped twice (kmwcd_viewer.py), hollow once
		B = IG._wcd_bottom(0)
		@test B == [(40.0, 95.0, 1.0, 0.0), (25.0, 50.0, 2.0, 1.0)]
		@test occursin("SOG=9.72 kn, Pos=38.500000,-9.000000", IG._wcd_meta(1))

		# pick the cell drawn at x = -2 (the viewer draws -sin(angle) * range), depth ~3.46: beam +30
		# at sample 4 -> across -2.0, depth 3.46; heading 15 -> east -1.93, north 0.52 m
		row = IG._wcd_pick(0, -2.0, 3.46)
		@test row[1] == "0" && row[3] == "-2.00" && row[4] == "3.46" && row[9] == "Heading"
		@test parse(Float64, row[7]) ≈ 38.5 + 0.5176 / 111320 atol = 1e-6
		@test parse(Float64, row[8]) ≈ -9.0 - 1.9319 / (111320 * cosd(38.5)) atol = 1e-6
	end
end

@testitem "wcd viewer: the window opens a file, steps pings, picks, saves the image" tags=[:gui] setup=[WcdFixture] begin
	IG = InteractiveGMT
	if !haskey(IG._LIB_FNS, :gmtvtk_wcd_open)
		@test_skip "the experimental water column viewer is not built into this library"
	else
		pump() = for _ in 1:10; sleep(0.05); end
		mktempdir() do d
			wcd, _ = write_wcd_pair(d)
			try
				@test wcdviewer(wcd)
				pump()
				s = IG._wcd_state()
				@test s.open && s.npings == 2 && s.ping == 0
				@test IG._wcd_set_ping(1) && IG._wcd_state().ping == 1
				@test IG._wcd_pick_at(-2.0, 3.46) && IG._wcd_state().npicks == 1
				png = joinpath(d, "w.png")
				@test IG._wcd_save_png(png) && filesize(png) > 2000
			finally
				IG._wcd_close()
				pump()
			end
		end
	end
end