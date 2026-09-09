# Fault plane demo assets

`footwall.stl` and `hanging_wall.stl` are unchanged copies of the user-supplied
`Footwall 4.stl` and `Hanging Wall 4.stl`. `parameters.jpg` is the supplied reference figure.

The binary meshes have 116 and 108 triangles. Their inclined faces have a 64-degree dip.
Julia (`src/faultdemo.jl`) aligns the meshes and rejoins their exterior into a closed
shell, preserving the surface markings. Changing dip cuts this shell at the selected
angle and caps both new mating faces. The outer surfaces stay level; the blocks are
reconstructed rather than tilted. This also handles horizontal and vertical fault planes.
The display is normalized by the common along-strike length, with a gap of 2.5% of that
length. The original STL files remain unchanged.

Fault azimuth means strike clockwise from North, with dip to the right. For positive
slip, rake +90 is reverse, -90 normal, 0 left-lateral, and ±180 right-lateral, following
the [USGS/Aki–Richards convention](https://pubs.usgs.gov/of/2011/1060/of2011-1060.pdf).
Slip moves the hanging wall only, with a maximum travel of ±30% of block length.
The focal-mechanism preview uses the package's existing Aki–Richards sector geometry and
updates from strike, dip, and signed rake; positive and negative rake are kept distinct.
