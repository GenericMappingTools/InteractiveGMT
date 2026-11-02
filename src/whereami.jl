# T & L > Earth-Moon system, "Moon from a place": the user's own place, from GMT.jl's whereami().

# C callback (74_eclipse.cpp): out[1..2] = lon, lat; "City, Country" to `name`. 1, or 0 when not found.
function _on_whereami(out::Ptr{Cdouble}, name::Ptr{UInt8}, cap::Cint)::Cint
	try
		D = GMT.whereami()
		isempty(D.data) && return Cint(0)
		unsafe_store!(out, Float64(D.data[1, 1]), 1)
		unsafe_store!(out, Float64(D.data[1, 2]), 2)
		where = join(filter(!isempty, [get(D.attrib, "City", ""), get(D.attrib, "Country", "")]), ", ")
		nb = _console_write(name, cap, where)
		cap > 0 && unsafe_store!(name, 0x00, Int(nb) + 1)
		return Cint(1)
	catch
		return Cint(0)
	end
end

# NOTHING OF THIS RUNS AT START-UP: the menu entry fires warmupTool("whereami") and waits for the callback.
const _WHEREAMI_WIRED = Ref(false)
function _register_whereami()
	_WHEREAMI_WIRED[] && return
	haskey(_LIB_FNS, :gmtvtk_set_whereami_callback) || return
	f = @cfunction((o, n, c) -> Base.invokelatest(_on_whereami, o, n, c), Cint, (Ptr{Cdouble}, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_whereami_callback), Cvoid, (Ptr{Cvoid},), f)
	_WHEREAMI_WIRED[] = true
	return
end
warm_register("whereami", _register_whereami)
