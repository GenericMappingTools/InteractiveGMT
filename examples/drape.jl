# Image-drape orientation test: a chiral "F" marker draped on a flat grid reveals any flip/mirror.
using InteractiveGMT
using GMT

# Flat grid so the texture shows undistorted, top-down-ish view.
nx, ny = 200, 160
G = mat2grid(zeros(Float32, ny, nx); x=[0.0, nx], y=[0.0, ny])

# Marker image: an "F" shape (clearly chiral). Build in plain image space: row 1 = TOP (north),
# col 1 = LEFT (west).
img = fill(UInt8(30), ny, nx, 3)                 # dark background
function box!(img, r0, r1, c0, c1, rgb)
    for r in r0:r1, c in c0:c1
        img[r, c, 1] = rgb[1]; img[r, c, 2] = rgb[2]; img[r, c, 3] = rgb[3]
    end
end
# "F": vertical stem on the left, two horizontal bars near the top.
box!(img, 20, 140, 40, 60, (255, 80, 80))        # stem (red)
box!(img, 20, 40, 40, 130, (80, 255, 80))        # top bar (green)
box!(img, 70, 88, 40, 110, (80, 120, 255))       # mid bar (blue)
I = mat2img(img; x=[0.0, nx], y=[0.0, ny])
# Force a ROW-MAJOR ([lon,lat]) image to exercise the layout-aware packer (gdal/grdimage products
# are typically row-major). permutedims so S[lon,lat,b] == img[lat,lon,b].
I.image  = permutedims(img, (2, 1, 3))
I.layout = "TRBa"
println("layout=", I.layout, "  size=", size(I.image))

fig = view_grid(G; drape=I, geographic=false, title="drape F test (row-major)")

if abspath(PROGRAM_FILE) == @__FILE__
    sleep(2)                                     # let the Qt pump render a frame
    println("save_png -> ", save_png("drape_test.png"))
    wait_windows()
end
