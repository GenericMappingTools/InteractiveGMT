# Figure handles + the live-figure registry. Each viewer call returns one of these opaque
# handles (each wraps a C `Scene*`). The registry lets the in-window Julia console bind `fig`
# to the window a command was typed in (see console.jl).

# Maps a window's opaque C handle (Scene*) -> its Julia figure object.
const _FIGREG = Dict{Ptr{Cvoid},Any}()
_register_fig!(fig) = (_FIGREG[getfield(fig, :h)] = fig; fig)

# A live grid viewer window. `h` is the opaque C handle (a Scene*); `G` is the grid it shows,
# kept so add!/add_curtain! can drape (x,y)-only data on the same surface.
struct QtFigure
	h::Ptr{Cvoid}
	G::GMTgrid
end

# A live point-cloud viewer window. `D` is the point table (x y z [...]), kept so
# `selection(fig)` can return the picked rows.
struct QtPoints
	h::Ptr{Cvoid}
	D::Matrix{Float64}
end

# A live GMTfv (solid / polygon mesh) viewer window. `fv` is the GMTfv it shows (overlays /
# curtains drape on a grid, not on an fv — kept for reference only).
struct QtFV
	h::Ptr{Cvoid}
	fv::GMTfv
end

# A live bare-image viewer window (no elevation): a flat plane textured with the image, opened
# top-down. `I` is the GMTimage it shows. The status-bar readout shows the pixel colour, not z.
struct QtImage
	h::Ptr{Cvoid}
	I::GMTimage
end

# A live EMPTY viewer window (a drag-and-drop launcher): drop a grid/image/table file onto it
# (or any window) to open it in a new window. Holds only the opaque C handle.
struct QtEmpty
	h::Ptr{Cvoid}
end

# A live X,Y plot window (the standalone 2-D plotter, the evolution of the Profile). `h` is the
# opaque C handle (an XYPlot*, NOT a Scene*); it has its OWN is_alive/close C API. `series` keeps a
# Julia-side copy of every line (x, y, name) added, so File > Save can write it back (the C side
# owns the vtkTables; Julia keeps the authoritative data it produced).
mutable struct QtXYPlot
	h::Ptr{Cvoid}
	series::Vector{Tuple{Vector{Float64},Vector{Float64},String}}
end
QtXYPlot(h::Ptr{Cvoid}) = QtXYPlot(h, Tuple{Vector{Float64},Vector{Float64},String}[])

"True while the figure's window is still open (closing it invalidates the handle)."
isalive(f::QtFigure)::Bool = ccall(_fn(:gmtvtk_is_alive), Cint, (Ptr{Cvoid},), f.h) != 0
isalive(f::QtPoints)::Bool = ccall(_fn(:gmtvtk_is_alive), Cint, (Ptr{Cvoid},), f.h) != 0
isalive(f::QtFV)::Bool     = ccall(_fn(:gmtvtk_is_alive), Cint, (Ptr{Cvoid},), f.h) != 0
isalive(f::QtImage)::Bool  = ccall(_fn(:gmtvtk_is_alive), Cint, (Ptr{Cvoid},), f.h) != 0
isalive(f::QtEmpty)::Bool  = ccall(_fn(:gmtvtk_is_alive), Cint, (Ptr{Cvoid},), f.h) != 0
isalive(f::QtXYPlot)::Bool = ccall(_fn(:gmtvtk_xyplot_is_alive), Cint, (Ptr{Cvoid},), f.h) != 0

# Opaque C handle of any figure type (QtFigure / QtPoints / QtFV).
_fig_handle(fig)::Ptr{Cvoid} = getfield(fig, :h)
