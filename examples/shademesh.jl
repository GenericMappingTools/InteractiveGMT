# Drape smaller than the grid; exercises outside=:shademesh (uncovered area = grey fill + mesh).
using InteractiveGMT
using GMT

# Grid LARGER than the drape image so there is an UNCOVERED area to render as mesh.
nx, ny = 200, 160
# gentle relief so mesh edges are visible in 3-D (not a flat sheet)
xs = range(0, nx, length=nx); ys = range(0, ny, length=ny)
Z  = Float32[20f0*sin(x/30)*cos(y/25) for y in ys, x in xs]   # ny x nx (row=y)
G  = mat2grid(Z; x=[0.0, nx], y=[0.0, ny])

# Image covers only the central third of the grid -> outer ring stays uncovered.
iw, ih = 80, 64
img = fill(UInt8(30), ih, iw, 3)
box!(a, r0, r1, c0, c1, rgb) = (for r in r0:r1, c in c0:c1; a[r,c,1]=rgb[1]; a[r,c,2]=rgb[2]; a[r,c,3]=rgb[3]; end)
box!(img, 8, 56, 16, 24, (255, 80, 80))      # red stem
box!(img, 8, 16, 16, 52, (80, 255, 80))      # green bar
I = mat2img(img; x=[60.0, 140.0], y=[48.0, 112.0])   # central footprint inside the grid

println("grid region=", G.range[1:4], "  image region=", I.range[1:4])

# :shademesh (default) — uncovered ring should be grey fill + visible mesh edges.
fig = view_grid(G; drape=I, geographic=false, outside=:shademesh, outside_color=200,
                title="shademesh: uncovered = mesh")

if abspath(PROGRAM_FILE) == @__FILE__
    sleep(2)
    println("save_png -> ", save_png("shademesh_test.png"))
    wait_windows()
end
