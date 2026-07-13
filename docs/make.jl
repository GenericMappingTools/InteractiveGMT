using InteractiveGMT
using Documenter

DocMeta.setdocmeta!(InteractiveGMT, :DocTestSetup, :(using InteractiveGMT); recursive = true)

# Add titles of sections and overrides page titles
const titles = Dict(
    "01-getting-started.md" => "Getting Started",
    "05-preferences.md" => "Preferences",
    "10-grid-viewer.md" => "Grid Viewer",
    "15-shading.md" => "Shading Dock",
    "20-point-clouds.md" => "Point Clouds",
    "30-solids.md" => "Solids and Meshes",
    "40-xyplot.md" => "X,Y Plot Tool",
    "50-utilities.md" => "Utilities",
    "60-geography.md" => "Geography Tools",
    "70-tools.md" => "Tools",
    "95-reference.md" => "API Reference",
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
    authors = "Joaquim Luis <jluis@ualg.pt>",
    repo = "https://github.com/joa-quim/InteractiveGMT.jl/blob/{commit}{path}#{line}",
    sitename = "InteractiveGMT.jl",
    format = Documenter.HTML(; canonical = "https://joa-quim.github.io/InteractiveGMT.jl"),
    pages = list_pages(),
)

deploydocs(; repo = "github.com/joa-quim/InteractiveGMT.jl")
