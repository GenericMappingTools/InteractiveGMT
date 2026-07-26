# manual.jl — the "Open full manual page" button every module dialog carries (the little green ?
# disk in its lower-left corner, addManualButton in 70_window.cpp). Opens that module's page of the
# GMT.jl manual at https://www.generic-mapping-tools.org/GMTjl_doc/.

# GMT.jl's own `@? name` is NOT used, deliberately: it starts with `getfield(Main, Symbol(name))`, so
# it only works for modules that have a verbose-syntax wrapper bound in Main. `@? grdgravmag3d` opens
# the page, `@? grdredpol` just prints an error, because grdredpol is a supplement GMT.jl has not
# ported and therefore has no binding — yet its manual page exists all the same. The URL is built
# here directly, from the module name, so a page opens for EVERY module a dialog can front, ported or
# not. `display_file` (GMT.jl) is still the opener, so the browser handling stays identical to `@?`.
const _MANUAL_BASE = "https://www.generic-mapping-tools.org/GMTjl_doc/documentation/modules/"

# C callback: `name` is the GMT module name (e.g. "grdredpol"). Returns Cint 1 if the page was
# handed to the browser, 0 on failure — the dialog reports a failure on itself.
function _on_open_manual(cname::Cstring)::Cint
	try
		name = unsafe_string(cname)
		isempty(name) && return Cint(0)
		GMT.display_file(_MANUAL_BASE * name * ".html")
		return Cint(1)
	catch e
		@warn "Open manual page FAILED" exception=(e,)
		return Cint(0)
	end
end

function _register_manual()
	fptr = @cfunction((c) -> Base.invokelatest(_on_open_manual, c)::Cint, Cint, (Cstring,))
	ccall(_fn(:gmtvtk_set_manual_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
