using InteractiveGMT
using Documenter

DocMeta.setdocmeta!(InteractiveGMT, :DocTestSetup, :(using InteractiveGMT); recursive = true)

# Add titles of sections and overrides page titles
const titles = Dict(
    "00-vision.md" => "Vision",
    "01-getting-started.md" => "Getting Started",
    "05-preferences.md" => "Preferences",
    "10-grid-viewer.md" => "Grid Viewer",
    "15-shading.md" => "Shading Dock",
    "20-point-clouds.md" => "Point Clouds",
    "30-solids.md" => "Solids and Meshes",
    "40-xyplot.md" => "X,Y Plot Tool",
    "41-xyplot-plot.md" => "X,Y Plot: plot",
    "41-xyplot-errorbar.md" => "X,Y Plot: errorbar",
    "41-xyplot-bubblechart.md" => "X,Y Plot: bubblechart",
    "41-xyplot-feather.md" => "X,Y Plot: feather",
    "41-xyplot-polarplot.md" => "X,Y Plot: polarplot",
    "41-xyplot-polarscatter.md" => "X,Y Plot: polarscatter",
    "41-xyplot-compassplot.md" => "X,Y Plot: compassplot",
    "41-xyplot-piechart.md" => "X,Y Plot: piechart",
    "41-xyplot-donutchart.md" => "X,Y Plot: donutchart",
    "41-xyplot-area.md" => "X,Y Plot: area",
    "41-xyplot-bar-stack.md" => "X,Y Plot: bar stack",
    "41-xyplot-bar.md" => "X,Y Plot: bar",
    "41-xyplot-barh-stack.md" => "X,Y Plot: barh stack",
    "41-xyplot-barh.md" => "X,Y Plot: barh",
    "41-xyplot-biplot.md" => "X,Y Plot: biplot",
    "41-xyplot-boxplot.md" => "X,Y Plot: boxplot",
    "41-xyplot-comet.md" => "X,Y Plot: comet",
    "41-xyplot-cornerplot.md" => "X,Y Plot: cornerplot",
    "41-xyplot-density.md" => "X,Y Plot: density",
    "41-xyplot-ecdfplot.md" => "X,Y Plot: ecdfplot",
    "41-xyplot-fill_between.md" => "X,Y Plot: fill_between",
    "41-xyplot-histogram.md" => "X,Y Plot: histogram",
    "41-xyplot-loglog.md" => "X,Y Plot: loglog",
    "41-xyplot-marginalhist.md" => "X,Y Plot: marginalhist",
    "41-xyplot-parallelplot.md" => "X,Y Plot: parallelplot",
    "41-xyplot-plotyy.md" => "X,Y Plot: plotyy",
    "41-xyplot-qqnorm.md" => "X,Y Plot: qqnorm",
    "41-xyplot-qqplot.md" => "X,Y Plot: qqplot",
    "41-xyplot-radar.md" => "X,Y Plot: radar",
    "41-xyplot-rose.md" => "X,Y Plot: rose",
    "41-xyplot-scatter.md" => "X,Y Plot: scatter",
    "41-xyplot-semilogx.md" => "X,Y Plot: semilogx",
    "41-xyplot-semilogy.md" => "X,Y Plot: semilogy",
    "41-xyplot-stairs.md" => "X,Y Plot: stairs",
    "41-xyplot-stem.md" => "X,Y Plot: stem",
    "41-xyplot-violin.md" => "X,Y Plot: violin",
    "41-xyplot-waterfall.md" => "X,Y Plot: waterfall",
    "50-utilities.md" => "Utilities",
    "60-geography.md" => "Geography Tools",
    "62-elastic-deformation.md" => "Tutorial: Elastic Deformation",
    "64-nested-grids.md" => "Tutorial: Nested Grids",
    "66-nswing.md" => "Tutorial: Running NSWING",
    "66b-tide-gauges.md" => "Tutorial: Virtual Tide Gauges",
    "66c-travel-times.md" => "Tutorial: Tsunami Travel Times",
    "67-moho-spada.md" => "Tutorial: Moho Topography",
    "68-gps-sanchez.md" => "Tutorial: GPS Velocities of the Alps",
    "69-jura.md" => "Tutorial: Jura Mountains 3-D Model",
    "69b-alpine-data.md" => "Tutorial: Alpine Data Integration",
    "69c-lapalma.md" => "Tutorial: La Palma Volcano",
    "69d-visualise.md" => "Tutorial: Slicing a Tomography Model",
    "69e-netcdf-tomography.md" => "Tutorial: A Tomography Model from netCDF",
    "69f-ascii-tomography.md" => "Tutorial: A Tomography Model from ASCII",
    "70-tools.md" => "Tools",
    "71-earthregions.md" => "Earth Regions",
    "71b-emodnet.md" => "EMODNET",
    "72-ecmwf.md" => "Copernicus / ECMWF",
    "73-sentinelhub.md" => "Sentinel Hub",
    "74-modis-scenes.md" => "MODIS Scenes (Terra / Aqua)",
    "74b-swot.md" => "SWOT",
    "74c-swot-tutorial.md" => "Tutorial: SWOT",
    "75-mbgrid.md" => "mbgrid",
    "76-cube.md" => "CUBE",
    "77-mbplugin.md" => "MB-System plugin (Linux, macOS)",
    "78-mbprocess.md" => "mbprocess",
    "79-mbnavadjust.md" => "MBnavadjust",
    "80-benchmark1.md" => "Catalina Benchmark 1",
    "81-gmtsarplugin.md" => "GMTSAR plugin",
    "90-user-manual.md" => "User Manual",
    "95-reference.md" => "API Reference",
    "96-recipes.md" => "Recipes (experimental)",
    "hillshade.md" => "Hillshade",
)

function recursively_list_pages(folder; path_prefix="")
    pages_list = Any[]
    for file in readdir(folder)
        if file == "index.md"
            # We add index.md separately to make sure it is the first in the list
            continue
        end
        # this is the relative path according to our prefix, not @__DIR__, i.e., relative to `src`
        relpath = joinpath(path_prefix, file)
        # full path of the file
        fullpath = joinpath(folder, relpath)

        if isdir(fullpath)
            # If this is a folder, enter the recursion case
            subsection = recursively_list_pages(fullpath; path_prefix=relpath)

            # Ignore empty folders
            if length(subsection) > 0
                title = if haskey(titles, relpath)
                titles[relpath]
                else
                @error "Bad usage: '$relpath' does not have a title set. Fix in 'docs/make.jl'"
                relpath
                end
                push!(pages_list, title => subsection)
            end

            continue
        end

        if splitext(file)[2] != ".md" # non .md files are ignored
            continue
        elseif haskey(titles, relpath) # case 'title => path'
            push!(pages_list, titles[relpath] => relpath)
        else # case 'title'
            push!(pages_list, relpath)
        end
    end

    return pages_list
end

function list_pages()
    root_dir = joinpath(@__DIR__, "src")
    pages_list = recursively_list_pages(root_dir)

    return ["index.md"; pages_list]
end

makedocs(;
    modules = [InteractiveGMT],
    checkdocs = :none,      # API reference (95-reference.md) is hand-curated -- don't require every docstring to appear
    authors = "Joaquim Luis <jluis@ualg.pt>",
    repo = Documenter.Remotes.GitHub("GenericMappingTools", "InteractiveGMT"),
    sitename = "InteractiveGMT.jl",
    # The API reference is one long page: allow it past Documenter's 200 KiB default
    format = Documenter.HTML(; canonical = "https://www.generic-mapping-tools.org/InteractiveGMT",
                             size_threshold = 500 * 2^10),
    pages = list_pages(),
)

deploydocs(; repo = "github.com/GenericMappingTools/InteractiveGMT.git", devbranch = "master")
