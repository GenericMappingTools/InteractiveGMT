# gmtjl_strip.jl -- GMT.jl as the iGMT Windows installer (deps\installer) sets it up: a git clone in
# <depot>\dev\GMT whose Project.toml has the JLLs below taken out, because the bundled GMT supplies
# those libraries. Pkg then never downloads nor precompiles them, `] up` included (it reads a dev'd
# package's dependencies from its own Project.toml). Used by update!() (selfupdate.jl) and, as a
# plain file, by the installer's make_env.jl and setup.jl. Plain text parsing: no TOML dependency.
#
# Such a depot holds the file <depot>\igmt_gmtjl.txt (written by setup.jl) with the clone's path.
# Without it -- a registry GMT.jl, a user's own `] dev` GMT.jl -- nothing here is ever applied.

const _GMTJL_DROP   = ("GMT_jll", "GDAL_jll", "PROJ_jll", "LASzip_jll", "Leptonica_jll")
const _GMTJL_MARKER = "igmt_gmtjl.txt"

# The [deps] names of the Project.toml at `pf`.
function _gmtjl_project_deps(pf::AbstractString)
	sect, names = "", String[]
	for l in readlines(pf)
		m = match(r"^\s*\[([^\]]+)\]", l)
		m !== nothing && (sect = strip(m.captures[1]); continue)
		sect == "deps" || continue
		m = match(r"^\s*([A-Za-z0-9_]+)\s*=", l)
		m === nothing || push!(names, m.captures[1])
	end
	return names
end

# Removes _GMTJL_DROP from [deps] and [compat] of the Project.toml at `pf`, line by line: the rest of
# the file stays as it is. Returns the names removed from [deps].
function _strip_gmtjl_project!(pf::AbstractString)
	sect, gone = "", String[]
	lines = filter(readlines(pf)) do l
		m = match(r"^\s*\[([^\]]+)\]", l)
		m !== nothing && (sect = strip(m.captures[1]); return true)
		sect in ("deps", "compat") || return true
		m = match(r"^\s*([A-Za-z0-9_]+)\s*=", l)
		(m !== nothing && m.captures[1] in _GMTJL_DROP) || return true
		sect == "deps" && push!(gone, m.captures[1])
		return false
	end
	write(pf, join(lines, "\n") * "\n")
	return gone
end

# GMT.jl's deps\deps.jl names the libraries with '/' (from `gmt --show-library`). Windows looks for a
# DLL's own dependencies next to it only when its path uses '\': with '/', gmt_w64.dll took
# postscriptlight_w64.dll from whatever other GMT is on PATH ("entry point PSL_... not found").
function _backslash_depsjl!(f::AbstractString)
	write(f, join(map(l -> occursin(r"^_lib\w*\s*=", l) ? replace(l, "/" => "\\\\") : l, readlines(f)), "\n") * "\n")
end
