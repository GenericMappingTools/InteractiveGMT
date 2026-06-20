# Read-only scene introspection for the test suite. `gmtvtk_scene_state` (90_c_api.cpp) serializes a
# window's scene state to a semicolon-separated key=value string; here we parse it into a Dict so
# tests can assert the invariants that regressions keep breaking (has_surface, emptyStart, imageOnly,
# flat2d, axes-in-renderer == coord grid present, region, object counts, each extra's kind+name).
# NOTHING here mutates the scene — it is a pure snapshot, safe to call at any time on a live handle.

# Parse the raw "k=v;k=v;extra0=image:Base image (global);..." dump. Numeric values become Int/Float64,
# `surf_name` stays a String, and the extraN tokens collapse into extras::Vector{(kind,name)}. Pure
# string work — no DLL — so it is unit-tested directly against known dumps (the harness-correctness layer).
function _parse_scene_state(str::AbstractString)
	d = Dict{String,Any}()
	extras = Tuple{String,String}[]
	for tok in split(str, ';'; keepempty=false)
		eq = findfirst('=', tok)
		eq === nothing && continue
		k = tok[1:eq-1]; v = tok[eq+1:end]
		if startswith(k, "extra") && all(isdigit, k[6:end])
			c = findfirst(':', v)
			c === nothing ? push!(extras, ("", String(v))) : push!(extras, (String(v[1:c-1]), String(v[c+1:end])))
		elseif k == "surf_name"
			d[k] = String(v)
		else
			iv = tryparse(Int, v)
			d[k] = iv !== nothing ? iv : (tryparse(Float64, v) !== nothing ? parse(Float64, v) : String(v))
		end
	end
	d["extras"] = extras
	return d
end

# Snapshot a live window's scene state -> Dict. Accepts a raw Scene* handle or any Qt* figure (.h).
# Two-pass: first call sizes the buffer (returns the full length), second fills it.
function _scene_state(h::Ptr{Cvoid})
	n = ccall(_fn(:gmtvtk_scene_state), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, C_NULL, Cint(0))
	buf = Vector{UInt8}(undef, n + 1)
	ccall(_fn(:gmtvtk_scene_state), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, buf, Cint(n + 1))
	return _parse_scene_state(unsafe_string(pointer(buf)))
end
_scene_state(fig) = _scene_state(getfield(fig, :h))

# Pump the Qt event loop once (flush pending UI work before snapshotting), best-effort.
_pump_once() = ccall(_fn(:gmtvtk_process_events), Cint, ())
