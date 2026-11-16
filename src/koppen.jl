# koppen.jl — Geography > Köppen-Geiger climate: the climate classes of Beck et al. (2023) for one
# historical period, over the window's view, as an INDEXED image wearing the dataset's own colours.
#
# Data: "High-resolution (1 km) Köppen-Geiger maps for 1901–2099 based on constrained CMIP6
# projections", figshare 10.6084/m9.figshare.21789074 (v2), CC BY 4.0. Beck, H. E. et al., Scientific
# Data 10, 724 (2023). Shipped in data/koppen: the 0.1 degree GeoTIFF of each of the four historical
# periods (~215 KB each) and the dataset's legend.txt, taken unchanged from koppen_geiger_tif.zip.
#
# Pixel values: 0 = sea / no data, 1..30 = the classes below (legend.txt, RGB verbatim).

const _KG_CLASSES = (
	("Af",  "Tropical, rainforest",                  (0, 0, 255)),
	("Am",  "Tropical, monsoon",                     (0, 120, 255)),
	("Aw",  "Tropical, savannah",                    (70, 170, 250)),
	("BWh", "Arid, desert, hot",                     (255, 0, 0)),
	("BWk", "Arid, desert, cold",                    (255, 150, 150)),
	("BSh", "Arid, steppe, hot",                     (245, 165, 0)),
	("BSk", "Arid, steppe, cold",                    (255, 220, 100)),
	("Csa", "Temperate, dry summer, hot summer",     (255, 255, 0)),
	("Csb", "Temperate, dry summer, warm summer",    (200, 200, 0)),
	("Csc", "Temperate, dry summer, cold summer",    (150, 150, 0)),
	("Cwa", "Temperate, dry winter, hot summer",     (150, 255, 150)),
	("Cwb", "Temperate, dry winter, warm summer",    (100, 200, 100)),
	("Cwc", "Temperate, dry winter, cold summer",    (50, 150, 50)),
	("Cfa", "Temperate, no dry season, hot summer",  (200, 255, 80)),
	("Cfb", "Temperate, no dry season, warm summer", (100, 255, 80)),
	("Cfc", "Temperate, no dry season, cold summer", (50, 200, 0)),
	("Dsa", "Cold, dry summer, hot summer",          (255, 0, 255)),
	("Dsb", "Cold, dry summer, warm summer",         (200, 0, 200)),
	("Dsc", "Cold, dry summer, cold summer",         (150, 50, 150)),
	("Dsd", "Cold, dry summer, very cold winter",    (150, 100, 150)),
	("Dwa", "Cold, dry winter, hot summer",          (170, 175, 255)),
	("Dwb", "Cold, dry winter, warm summer",         (90, 120, 220)),
	("Dwc", "Cold, dry winter, cold summer",         (75, 80, 180)),
	("Dwd", "Cold, dry winter, very cold winter",    (50, 0, 135)),
	("Dfa", "Cold, no dry season, hot summer",       (0, 255, 255)),
	("Dfb", "Cold, no dry season, warm summer",      (55, 200, 255)),
	("Dfc", "Cold, no dry season, cold summer",      (0, 125, 125)),
	("Dfd", "Cold, no dry season, very cold winter", (0, 70, 95)),
	("ET",  "Polar, tundra",                         (178, 178, 178)),
	("EF",  "Polar, frost",                          (102, 102, 102)),
)

const _KG_PERIODS = ("1901_1930", "1931_1960", "1961_1990", "1991_2020")

_kg_file(period::AbstractString) = joinpath(_PKGROOT, "data", "koppen", "koppen_geiger_$(period)_0p1.tif")

# The classes as an indexed image of the whole Earth: value 0 (sea) transparent, 1..30 the legend
# colours.
function _kg_image(period::AbstractString)::GMTimage
	I = GMT.gdaltranslate(_kg_file(period), String[])
	L = zeros(UInt8, length(_KG_CLASSES) + 1, 4)          # row 1 = value 0: the sea, alpha 0
	for (k, c) in enumerate(_KG_CLASSES)
		L[k + 1, 1], L[k + 1, 2], L[k + 1, 3] = UInt8.(c[3]);  L[k + 1, 4] = 0xff
	end
	_img_set_palette!(I, L)
	I.labels = ["Sea"; [c[1] for c in _KG_CLASSES]]       # value k's name = labels[k+1] (the export's bar)
	return I
end

# Geography menu leaf: `period` as the data file names it ("1991_2020"). ALWAYS the whole Earth, never
# cut to the window's view: the map arrives as a new raster with its own axes, and a view cut showed
# NOTHING whenever the view was not land in lon/lat (an empty window's 0..1 scaffold, an x/y grid's
# -3..3, any patch of sea) -- the region the menu sends is ignored.
function _kg_plot(scene::Ptr{Cvoid}, period::AbstractString, W, E, S, N)
	period in _KG_PERIODS || error("Köppen-Geiger: unknown period '$period'")
	I = _kg_image(period)
	name = "Köppen-Geiger " * replace(period, '_' => '-')
	_place_image_in_window(scene, I, name; geographic = true)
	# Hovering names the class: the readout gets the pixel values (the class numbers) and the names of
	# those numbers -- the code on the colour bar, "code — description" in the readout.
	_push_image_dn(scene, name, I)
	lines = ["Sea\tsea / no data"; ["$(c[1])\t$(c[2])" for c in _KG_CLASSES]]
	ccall(_fn(:gmtvtk_image_set_class_names_h), Cvoid, (Ptr{Cvoid}, Cstring, Cstring), scene, name, join(lines, '\n'))
	# The placement fits the camera and THEN opens the Scene Objects panel, which narrows the view: let
	# the panel take its room, then fit again, or the map runs off both sides with no Y axis.
	ccall(_fn(:gmtvtk_process_events), Cint, ())
	ccall(_fn(:gmtvtk_refit_view_h), Cvoid, (Ptr{Cvoid},), scene)
	_viewer_log_info(scene, "$name: Beck et al. (2023), Scientific Data 10, 724 — CC BY 4.0. Classes: " *
		join(("$k=$(c[1])" for (k, c) in enumerate(_KG_CLASSES)), ", "))
	return nothing
end
