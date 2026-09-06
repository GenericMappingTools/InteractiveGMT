# Catalina BENCHMARK 1 — the solitary wave running up a sloping beach, as a demo/teaching tool:
# Geophysics > Tsunamis > "Catalina benchmark 1".
#
# IT STARTS AT THE BEGINNING, AND IT STARTS AT ONCE. Clicking the entry builds the model and opens
# the tank at t = 0 — bathymetry plus the analytic solitary wave as the water surface — in the iGMT
# window the menu was used in, IN 3-D. It runs NOTHING: a tool that disappears into a simulation the
# moment it is opened is a tool nobody can be shown. The run is its own, explicit step
# (the Aquamoto dialog's "Benchs" tab, or the NSWING dialog).
#
# The window is reached through the SAME door a dropped NSWING cube uses (`_on_drop` -> the Aquamoto
# viewer), never a private display path.
#
# THE MODEL IS THE BENCHMARK'S OWN. `_bm1_bathymetry` / `_bm1_source` are the ported `faz_bat` /
# `faz_fonte` of Mirone's testa_barnabeu.m, and the analytic table below is that file's own literal:
# the flume runs from x = -200 m (the land side) to x = 50 km, 51 rows wide, with the analytic
# solitary wave as the t = 0 free surface.
#
# THE RUN, WHEN IT HAPPENS, IS ALWAYS THE FULL 50 km FLUME. Only the DISPLAY is clipped, to
# [-200, 20000] m — a DISPLAY WINDOW applied when the grids are read (`_AQUA_XWIN`/`_aqua_clipx`),
# never a cropped copy on disk. The land side ends at -200 m, which is where the model itself starts.
# Shortening the simulation itself would be a different problem: at 20 km the water is ~2 km deep, so the outer domain reaches the
# shore well inside the 220 s the benchmark covers, and the analytic initial condition still carries
# real amplitude out to ~29 km.
#
# RUN LENGTH: 250 s of model time (5000 cycles at dt = 0.05), one output step every 50 cycles = 100
# steps. The benchmark's analytic solution is tabulated to 220 s; the run goes past it so the wave's
# return down the slope is on screen too.
#
# A RUN GOES THROUGH nswing.jl's OWN RUNNER (`_nswing_run_external`) — off-process, watched by a
# main-thread Timer that drives the progress bar. NSWING is one long ccall, and running it in this
# process freezes the window even on a worker thread (a thread inside a long ccall never reaches a GC
# safepoint), which is precisely why that runner exists. There is no second run path here.

# The .m `analytic()` table, byte-for-byte — the benchmark's DIGITISED reference profiles, kept for
# cross-checks only. It is NOT what the run is started from any more (see `_bm1_source`): at t = 0 it
# is ~10% low in the trough, and a run started from it drifts from the analytic solution.  Columns in
# (x, eta, u) triples, one triple per benchmark time (t = 0 / 160 / 175 / 220 s).
const _BM1_ANALYTIC = Float64[
50328.0	0.00000	0.00000	50316.9	1.10722	0.04919	50317.9	1.00656	0.04919	50324.5	0.35230	0.02108
49326.5	0.00000	0.00000	49315.4	1.10722	0.04919	49316.9	0.95623	0.04919	49324.0	0.25164	0.01405
48335.0	0.00000	0.00000	48324.4	1.05689	0.04919	48326.0	0.90590	0.04216	48334.0	0.10066	0.00703
47353.6	0.00000	0.00000	47343.0	1.05689	0.04919	47345.1	0.85558	0.04216	47355.1	-0.15098	0.00000
46382.3	0.00000	0.00000	46372.2	1.00656	0.04919	46374.2	0.80525	0.04216	46386.3	-0.40262	-0.01405
45421.0	0.00000	0.00000	45411.5	0.95623	0.04919	45414.0	0.70459	0.03513	45429.1	-0.80525	-0.03513
44469.8	0.00000	0.00000	44460.8	0.90590	0.04216	44463.3	0.65426	0.03513	44481.9	-1.20787	-0.04919
43528.7	0.00000	0.00000	43520.6	0.80525	0.04216	43522.6	0.60394	0.03513	43545.3	-1.66082	-0.07729
42597.6	0.00000	0.00000	42590.1	0.75492	0.04216	42592.6	0.50328	0.02811	42618.3	-2.06345	-0.09837
41676.6	0.00000	0.00000	41669.6	0.70459	0.03513	41672.1	0.45295	0.02811	41700.8	-2.41574	-0.11242
40765.7	0.00000	0.00000	40759.6	0.60394	0.03513	40762.2	0.35230	0.02108	40791.9	-2.61706	-0.12648
39864.8	0.00000	0.00000	39859.3	0.55361	0.03513	39862.3	0.25164	0.02108	39891.5	-2.66738	-0.13350
38974.0	0.00000	0.00000	38969.5	0.45295	0.02811	38973.5	0.05033	0.00703	39000.2	-2.61706	-0.12648
38093.3	0.00000	0.00000	38089.2	0.40262	0.02811	38094.8	-0.15098	0.00000	38117.4	-2.41574	-0.11945
37222.6	0.00000	0.00000	37219.6	0.30197	0.02108	37227.1	-0.45295	-0.01405	37243.7	-2.11378	-0.10540
36361.5	0.05033	0.00000	36360.5	0.15098	0.01405	36370.5	-0.85558	-0.04216	36380.1	-1.81181	-0.09134
35510.9	0.05033	0.00000	35512.4	-0.10066	0.00000	35524.5	-1.30853	-0.06324	35526.0	-1.45951	-0.07729
34670.0	0.10066	0.00000	34674.5	-0.35230	-0.01405	34689.1	-1.81181	-0.09134	34682.5	-1.15754	-0.06324
33839.0	0.15098	0.00000	33848.1	-0.75492	-0.03513	33862.7	-2.21443	-0.11242	33849.6	-0.90590	-0.04919
33018.2	0.20131	0.00000	33032.3	-1.20787	-0.05621	33045.9	-2.56673	-0.13350	33026.7	-0.65426	-0.03513
32207.4	0.25164	0.00000	32226.5	-1.66082	-0.08432	32238.1	-2.81837	-0.14756	32215.0	-0.50328	-0.02811
31406.2	0.35230	0.00000	31431.3	-2.16410	-0.11242	31438.4	-2.86870	-0.15458	31413.2	-0.35230	-0.02108
30614.5	0.50328	0.00000	30644.7	-2.51640	-0.14053	30647.2	-2.76804	-0.15458	30622.1	-0.25164	-0.01405
29832.9	0.65426	0.00000	29867.7	-2.81837	-0.15458	29864.6	-2.51640	-0.14756	29841.5	-0.20131	-0.01405
29061.4	0.80525	0.00000	29098.6	-2.91902	-0.16864	29091.6	-2.21443	-0.12648	29071.0	-0.15098	-0.01405
28299.4	1.00656	0.00000	28338.2	-2.86870	-0.16864	28328.1	-1.86214	-0.11242	28310.5	-0.10066	-0.00703
27547.5	1.20787	0.00000	27586.3	-2.66738	-0.15458	27574.7	-1.50984	-0.09134	27560.6	-0.10066	-0.00703
26805.7	1.40918	0.00000	26843.4	-2.36542	-0.14053	26831.9	-1.20787	-0.07729	26820.3	-0.05033	-0.00703
26073.4	1.66082	0.00000	26110.2	-2.01312	-0.12648	26099.1	-0.90590	-0.05621	26090.5	-0.05033	-0.00703
25351.7	1.86214	0.00000	25387.0	-1.66082	-0.10540	25377.4	-0.70459	-0.04919	25370.8	-0.05033	-0.00703
24640.1	2.06345	0.00000	24673.8	-1.30853	-0.08432	24665.8	-0.50328	-0.03513	24661.2	-0.05033	-0.00703
23938.5	2.26476	0.00000	23971.2	-1.00656	-0.07027	23964.7	-0.35230	-0.02811	23961.7	-0.05033	-0.00703
23247.5	2.41574	0.00000	23279.2	-0.75492	-0.05621	23274.2	-0.25164	-0.02108	23272.2	-0.05033	-0.00703
22566.6	2.56673	0.00000	22597.8	-0.55361	-0.04216	22594.3	-0.20131	-0.01405	22592.7	-0.05033	-0.00703
21896.2	2.66738	0.00000	21926.9	-0.40262	-0.03513	21924.4	-0.15098	-0.01405	21923.4	-0.05033	-0.00703
21236.4	2.71771	0.00000	21266.6	-0.30197	-0.02811	21264.6	-0.10066	-0.01405	21264.1	-0.05033	-0.00703
20587.2	2.71771	0.00000	20616.4	-0.20131	-0.02108	20615.4	-0.10066	-0.00703	20614.9	-0.05033	-0.00703
19948.5	2.66738	0.00000	19976.7	-0.15098	-0.01405	19975.7	-0.05033	-0.00703	19975.7	-0.05033	-0.00703
19319.9	2.61706	0.00000	19347.1	-0.10066	-0.01405	19346.6	-0.05033	-0.00703	19346.6	-0.05033	-0.00703
18701.9	2.51640	0.00000	18728.1	-0.10066	-0.01405	18727.6	-0.05033	-0.00703	18727.6	-0.05033	-0.00703
18093.9	2.41574	0.00000	18118.6	-0.05033	-0.01405	18118.6	-0.05033	-0.00703	18118.6	-0.05033	-0.00703
17496.5	2.26476	0.00000	17519.7	-0.05033	-0.00703	17519.7	-0.05033	-0.00703	17519.7	-0.05033	-0.01405
16909.2	2.11378	0.00000	16930.8	-0.05033	-0.01405	16930.8	-0.05033	-0.00703	16930.8	-0.05033	-0.01405
16331.9	1.96279	0.00000	16352.1	-0.05033	-0.00703	16352.1	-0.05033	-0.00703	16352.1	-0.05033	-0.01405
15765.2	1.76148	0.00000	15783.4	-0.05033	-0.01405	15783.4	-0.05033	-0.01405	15783.9	-0.10066	-0.01405
15208.1	1.61050	0.00000	15224.7	-0.05033	-0.01405	15224.7	-0.05033	-0.01405	15225.2	-0.10066	-0.01405
14661.6	1.40918	0.00000	14676.1	-0.05033	-0.01405	14676.1	-0.05033	-0.01405	14676.7	-0.10066	-0.01405
14125.1	1.20787	0.00000	14137.6	-0.05033	-0.01405	14137.6	-0.05033	-0.01405	14138.1	-0.10066	-0.01405
13598.6	1.00656	0.00000	13609.2	-0.05033	-0.01405	13609.2	-0.05033	-0.01405	13609.7	-0.10066	-0.01405
13082.8	0.75492	0.00000	13090.8	-0.05033	-0.01405	13090.8	-0.05033	-0.01405	13091.3	-0.10066	-0.01405
12578.0	0.40262	0.00000	12582.5	-0.05033	-0.01405	12582.5	-0.05033	-0.01405	12583.0	-0.10066	-0.02108
12084.3	-0.05033	0.00000	12083.8	0.00000	-0.01405	12084.3	-0.05033	-0.01405	12084.8	-0.10066	-0.02108
11603.1	-0.75492	0.00000	11595.6	0.00000	-0.02108	11596.1	-0.05033	-0.02108	11596.6	-0.10066	-0.02108
11134.1	-1.66082	0.00000	11117.5	0.00000	-0.02108	11118.0	-0.05033	-0.02108	11118.5	-0.10066	-0.02108
10677.6	-2.81837	0.00000	10648.9	0.05033	-0.02811	10649.9	-0.05033	-0.02108	10650.9	-0.15098	-0.02108
10232.2	-4.07657	0.00000	10190.4	0.10066	-0.03513	10191.9	-0.05033	-0.02811	10192.9	-0.15098	-0.02811
9797.4	-5.38510	0.00000	9742.0	0.15098	-0.04216	9743.5	0.00000	-0.02811	9745.0	-0.15098	-0.02811
9371.1	-6.54264	0.00000	9303.6	0.20131	-0.05621	9305.6	0.00000	-0.03513	9307.2	-0.15098	-0.02811
8952.3	-7.44854	0.00000	8874.8	0.30197	-0.06324	8877.4	0.05033	-0.04216	8879.4	-0.15098	-0.03513
8539.2	-7.90150	0.00000	8456.1	0.40262	-0.08432	8459.1	0.10066	-0.04919	8461.6	-0.15098	-0.03513
8132.0	-7.95182	0.00000	8046.9	0.55361	-0.09837	8050.5	0.20131	-0.06324	8054.5	-0.20131	-0.03513
7730.9	-7.59953	0.00000	7648.3	0.65426	-0.11945	7652.4	0.25164	-0.07729	7656.9	-0.20131	-0.04216
7336.3	-6.89494	0.00000	7259.3	0.80525	-0.14053	7263.8	0.35230	-0.09134	7269.4	-0.20131	-0.04216
6949.8	-5.98903	0.00000	6880.3	0.95623	-0.16864	6884.9	0.50328	-0.11242	6891.9	-0.20131	-0.04919
6572.8	-5.03280	0.00000	6511.4	1.10722	-0.19674	6516.5	0.60394	-0.13350	6524.5	-0.20131	-0.05621
6205.4	-4.02624	0.00000	6152.6	1.25820	-0.23187	6157.6	0.75492	-0.16161	6167.2	-0.20131	-0.06324
5849.6	-3.17066	0.00000	5803.8	1.40918	-0.25998	5808.9	0.90590	-0.19674	5819.9	-0.20131	-0.07729
5504.9	-2.41574	0.00000	5465.1	1.56017	-0.28809	5469.6	1.10722	-0.22485	5482.7	-0.20131	-0.08432
5171.2	-1.76148	0.00000	5137.0	1.66082	-0.32322	5141.0	1.25820	-0.26701	5155.1	-0.15098	-0.10540
4849.1	-1.25820	0.00000	4819.4	1.71115	-0.35133	4822.4	1.40918	-0.30214	4837.5	-0.10066	-0.12648
4538.6	-0.90590	0.00000	4511.9	1.76148	-0.38646	4514.4	1.50984	-0.34430	4530.0	-0.05033	-0.14756
4238.6	-0.60394	0.00000	4214.5	1.81181	-0.41456	4216.5	1.61050	-0.38646	4232.6	0.00000	-0.18269
3949.7	-0.40262	0.00000	3927.6	1.81181	-0.44267	3928.6	1.71115	-0.42862	3945.2	0.05033	-0.21782
3671.9	-0.30197	0.00000	3651.3	1.76148	-0.46375	3651.3	1.76148	-0.47078	3667.4	0.15098	-0.27403
3404.2	-0.20131	0.00000	3385.6	1.66082	-0.49186	3384.1	1.81181	-0.51293	3399.7	0.25164	-0.33025
3146.5	-0.10066	0.00000	3129.4	1.61050	-0.51293	3127.4	1.81181	-0.55509	3142.0	0.35230	-0.40051
2899.9	-0.10066	0.00000	2884.3	1.45951	-0.54104	2881.3	1.76148	-0.59725	2894.4	0.45295	-0.48483
2662.9	-0.05033	0.00000	2648.8	1.35886	-0.56212	2645.7	1.66082	-0.63941	2656.8	0.55361	-0.58320
2436.4	-0.05033	0.00000	2423.8	1.20787	-0.59023	2420.3	1.56017	-0.68157	2429.8	0.60394	-0.69562
2219.5	0.00000	0.00000	2208.9	1.05689	-0.61833	2204.9	1.45951	-0.73076	2213.4	0.60394	-0.82913
2013.1	0.00000	0.00000	2004.1	0.90590	-0.65346	2000.0	1.30853	-0.77994	2007.1	0.60394	-0.99074
1816.8	0.00000	0.00000	1809.8	0.70459	-0.69562	1805.8	1.10722	-0.83615	1811.8	0.50328	-1.16640
1630.6	0.00000	0.00000	1625.6	0.50328	-0.74481	1621.6	0.90590	-0.90642	1627.1	0.35230	-1.37719
1454.5	0.00000	0.00000	1451.5	0.30197	-0.79400	1447.4	0.70459	-0.99074	1453.0	0.15098	-1.61610
1288.4	0.00000	0.00000	1287.9	0.05033	-0.85723	1283.9	0.45295	-1.08911	1288.4	0.00000	-1.88310
1132.4	0.00000	0.00000	1135.4	-0.30197	-0.91345	1130.9	0.15098	-1.21559	1134.4	-0.20131	-2.16416
986.4	0.00000	0.00000	994.0	-0.75492	-0.94858	988.4	-0.20131	-1.37719	988.9	-0.25164	-2.45225
850.5	0.00000	0.00000	864.6	-1.40918	-0.93453	856.1	-0.55361	-1.58096	852.1	-0.15098	-2.71223
724.7	0.00000	0.00000	748.4	-2.36542	-0.83615	735.3	-1.05689	-1.82689	722.2	0.25164	-2.91600
609.0	0.00000	0.00000	645.7	-3.67394	-0.59023	626.6	-1.76148	-2.12903	598.9	1.00656	-3.02842
503.3	0.00000	0.00000	556.6	-5.33477	-0.14053	530.0	-2.66738	-2.48738	482.6	2.06345	-3.02140
407.7	0.00000	0.00000	481.1	-7.34789	0.56212	446.4	-3.87526	-2.90195	372.9	3.47263	-2.88087
322.1	0.00000	0.00000	417.2	-9.51199	1.51070	376.5	-5.43542	-3.37975	271.3	5.08313	-2.59981
246.6	0.00000	0.00000	361.9	-11.52511	2.60683	320.6	-7.39822	-3.90674	178.7	6.79428	-2.19227
181.2	0.00000	0.00000	311.5	-13.03495	3.67486	278.3	-9.71330	-4.51804	95.6	8.55576	-1.68636
125.8	0.00000	0.00000	265.7	-13.99118	4.58831	248.6	-12.28003	-5.24177	24.2	10.16626	-1.11721
80.5	0.00000	0.00000	225.0	-14.44414	5.29096	229.5	-14.89709	-6.18332	-36.2	11.67610	-0.53401
45.3	0.00000	0.00000	190.2	-14.49446	5.78984	218.4	-17.31283	-7.50431	-84.0	12.93430	0.01405
20.1	0.00000	0.00000	164.1	-14.39381	6.11306	212.9	-19.27562	-9.23985	-118.8	13.89053	0.46375
5.0	0.00000	0.00000	147.5	-14.24282	6.29575	209.4	-20.43317	-10.75758	-140.4	14.54479	0.75886
0.0	0.00000	0.00000	141.9	-14.19250	6.35196	207.9	-20.78546	-11.31970	-147.5	14.74610	0.85723
]

# 1-D interpolation with GMT's own sampler (`sample1d`), matching the .m `interp1(..., 'linear' |
# 'spline', fillval)`: query points outside the data's x-range come back NaN and are replaced by
# `fillval`. Never a hand-rolled spline — the algorithm is ported, not substituted.
function _bm1_interp1(xp::Vector{Float64}, yp::Vector{Float64}, xq::Vector{Float64};
                      interp::Symbol=:cubic, fillval::Float64=NaN)::Vector{Float64}
	idx = sortperm(xp)
	ds  = hcat(xp[idx], yp[idx])
	r   = sample1d(ds; T=xq, F=interp, V=:q)
	out = r.data[:, 2]
	isnan(fillval) || (out[isnan.(out)] .= fillval)
	return out
end

# The level-0 flume: 20 m of land at the west end sloping to -5000 m at x = 50 km, with a 10 m bank
# along each side (the .m `faz_bat`). `dx` is the along-flume cell size; rows are 4*dx apart.
function _bm1_bathymetry(dx::Float64=25.0)::GMTgrid
	x  = collect(-200.0:dx:50000.0)
	nx = length(x)
	z  = collect(range(20.0, -5000.0, length=nx))
	bat = vcat(fill(10.0, 10, nx), repeat(reshape(z, 1, nx), 31, 1), fill(10.0, 10, nx))
	return mat2grid(bat; hdr=[-200.0, 50000.0, 0.0, 50*dx*4, -5000.0, 20.0, 0.0, dx, 4*dx])
end

# THE NESTED LEVELS. The benchmark's inundation end is only as good as the cell it is resolved on:
# 25 m on a 1:10 slope puts the whole run-up tip inside two columns, which is what made the last
# timesteps disagree with the analytic solution. The .m source (and `examples/testa_barnabeu2.jl`,
# its port) runs the flume with two nests — 5 m, then 1 m — and compares on the FINEST one.
#
# `faz_batL2`/`faz_batL3` of the .m, as further methods of THIS function: same operation (build the
# flume's bathymetry at some level), same name. All of them reduce to the same y-invariant x-slope,
# so the slope/header maths exists once, in `_bm1_slope_grid`.
function _bm1_slope_grid(x0::Float64, x1::Float64, y0::Float64, y1::Float64, dx::Float64)::GMTgrid
	x   = collect(x0:dx:x1)
	nx  = length(x)
	z   = collect(range(abs(x0)/10, -x1/10, length=nx))
	n   = round(Int, (y1 - y0) / (4dx) + 1)
	bat = repeat(reshape(z, 1, nx), n, 1)
	return mat2grid(bat; hdr=[x0, x1, y0, y1, z[end], z[1], 0.0, dx, 4dx])
end

# level-1 nest: the window given as raw a/b/y0 bounds — there is no parent GMTgrid at this level, the
# level-0 flume's own geometry is a literal in the .m too.
function _bm1_bathymetry(a::Float64, b::Float64, y0::Float64, dxInner::Float64, dxOuter::Float64)::GMTgrid
	x0  = a + dxOuter/2 + dxInner/2
	x1  = b - dxOuter/2 - dxInner/2
	y0b = y0 + dxOuter*4/2 + dxInner*4/2
	return _bm1_slope_grid(x0, x1, y0b, y0b + 29*dxInner*4, dxInner)
end

# level-2 nest: the window offset off the parent grid `Gp`, .m-exact. `xSpan` is the raw x1 offset
# from `Gp`'s west edge (160 parent cells), `rowA:rowB` the y-window in parent rows.
function _bm1_bathymetry(Gp::GMTgrid, dxInner::Float64, dxOuter::Float64, xSpan::Float64,
                         rowA::Int, rowB::Int)::GMTgrid
	x0 = Gp.range[1] + dxOuter/2 + dxInner/2
	x1 = Gp.range[1] + xSpan - dxOuter/2 - dxInner/2
	y0 = Gp.range[3] + rowA*Gp.inc[2] + dxOuter*4/2 + dxInner*4/2
	y1 = Gp.range[3] + rowB*Gp.inc[2] - dxOuter*4/2 - dxInner*4/2
	return _bm1_slope_grid(x0, x1, y0, y1, dxInner)
end

# The chain a run is given, `levels` counted the .m's own way (its `QUANTAS`): 1 = the outer flume
# alone, 2 = one nest (5 m), 3 = two nests (5 m then 1 m) — the .m's own default, and the only level
# at which the run-up is resolved well enough to compare with the analytic solution.
function _bm1_nests(; dxOuter::Float64=25.0, dxInner::Float64=5.0, levels::Int=3)::Vector{GMTgrid}
	levels <= 1 && return GMTgrid[]
	L1 = _bm1_bathymetry(-200.0, 1025.0, 1900.0, dxInner, dxOuter)
	levels == 2 && return GMTgrid[L1]
	L2 = _bm1_bathymetry(L1, 1.0, dxInner, 160 * Float64(L1.inc[1]), 16, 25)
	return GMTgrid[L1, L2]
end

# The initial free surface: the analytic free surface at time `t`, on the flume's x and repeated
# across all 51 rows (the .m `faz_fonte`) — but taken from THE SOLUTION ITSELF
# (`benchmark1_analytic.jl`), not from the tabulated literal above.
#
# WHY IT IS NOT THE TABLE ANY MORE. The table is a digitised copy: at t = 0 its trough reads -7.95 m
# where the benchmark's own closed-form condition (Carrier--Wu--Yeh's two Gaussians) gives -8.81 m —
# ~10% low, rms 0.28 m over the profile. A model started from that solves a different problem, and
# the error grows as the wave shoals: measured on the 1 m nest, a run from the TABLE sat 1.3 m rms
# away from the analytic solution at t = 160/175/220 s, while the same run from THIS condition sits
# 0.03 / 0.08 / 0.10 m rms from it. That is the whole disagreement the η(x) reference curve showed;
# it was never an error in the analytic code (which reproduces its own closed form to 0.05 m rms and
# whose evolution the model follows to within a tenth of a metre).
#
# AND IT IS `catalina1(t)` AT EVERY t, INCLUDING 0 — not the exact closed form (`initial_eta`), which
# would look like the better start and measurably is not: a run from the closed form sits 0.28 / 0.46 /
# 0.21 m rms from the analytic solution, a run from `catalina1(0)` 0.03 / 0.08 / 0.10 m. The reason is
# the comparison itself. Carrier--Greenspan's condition is set in the TRANSFORM plane, and its physical
# profile is displaced (x = sigma^2/16 - eta); the state the solution is actually at when t = 0 is what
# `catalina1(0)` returns. Starting the model anywhere else means comparing it against a solution of a
# slightly different problem. One function for "the analytic free surface at t", for the model's start
# and for the reference curve alike (SACRED_LAW.md).
function _bm1_source(dx::Float64=25.0, t::Float64=0.0)::GMTgrid
	x = collect(-200.0:dx:50000.0)
	R = CatalinaBenchmark1.catalina1(t; xmax=60000.0, npoints=3000)
	z = _bm1_interp1(R.x, R.eta, x; interp=:linear, fillval=0.0)
	fonte = repeat(reshape(z, 1, length(x)), 51, 1)
	return mat2grid(fonte; hdr=[-200.0, 50000.0, 0.0, 50*dx*4, minimum(z), maximum(z), 0.0, dx, 4*dx])
end


# The cube's time-varying variable, found the way the viewer finds it — netCDF introspection, never a
# hard-coded name.
function _bm1_var(cube::String)::String
	names = _aqua_find_all_varnames(cube, "bathymetry")
	isempty(names) && error("Catalina benchmark 1: '$cube' carries no time-varying variable")
	return names[1]
end

# Its number of timesteps, off the netCDF's own shape report (a 3-D variable is [time y x]).
function _bm1_nsteps(cube::String, var::String)::Int
	for v in _netcdf_subdatasets(cube)
		v.name == var && length(v.dims) >= 3 && return Int(v.dims[1])
	end
	error("Catalina benchmark 1: could not read the layer count of '$var' in '$cube'")
end

# Write an NSWING-shaped cube: x/y/time coordinates, a static `bathymetry` (y,x) and the time-varying
# quantity (time,y,x), carrying the units/long_name/actual_range attributes GMT reads and the
# `TSU = NSWING` global attribute the Aquamoto file test looks for. `layer(k)` hands over the k-th
# (1-based) slice as a (ny,nx) Float32 matrix, so the whole cube is never held at once.
#
# The 1-D pieces and every attribute go through shapenc.jl's own create/write/release helpers; only
# the 2-D/3-D array create + block write is spelled out here, because those helpers are 1-D by
# construction (a SHAPENC file carries no rasters).
function _bm1_write_cube(dst::String, xs::Vector{Float64}, ys::Vector{Float64},
                         times::Vector{Float64}, bat::Matrix{Float32}, varname::String,
                         layer::Function)::String
	nx, ny, nt = length(xs), length(ys), length(times)
	ds = _shnc_create_multidim(dst)
	try
		root = _shnc_root(ds)
		dimX = _shnc_create_dim(root, "x", nx)
		dimY = _shnc_create_dim(root, "y", ny)
		dimT = _shnc_create_dim(root, "time", nt)
		edt64, edt32, edtS = _shnc_edt_f64(), _shnc_edt_f32(), _shnc_edt_str()

		for (dim, nm, vals, un) in ((dimX, "x", xs, "meters"), (dimY, "y", ys, "meters"),
		                            (dimT, "time", times, "Seconds"))
			a = _shnc_create_array(root, nm, dim, edt64; deflate=0)
			_shnc_array_write!(a, edt64, vals)
			_shnc_set_str_attr!(a, false, "units", edtS, un)
			_shnc_set_f64v_attr!(a, false, "actual_range", edt64, [minimum(vals), maximum(vals)])
			_shnc_release_array(a)
		end

		mkarr = (nm, dims) -> begin
			dv = Ptr{Cvoid}[dims...]
			h = ccall((:GDALGroupCreateMDArray, GMT.libgdal), Ptr{Cvoid},
			          (Ptr{Cvoid}, Cstring, Csize_t, Ptr{Ptr{Cvoid}}, Ptr{Cvoid}, Ptr{Ptr{UInt8}}),
			          root, nm, Csize_t(length(dv)), dv, edt32, C_NULL)
			h == C_NULL && error("Catalina benchmark 1: could not create netCDF array '$nm'")
			h
		end
		# One (ny,nx) Float32 matrix -> the row-major block netCDF stores, written at `start`.
		writeblk = (arr, start::Vector{UInt64}, cnt::Vector{UInt64}, m::Matrix{Float32}) -> begin
			buf = Vector{Float32}(undef, length(m))
			@inbounds for iy in 1:size(m, 1), ix in 1:size(m, 2)
				buf[(iy - 1) * size(m, 2) + ix] = m[iy, ix]
			end
			step = fill(Int64(1), length(start))
			r = ccall((:GDALMDArrayWrite, GMT.libgdal), Cint,
			          (Ptr{Cvoid}, Ptr{UInt64}, Ptr{UInt64}, Ptr{Int64}, Ptr{Int64}, Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Cvoid}, Csize_t),
			          arr, start, cnt, step, C_NULL, edt32, buf, C_NULL, 0)
			r == 0 && error("Catalina benchmark 1: netCDF write failed")
		end
		tag = (arr, long, un, lo, hi) -> begin
			_shnc_set_str_attr!(arr, false, "long_name", edtS, long)
			_shnc_set_str_attr!(arr, false, "units", edtS, un)
			_shnc_set_f64v_attr!(arr, false, "actual_range", edt64, [lo, hi])
		end

		ab = mkarr("bathymetry", (dimY, dimX))
		writeblk(ab, UInt64[0, 0], UInt64[ny, nx], bat)
		fb = filter(isfinite, bat)
		tag(ab, "bathymetry", "meters", isempty(fb) ? 0.0 : Float64(minimum(fb)),
		                                isempty(fb) ? 1.0 : Float64(maximum(fb)))
		_shnc_release_array(ab)

		az = mkarr(varname, (dimT, dimY, dimX))
		zlo, zhi = Inf, -Inf
		for k in 1:nt
			m = layer(k)
			writeblk(az, UInt64[k - 1, 0, 0], UInt64[1, ny, nx], m)
			fm = filter(isfinite, m)
			if !isempty(fm)
				zlo = min(zlo, Float64(minimum(fm)));  zhi = max(zhi, Float64(maximum(fm)))
			end
		end
		isfinite(zlo) || ((zlo, zhi) = (0.0, 1.0))
		tag(az, "Sea surface", "meters", zlo, zhi)
		_shnc_release_array(az)

		_shnc_set_str_attr!(root, true, "Conventions", edtS, "COARDS/CF-1.0")
		_shnc_set_str_attr!(root, true, "title", edtS, "Water levels series created by Mirone-NSWING")
		_shnc_set_str_attr!(root, true, "TSU", edtS, "NSWING")
		for hh in (dimX, dimY, dimT); _shnc_release_dim(hh); end
		for hh in (edt64, edt32, edtS); _shnc_release_edt(hh); end
		# EVERY handle GDAL hands back must be released before the close, the ROOT GROUP included — an
		# NC4 file whose group is still referenced is closed unflushed and reads back as "not recognized
		# as being in a supported file format" (shapenc.jl's own release note, learned the same way).
		ccall((:GDALGroupRelease, GMT.libgdal), Cvoid, (Ptr{Cvoid},), root)
	finally
		ccall((:GDALClose, GMT.libgdal), Cint, (Ptr{Cvoid},), ds)
	end
	return dst
end



# THE STARTING STATE, as a one-step Aquamoto cube: the flume's bathymetry and the analytic solitary
# wave at t = 0. NOTHING IS SIMULATED HERE — this is the benchmark before it runs, and it is what the
# menu entry opens with, at once. The two arrays come from the model builders above (the same
# `faz_bat` / `faz_fonte` a run is given); they are not rebuilt.
#
# IT IS THE WHOLE 50 km FLUME, like the run. The 20 km tank on screen is a DISPLAY WINDOW applied at
# READ time (`_aqua_clipx`, aquamoto.jl) — one mechanism for the starting state and for a result, and
# no cropped copy of anything on disk.
function _bm1_initial_cube(; dx::Float64=25.0,
                             outdir::String=joinpath(tempdir(), "igmt_benchmark1"),
                             tag::String="", force::Bool=false)::String
	mkpath(outdir)
	Gb, Gs = _bm1_bathymetry(dx), _bm1_source(dx, 0.0)
	ny, nxa = size(Gb.z)
	# THE MODEL'S OWN EXTENT IS IN THE NAME. A tank left by the version that wrote the display window
	# into the file spans 20 km, not the flume's 50, and re-using it would put a clipped model back on
	# screen under a mechanism that clips again. Naming the file after what is in it means the stale
	# one is simply never asked for — it cannot be deleted anyway: a netCDF this process has read
	# through GDAL stays locked (EBUSY) for the life of the session (see drop.jl).
	dst = joinpath(outdir, "benchmark1_t0$(tag)_$(round(Int, (Gb.range[2] - Gb.range[1]) / 1000))km.nc")
	(!force && isfile(dst)) && return dst
	xs  = collect(range(Float64(Gb.range[1]), Float64(Gb.range[2]); length=nxa))
	ys  = collect(range(Float64(Gb.range[3]), Float64(Gb.range[4]); length=ny))
	_bm1_write_cube(dst, xs, ys, [0.0], Float32.(Gb.z), "stage", _ -> Float32.(Gs.z))
	return dst
end

"""
    catalina_benchmark1(scene; xmin=-200.0, xmax=20000.0) -> String

Open Catalina benchmark 1 in the iGMT window `scene`, AT ITS BEGINNING: the flume's bathymetry with
the analytic solitary wave as the water surface at t = 0, clipped for display to x ∈ [`xmin`,
`xmax`]. It builds two grids and writes a one-step cube — nothing is simulated, so the window is up
immediately. Running the model is a separate, explicit step — the Aquamoto dialog's "Benchs" tab.
"""
function catalina_benchmark1(scene::Ptr{Cvoid}; xmin::Float64=-200.0, xmax::Float64=20000.0,
                             azim::Float64=-35.0, elev::Float64=20.0, ve::Float64=0.15)::String
	# ALREADY IN THIS WINDOW (the entry clicked twice): the drop door would ignore the file, so there is
	# nothing to open — just put the window back in 3-D and be done.
	push!(_BM1_SCENES, scene)                 # this window has an analytic solution (η(x) reference curve)
	_AQUA_XWIN[scene] = (xmin, xmax)          # ...and shows [xmin, xmax] of the 50 km flume, clipped at READ
	here = get(_AQUA, scene, nothing)
	if here !== nothing && occursin("benchmark1_t0", here.path)
		return here.path                          # already the tank: the entry was clicked twice
	end
	# A file ALREADY OPEN IN ANOTHER LIVE WINDOW is ignored by `_on_drop` (it raises that window
	# instead), so a second window asking for the benchmark would wait forever for a state that is
	# never coming. Give each window its own copy of the tank — it is 350 kB.
	cube = ""
	for i in 0:99
		cand = _bm1_initial_cube(; tag=(i == 0 ? "" : "_$i"))
		any(st -> abspath(st.path) == abspath(cand), values(_AQUA)) && continue
		cube = cand
		break
	end
	isempty(cube) && error("Catalina benchmark 1: every benchmark tank is already open in a window")
	# THE VIEWER'S OWN DOOR, QUEUED. `gmtvtk_aqua_queue_open` is the route the Aquamoto dialog's own
	# Browse takes (openFor + setAndOpenPath), deferred to the next turn of the Qt loop. Deferred
	# matters: the menu entry shows the dialog FIRST, so by the time this runs the window usually
	# exists, and reaching it synchronously from inside a Julia call is the re-entry that hangs and
	# then kills the process (verified live). The open puts the tank in 3-D on its own.
	ccall(_fn(:gmtvtk_aqua_queue_open), Cvoid, (Ptr{Cvoid}, Cstring), scene, cube)
	# AND IT COMES UP IN 3-D. The open is queued, so the 3-D switch waits for it on a TASK — it cannot
	# be thrown from here (the tank does not exist yet) and it cannot pump the loop (the app's own pump
	# is already running). `expect` pins it to THIS file, so it fires for the tank and not for whatever
	# the window happened to show before.
	@async _bm1_show_3d(scene; xmin=xmin, xmax=xmax, azim=azim, elev=elev, ve=ve,
	                    expect=cube, pump=false)
	return cube
end

# A TANK IS A 3-D THING, so this opens in 3-D — never the flat top-down map the window is promoted
# into. Two steps, both through controls that already exist: an Aquamoto slice always arrives as a
# flat draped image (`showLayerImageTail` sets layerImgMode on every push), so the layer is put back
# on its surface with the window's OWN "Shaded image (2-D)" switch (`imgmode=0`, = sceneSetShadedImage2D),
# and then the camera is placed absolutely with `gmtvtk_set_view_azel_h` — which also leaves flat-2D
# mode, so drag-rotation and the gizmo come back with it.
function _bm1_show_3d(scene::Ptr{Cvoid}; xmin::Float64, xmax::Float64,
                      azim::Float64, elev::Float64, ve::Float64, timeout::Float64=60.0,
                      expect::String="", pump::Bool=true)
	# `pump=false` is for the version of this that runs on a TASK, waiting for an open that was QUEUED
	# on the Qt loop: there the app's own pump (the eventloop.jl Timer) is already turning, and pumping
	# it again from in here would re-enter it. The wait is then a plain sleep, which yields to the
	# scheduler and lets that Timer do its work.
	pumpNow() = pump && ccall(_fn(:gmtvtk_process_events), Cint, ())
	want = isempty(expect) ? "" : abspath(expect)
	# WAIT FOR THE FIRST SLICE, not merely for the state to exist. The Aquamoto window shows it from
	# its own open handler, and a slice is ALWAYS pushed as a flat draped image — so a 3-D switch
	# thrown before that lands is undone by the slice that arrives after it. `st.first` is the state's
	# own record of "a slice has been drawn", flipped at the end of `_aquamoto_slice`.
	t0 = time()
	while true
		st = get(_AQUA, scene, nothing)
		if st !== nothing && !st.first && (isempty(want) || abspath(st.path) == want)
			break
		end
		pumpNow()
		(time() - t0) > timeout && error("Catalina benchmark 1: the Aquamoto viewer did not open the tank")
		sleep(0.02)
	end
	for _ in 1:5                          # let the open handler's own tail finish before switching
		pumpNow()
		sleep(0.02)
	end
	# THE WINDOW'S MODE FIRST. A tsunami window is promoted from the empty launcher, which is a FLAT-2D
	# map, and flat-2D locks drag-rotation and hides the gizmo — so an oblique camera alone gives a
	# picture that looks 3-D but cannot be turned. `gmtvtk_set_view_mode_h(…, 0)` is the toolbar's own
	# 3-D switch; verified live: without it the scene state still reads flat2d=1.
	ccall(_fn(:gmtvtk_set_view_mode_h), Cint, (Ptr{Cvoid}, Cint), scene, Cint(0))
	# Then the layer itself: an Aquamoto slice always arrives as a flat draped image, and `imgmode=0`
	# is the window's own "Shaded image (2-D)" switch putting it back on its surface.
	ccall(_fn(:gmtvtk_apply_scene_state), Cvoid, (Ptr{Cvoid}, Cstring), scene, "imgmode=0;")
	pumpNow()
	ccall(_fn(:gmtvtk_set_view_azel_h), Cint,
	      (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble, Cint, Cdouble, Cdouble, Cdouble),
	      scene, azim, elev, xmax - xmin, ve, Cint(0), 0.0, 0.0, 0.0)
	pumpNow()
	return nothing
end

# Menu callback (Geophysics > Tsunamis > "Catalina benchmark 1"). Errors are reported into the
# window's own Messages dock rather than thrown across the C boundary.
function _on_catalina_benchmark1(scene::Ptr{Cvoid})::Cvoid
	try
		catalina_benchmark1(scene)
	catch e
		@tool_error "Catalina benchmark 1 failed" exception=(e,)
	end
	return
end

# ── the Benchs tab (Aquamoto) ─────────────────────────────────────────────────────────────────
# Start the benchmark and write it to `outfile`. THE RUN GOES THROUGH nswing.jl's OWN RUNNER
# (`_nswing_run_external`) — off-process, watched by a main-thread Timer that drives the progress
# bar. That is not a detail: NSWING is one long ccall, and running it in this process freezes the
# window even on a worker thread (a thread inside a long ccall never reaches a GC safepoint), which
# is exactly why that runner exists. A private `gmt("nswing …")` here would be a second run path and
# a frozen iGMT (SACRED_LAW.md).
#
# Off-process means the inputs must be FILES, so the two model grids are written beside the output.
function _bm1_run_async(scene::Ptr{Cvoid}, outfile::String; ncycles::Int=5000, interval::Int=50,
                        dt::Float64=0.05, dx::Float64=25.0, xmin::Float64=-200.0,
                        xmax::Float64=20000.0, keepram::Bool=false, levels::Int=3)
	_NSWING_RUNNING[] && error("a NSWING run is already going")
	dir = dirname(abspath(outfile))
	mkpath(dir)
	stem = joinpath(dir, splitext(basename(outfile))[1])
	bat  = stem * "_bat.grd"
	src  = stem * "_src.grd"
	Gb   = _bm1_bathymetry(dx)
	gmtwrite(bat, Gb)
	gmtwrite(src, _bm1_source(dx, 0.0))
	# THE NESTED LEVELS, checked before they are written. `_nswing_check_nest_fits` is the run's own
	# pre-flight (Mirone's check_binning port) — the same authority the NSWING dialog uses — so a bad
	# chain is refused here with the real message instead of inside the model.
	nests = _bm1_nests(; dxOuter=dx, levels=levels)
	nestpaths = String[]
	parent = Gb
	for (i, G) in enumerate(nests)
		_nswing_check_nest_fits(parent, G, i == 1 ? "bathymetry" : "nest level $(i-1)", "nest level $i")
		p = stem * "_L$(i).grd"
		gmtwrite(p, G)
		push!(nestpaths, "-$(i)$(p)")            # nswing CLI: -1<grd> -2<grd> …, path ATTACHED
		parent = G
	end
	# ONE RUN PER LEVEL. nswing writes its output on the FINEST grid it was given and on no other
	# (verified: `-G` and `-G…+m` alike hand back a single 1 m cube for a 25/5/1 chain), so a profile
	# that is to be 1 m at the beach, 5 m behind it and 25 m out to sea needs each level's own run:
	#   stage 1  no nests        -> <stem>.nc       the 20 km tank ON SCREEN, and the outer chunk
	#   stage 2  -1              -> <stem>_lev1.nc  the 5 m chunk
	#   stage 3  -1 -2           -> <stem>_lev2.nc  the 1 m chunk
	# Same bathymetry, same source, same -N/-t/interval, so the three carry the same 101 model times
	# and a slice index means the same instant in all of them.
	stages = [(0, stem * ".nc", String[])]
	for i in eachindex(nestpaths)
		push!(stages, (i, stem * "_lev$(i).nc", nestpaths[1:i]))
	end
	cube = stages[1][2]                                  # what the window will show
	for (_, out, _) in stages
		isfile(out) && rm(out; force=true)
	end

	# The stages run one after another, each launched from the previous one's completion (the runner
	# clears `_NSWING_RUNNING` before it calls back), and the RESULT IS OPENED WHEN THE LAST ONE ENDS —
	# the finer cubes must exist before the figure asks for their chunks.
	function runstage(i::Int)
		(lev, out, flags) = stages[i]
		_NSWING_RUNNING[] = true
		_progress_show_async(100, "Benchmark 1: NSWING $(i)/$(length(stages)) (level $lev)…")
		_nswing_run_external(scene, String[bat, src, flags..., "-v",
		                                   "-G$(splitext(out)[1]),$(interval)",
		                                   "-N$(ncycles)", "-t$(dt)"];
		                     on_done = err -> begin
		                         isempty(err) || return                 # the runner already logged it
		                         isfile(out) || return _viewer_log_error(scene,
		                             "Benchmark 1: the level-$lev run left no cube at '$out'")
		                         if i < length(stages)
		                             return runstage(i + 1)
		                         end
		                         disp = _bm1_open_result(scene, cube; xmin=xmin, xmax=xmax)
		                         # The box was ticked before this file existed: honour it now, on a task,
		                         # so the watcher timer this runs in is not held while the cube is read.
		                         keepram && @async _bm1_keep_in_ram(scene; wait_for = disp)
		                     end)
	end
	runstage(1)
	return cube
end

# The x extent a cube actually covers, off its first layer (its own header). One layer read: the file
# may be the 50 km flume (a single-level run) or the 800 m fine window (a nested one), and everything
# that frames the window — the display clip, the camera — has to be told which.
function _bm1_cube_xrange(cube::String)::Tuple{Float64,Float64}
	G = _read_cube_layer("$(cube)?$(_bm1_var(cube))", 1)
	G === nothing && error("Catalina benchmark 1: cannot read '$cube'")
	return (Float64(G.range[1]), Float64(G.range[2]))
end

# Show a finished run: open THE RUN ITSELF in `scene`, in 3-D, with the display window applied at read
# time. Used by both the end of a run and the "Load from disk…" button, so a loaded simulation and a
# fresh one look alike.
#
# NOTHING IS WRITTEN BESIDE THE USER'S RUN. This used to crop the cube into a `…_display20km.nc` next
# to it — a silent 16.8 MB file that came back every time it was deleted. The window now travels in
# `_AQUA_XWIN` and is applied where the grids are READ (`_aqua_clipx`, aquamoto.jl).
function _bm1_open_result(scene::Ptr{Cvoid}, cube::String; xmin::Float64=-200.0,
                          xmax::Float64=20000.0)::String
	push!(_BM1_SCENES, scene)                 # a window has an analytic solution (η(x) reference curve)
	_bm1_register_levels!(scene, cube)        # ...and, if the finer runs are there, a stitched profile
	# WHAT IS ON SCREEN IS THE CUBE'S OWN EXTENT, INTERSECTED WITH THE DISPLAY WINDOW — never the
	# window alone. A NESTED run's output is written by nswing on the FINEST level (verified live: a
	# 5 m / 1 m chain gives a 795 x 40 cube over x = -182…612), so the 20 km window covers it whole and
	# the tank is 800 m long. Framing the camera on the window instead would leave that tank a smear
	# 25 times too far away.
	x0, x1 = _bm1_cube_xrange(cube)
	vx0, vx1 = max(x0, xmin), min(x1, xmax)
	_AQUA_XWIN[scene] = (vx0, vx1)            # a no-op clip when the cube is already inside the window
	xmin, xmax = vx0, vx1
	disp = cube
	if haskey(_AQUA, scene)
		# THIS WINDOW ALREADY HAS AN AQUAMOTO SESSION (the t = 0 tank the tool opened with). Going in
		# through `_on_drop` from here would reach `AquamotoWindow::openFor` -> `setAndOpenPath` ->
		# a BLOCKING Julia call, while this very Julia call is still on the stack: verified live, that
		# re-entry hangs and then kills the process. `gmtvtk_aqua_queue_open` is the door built for
		# exactly this — it defers the open to the next turn of the Qt loop, so this call returns
		# first. The window is put back in 3-D by the open itself, not by pumping the loop from here.
		ccall(_fn(:gmtvtk_aqua_queue_open), Cvoid, (Ptr{Cvoid}, Cstring), scene, disp)
		@async _bm1_show_3d(scene; xmin=xmin, xmax=xmax, azim=-35.0, elev=20.0, ve=0.15,
		                    expect=disp, pump=false)     # the result comes up in 3-D too
		return disp
	end
	_on_drop(scene, disp)
	_bm1_show_3d(scene; xmin=xmin, xmax=xmax, azim=-35.0, elev=20.0, ve=0.15)
	return disp
end

# Benchs tab, "Run simulation". `keepram` != 0 -> the whole cube is pulled into memory as soon as the
# run's result is open (the box can be ticked before the file it describes exists).
function _on_bench1_run(scene::Ptr{Cvoid}, outfile::AbstractString, keepram::Integer=0)::Cvoid
	_bm1_run_async(scene, String(outfile); keepram = keepram != 0)
	return
end

# Benchs tab, "Load from disk…" (and the "Load it" answer when a run would overwrite one).
function _on_bench1_load(scene::Ptr{Cvoid}, path::AbstractString, keepram::Integer=0)::Cvoid
	disp = _bm1_open_result(scene, String(path))
	if keepram != 0
		@async _bm1_keep_in_ram(scene; wait_for = disp)   # on a task: the open it waits for is queued
	end
	return
end

# How much memory the WHOLE cube at `path` would take if held in RAM, in MB. Before anything has been
# computed the file does not exist yet, so the estimate comes from the model the run will produce:
# the display window's columns × the flume's 51 rows × one Float32 per node per step.
function _bm1_ram_mb(path::String; xmin::Float64=-200.0, xmax::Float64=20000.0, dx::Float64=25.0,
                     ncycles::Int=5000, interval::Int=50)::Float64
	nx, ny, nt = 0, 0, 0
	if isfile(path)
		try
			var = _bm1_var(path)
			for v in _netcdf_subdatasets(path)
				v.name == var && length(v.dims) >= 3 || continue
				nt, ny, nx = Int(v.dims[1]), Int(v.dims[2]), Int(v.dims[3])
			end
		catch                                    # unreadable / not one of ours: fall through to the model
		end
	end
	if nx == 0
		# No file yet: estimate from THE MODEL THE RUN WILL PRODUCE. With nesting that is the FINEST
		# level, which is what nswing writes its output on — the outer flume's own columns would
		# overstate it by an order of magnitude.
		nests = _bm1_nests(; dxOuter=dx)
		if isempty(nests)
			x  = collect(-200.0:dx:50000.0)
			nx = count(v -> xmin <= v <= xmax, x)
			ny = 51
		else
			ny, nx = size(nests[end].z)
		end
		nt = ncycles ÷ interval + 1
	end
	return nx * ny * nt * sizeof(Float32) / (1024.0 * 1024.0)
end

# "Keep whole cube in RAM" for the window's CURRENT file. It is the netCDF tab's own button doing the
# work (`_aqua_load_all` -> the shared `_read_whole_cube` / `_cube_fits_ram`), never a second loader;
# this only waits for a file to be open first, because the box can be ticked before the run that will
# fill it has finished. Returns 0 loaded, 1 would not fit, 2 error/no file.
function _bm1_keep_in_ram(scene::Ptr{Cvoid}; wait_for::String="", timeout::Float64=1800.0)::Cint
	if !isempty(wait_for)
		want = abspath(wait_for)
		t0 = time()
		while true
			st = get(_AQUA, scene, nothing)
			st !== nothing && abspath(st.path) == want && !st.first && break
			(time() - t0) > timeout && return Cint(2)
			sleep(0.1)
			yield()
		end
	end
	return _aqua_load_all(scene)
end

# Benchs tab: the RAM estimate for what the "Save to" box points at, as a plain MB number.
_on_bench1_ram_mb(path::AbstractString)::Float64 = _bm1_ram_mb(String(path))

# ── the analytic solution as the η(x) figure's reference curve ─────────────────────────────────
# The Cinema tab's floating η(x) figure draws the row of the slice ON SCREEN. For a benchmark that
# curve has something to be compared WITH — the Carrier–Greenspan analytic solution
# (`benchmark1_analytic.jl`, the same file that can be run stand-alone) — and the comparison is only
# worth anything if both are the same quantity on the same axes, which is why the reference is a
# second curve in THAT panel and not a plot of its own (ProfilePanel::setSeries2, 60_profile.cpp).
#
# The window asks at every slice (`askEtaReference` -> here); a window that has no reference
# solution answers by clearing it, so the C++ side carries no per-tool knowledge.

# Windows showing benchmark 1 — the tank the menu entry opens and any run/loaded result adopted into
# it. Membership is what says "this window has an analytic solution", never a guess from the file
# name (a run is saved wherever the user pointed the Save box).
const _BM1_SCENES = Set{Ptr{Cvoid}}()

# THE PROFILE IS STITCHED FROM THE NESTING LEVELS, finest first: the run writes one cube per level
# (see `_bm1_run_async`), and the η(x) curve takes each stretch of x from the finest grid that
# resolves it — 1 m in over the beach, 5 m behind it, 25 m out to sea. A 25 m cell puts the whole
# run-up tip inside two columns, which is what made the modelled curve disagree with the analytic
# solution at the last timesteps.
#
# Per scene: the FINER cubes only (level 1, level 2, …). Level 0 is the file the window itself has
# open, and it is read through the window's own state, never a second time from disk.
const _BM1_LEVELS = Dict{Ptr{Cvoid},Vector{String}}()

# Where one level hands over to the next, in metres: level 2 below 600, level 1 up to 1000, level 0
# beyond. The numbers are the nests' own useful spans (level 2 ends at x = 612, level 1 at 1010), kept
# clear of each grid's outer edge where the nesting boundary condition lives.
const _BM1_LEVEL_EDGES = (600.0, 1000.0)

# Note which of a run's per-level cubes are actually on disk. Called wherever a result is adopted, so
# a run and a "Load from disk…" of the same file behave the same; a run made before this existed (or a
# single-level one) simply registers nothing and the figure keeps the window's own curve.
function _bm1_register_levels!(scene::Ptr{Cvoid}, cube::String)
	stem = splitext(abspath(cube))[1]
	levs = String[]
	for i in 1:9
		p = stem * "_lev$(i).nc"
		isfile(p) || break
		push!(levs, p)
	end
	isempty(levs) ? delete!(_BM1_LEVELS, scene) : (_BM1_LEVELS[scene] = levs)
	return levs
end

# The row of a cube layer nearest mid-tank, as (x, z) — the same "middle row" the window's own η(x)
# curve is read along (AquamotoWindow::updateEtaFigure asks for y = (gy0+gy1)/2). `k` is 0-based.
const _BM1_LEVEL_VAR = Dict{String,String}()   # path -> its time-varying variable, found once

function _bm1_level_row(path::String, k::Int)
	var = get!(() -> _bm1_var(path), _BM1_LEVEL_VAR, path)   # netCDF introspection, not once per slice
	G   = _read_cube_layer("$(path)?$(var)", k + 1)
	G === nothing && return (Float64[], Float64[])
	Z = _zmat(G)
	return (Float64.(G.x), Float64.(Z[max(1, size(Z, 1) ÷ 2), :]))
end

# Analytic η at the model times/positions asked for, cached per (t, x0, x1, n): the figure is
# refreshed at every slice and a replay would otherwise recompute the same curve for ever.
const _BM1_ANALYTIC_CACHE = Dict{NTuple{4,Float64},Tuple{Vector{Float64},Vector{Float64}}}()

# η(x) of the analytic solution at model time `t`, sampled on `n` points over [x0, x1] (metres, the
# flume's own frame: x = 0 is the initial shoreline, positive offshore — the same frame the model
# grid and the tabulated `_BM1_ANALYTIC` use). Points the solution does not reach (dry beach beyond
# the run-up) come back dropped, not as NaN, so the panel never draws a break.
function _bm1_analytic_curve(t::Float64, x0::Float64, x1::Float64, n::Int)
	key = (t, x0, x1, Float64(n))
	hit = get(_BM1_ANALYTIC_CACHE, key, nothing)
	hit === nothing || return hit
	xq = collect(range(x0, x1; length = max(n, 2)))
	R  = CatalinaBenchmark1.catalina1(t; xmax = max(x1, 1000.0) * 1.05, npoints = clamp(n, 400, 2000))
	# `_bm1_interp1` is this file's ONE 1-D sampler (GMT's own sample1d) — the analytic curve is put
	# on the figure's x the same way the initial condition is put on the flume's.
	eta  = _bm1_interp1(R.x, R.eta, xq; interp = :linear)
	keep = .!isnan.(eta)
	out  = (xq[keep], eta[keep])
	length(_BM1_ANALYTIC_CACHE) > 512 && empty!(_BM1_ANALYTIC_CACHE)
	_BM1_ANALYTIC_CACHE[key] = out
	return out
end

# The MODEL profile over [x0, x1], STITCHED from the run's nesting levels: each stretch of x comes
# from the finest cube that resolves it (level 2 below 600 m, level 1 to 1000 m, level 0 beyond), all
# at the same slice `k` (0-based), all along the middle row. Empty when this window has no per-level
# cubes — then the figure keeps the curve it read off the displayed grid.
function _bm1_stitch_curve(levs::Vector{String}, k::Int, x0::Float64, x1::Float64,
                           base::Tuple{Vector{Float64},Vector{Float64}})
	isempty(levs) && return (Float64[], Float64[])
	# Finest first: level N up to the first edge, … , level 0 from the last edge outwards. The edges
	# list is one shorter than the number of grids, so a run with one nest uses just the first edge.
	edges = collect(_BM1_LEVEL_EDGES)[1:min(length(levs), length(_BM1_LEVEL_EDGES))]
	xs, ys = Float64[], Float64[]
	lo = -Inf
	for (i, path) in enumerate(Iterators.reverse(levs))          # levs is [lev1, lev2, …] -> finest first
		hi = edges[i]                                            # finest ends at the first edge (600 m)
		gx, gz = _bm1_level_row(path, k)
		m = (gx .>= max(lo, x0)) .& (gx .< min(hi, x1))
		append!(xs, gx[m]);  append!(ys, gz[m])
		lo = hi
	end
	gx, gz = base                                                # the outermost stretch, level 0
	m = (gx .>= max(lo, x0)) .& (gx .<= x1)
	append!(xs, gx[m]);  append!(ys, gz[m])
	o = sortperm(xs)
	return (xs[o], ys[o])
end

# The same profile for a live window: level 0 comes from the window's OWN layer, read through the
# state so a RAM-resident cube is not read off disk a second time (`_aqua_layer`).
function _bm1_model_curve(scene::Ptr{Cvoid}, st::_AquaState, k::Int, x0::Float64, x1::Float64)
	levs = get(_BM1_LEVELS, scene, String[])
	isempty(levs) && return (Float64[], Float64[])
	G = _aqua_layer(st, k)
	base = if G === nothing
		(Float64[], Float64[])
	else
		Z = _zmat(G)
		(Float64.(G.x), Float64.(Z[max(1, size(Z, 1) ÷ 2), :]))
	end
	return _bm1_stitch_curve(levs, k, x0, x1, base)
end

# What the Aquamoto window calls after every slice: hand it the curves the η(x) figure is to draw over
# the x window it is drawing — the stitched model profile (empty = keep the window's own) and the
# analytic reference. `n` is the number of samples the figure asked for.
# `x0`/`x1`/`n` arrive as literals from the dialog's own call string, so an integral value ("-200")
# reaches this as an Int — the arguments are Reals, converted here, never a Float64-only signature
# that a whole number misses.
function _aqua_eta_curves(scene::Ptr{Cvoid}, x0::Real, x1::Real, n::Real)::Cvoid
	push(xm, ym, xr, yr, name) =
		GC.@preserve xm ym xr yr ccall(_fn(:gmtvtk_aqua_set_eta_curves_h), Cvoid,
		      (Ptr{Cvoid}, Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cdouble}, Ptr{Cdouble}, Cint, Cstring),
		      scene, isempty(xm) ? C_NULL : pointer(xm), isempty(ym) ? C_NULL : pointer(ym), Cint(length(xm)),
		             isempty(xr) ? C_NULL : pointer(xr), isempty(yr) ? C_NULL : pointer(yr), Cint(length(xr)),
		      name)
	clear() = push(Float64[], Float64[], Float64[], Float64[], "")
	try
		(scene in _BM1_SCENES) || return clear()
		st = get(_AQUA, scene, nothing)
		st === nothing && return clear()
		(0 <= st.cur < st.nsteps) || return clear()
		# The MODEL TIME of the slice on screen — the cube's own time coordinate, the same value the
		# titlebar shows. A cube with no time axis has nothing to evaluate an analytic solution at.
		st.cur + 1 <= length(st.times) || return clear()
		t = st.times[st.cur+1]
		(isfinite(t) && t >= 0) || return clear()
		xa, ya = _bm1_analytic_curve(Float64(t), Float64(x0), Float64(x1), round(Int, n))
		xm, ym = _bm1_model_curve(scene, st, st.cur, Float64(x0), Float64(x1))
		length(xa) < 2 && (xa = Float64[]; ya = Float64[])
		length(xm) < 2 && (xm = Float64[]; ym = Float64[])
		push(xm, ym, xa, ya, "analytic")
	catch e
		@tool_error "Catalina benchmark 1: the η(x) curves failed" exception=(e,)
		clear()
	end
	return
end
