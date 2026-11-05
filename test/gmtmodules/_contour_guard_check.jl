# Does GDALContourGenerateEx call the progress callback, and stop when it returns 0? And what does
# the guard cost? julia _contour_guard_check.jl
using InteractiveGMT, GMT
const IG = InteractiveGMT
function grid(n)
	x = collect(range(0, 20, length = n))
	z = Float32[sin(xi) * cos(yj) + 0.3f0 * sin(3.7xi + 1.3yj) + 0.05f0 * sin(17xi) * cos(19yj) for yj in x, xi in x]
	return mat2grid(z, x = x, y = x)
end
const CALLS = Ref(0)
cb_count(::Cdouble, ::Cstring, ::Ptr{Cvoid})::Cint = (CALLS[] += 1; Cint(1))
cb_stop(::Cdouble, ::Cstring, ::Ptr{Cvoid})::Cint = (CALLS[] += 1; Cint(CALLS[] < 5))
function trace(G, lev, cb)
	gdl = GMT.Gdal
	dsr = GMT.gmt2gd(G)
	band = ccall((:GDALGetRasterBand, GMT.libgdal), Ptr{Cvoid}, (Ptr{Cvoid}, Cint), dsr.ptr, Cint(1))
	ds = gdl.create(gdl.getdriver("MEM"))
	lyr = gdl.createlayer(name = "contour", dataset = ds, geom = gdl.wkbLineString)
	gdl.addfielddefn!(lyr, "ID", gdl.OFTInteger); gdl.addfielddefn!(lyr, "ELEV", gdl.OFTReal)
	copts = C_NULL
	for o in ["ID_FIELD=0", "ELEV_FIELD=1", "FIXED_LEVELS=" * join(lev, ',')]
		copts = ccall((:CSLAddString, GMT.libgdal), Ptr{Ptr{UInt8}}, (Ptr{Ptr{UInt8}}, Cstring), copts, o)
	end
	return GC.@preserve dsr ds ccall((:GDALContourGenerateEx, GMT.libgdal), Cint,
	      (Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Ptr{UInt8}}, Ptr{Cvoid}, Ptr{Cvoid}), band, lyr.ptr, copts, cb, C_NULL)
end
G = grid(2000);  lev = collect(range(-1.3, 1.3, length = 100))
CALLS[] = 0;  e = trace(G, lev, @cfunction(cb_count, Cint, (Cdouble, Cstring, Ptr{Cvoid})))
println("count: err=$e calls=$(CALLS[])")
CALLS[] = 0;  e = trace(G, lev, @cfunction(cb_stop, Cint, (Cdouble, Cstring, Ptr{Cvoid})))
println("stop:  err=$e calls=$(CALLS[])")
# the real guard, end to end, timed against no callback at all
IG._contour_gdal(grid(200), lev, 0)
for k in 1:3
	t0 = time();  trace(G, lev, C_NULL);  t1 = time()
	IG._contour_gdal(G, lev, 0);  t2 = time()
	GC.gc()
	println("no callback: $(round(t1 - t0, digits = 3)) s   _contour_gdal (guarded, plus its copy): $(round(t2 - t1, digits = 3)) s")
end
t0 = time();  for _ in 1:2000; IG._contour_progress(0.5, reinterpret(Cstring, C_NULL), C_NULL); end
println("callback, 2000 calls inside the 0.1 s window: $(round((time() - t0) * 1e6 / 2000, digits = 2)) us each")
t0 = time();  for _ in 1:2000; IG._mem_available(); end
println("_mem_available: $(round((time() - t0) * 1e6 / 2000, digits = 2)) us each")
# A trace whose memory the guard thinks is already used up refuses
IG._CONTOUR_MEM.avail0[] = IG._mem_available() + 2^44;  IG._CONTOUR_MEM.next[] = 0.0
println("guard verdict with 16 TiB 'already traced': ", IG._contour_progress(0.5, reinterpret(Cstring, C_NULL), C_NULL))
