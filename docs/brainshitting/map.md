1. Decouple the terrain mesh and heightmap from the grid.
2. The grid is made of:
- the blocks (just squares)
- the subtriangles (made from diagonals crossing each point)
Q: What's the best way to index? (x, y, 0-3)?
3. The subgrid is made of NxN (5x5) grid spanning each block edge to edge (hence the edges overlap between blocks)
- some objects snap to subgrid
4. The map is largely 2d and the buildable/walkable area is made of flat sections
5. Sections are not necessarily same height (though within a section they should largely have similar height)
6. The sections will be connected, forming a graph-of-sections. Connected sections will, most often, have different heights.
7. We don't expect big verticality in the map, treat it mostly as visual guideline. We want to have interesting shapes on the map and they should prevent blocking and pathfinding.


Visualization:
- block side is 10m
- sections should have limited height difference (max 0.5m per block)
- use debug lines to draw the grid, above the section

Map gen example:
- 2d heightmap, 1m density, 2k x 2k
- voronoi style to generate pre-sections (large)
- assign biomes to clusters of sections: lake/swamp/forest/plains/hills
- apply detailed noise masked by the biome (lake/swamp = flat, plains = largely flat, forest = largely flat, hills = most rigged with rocky areas)
- analyze the heights to produce actual sections + blocks (blocks are always aligned to the meter grid, assume texels represent the height in the middle of the area footprint and there's a "clamp" style sampling = all the edges are flat)
- the water / swamp will produce cavity in the terrain mesh with smooth edges (we want gentle slopes)
- plains should always have height modulated to create big sections (ideally 1 covering all connected planes)
- hills should be gentle, with rocky areas that should follow organic ridge formations 