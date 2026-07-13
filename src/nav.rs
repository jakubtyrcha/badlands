// 2D navmesh-style path service (Rust side of the GamePathfinder contract).
//
// The C++ sim owns movement/ECS and delegates *path geometry* to this module
// through a narrow C ABI (`GamePathfinder` in game/include/badlands_game.h):
// obstacles mutate one building at a time (`nav_add_obstacle`/
// `nav_remove_obstacle`) and paths are queried with `nav_find_path`. Buildings
// are added/destroyed singly, so the visibility graph is maintained
// *incrementally* — never rebuilt from scratch on each change.
//
// Geometry stack (mature, permissively-licensed crates):
//   - cavalier_contours: offset each footprint outward by the agent radius
//     (Minkowski clearance), arcs sampled to line segments.
//   - parry2d (glam backend): segment-vs-convex line-of-sight + point recovery.
//   - pathfinding: A* over the reduced visibility graph.
//
// Obstacles are kept per-building and independent (no global union), which is
// what makes incremental add/remove cheap: a union would force a from-scratch
// re-merge on every change.

use std::collections::HashMap;
use std::os::raw::c_void;

use cavalier_contours::polyline::{PlineSource, PlineSourceMut, Polyline};
use glam::Vec2;
use parry2d::math::Pose;
use parry2d::query::{intersection_test, PointQuery};
use parry2d::shape::{ConvexPolygon, Segment};
use pathfinding::directed::astar::astar;

/// Max deviation when flattening offset arcs into line segments (world units).
const ARC_ERROR: f64 = 0.05;
/// Corner nodes are pushed this far outside their own expanded polygon so a
/// line-of-sight test never reports a node as blocked by its own obstacle.
const NODE_NUDGE: f32 = 0.02;
/// A* costs are integers; scale world distance to fixed-point millis-ish units.
const COST_SCALE: f32 = 1024.0;
/// Radius-bucket granularity for the per-radius graph cache.
const RADIUS_QUANT: f32 = 256.0;

fn cost(d: f32) -> u64 {
    (d * COST_SCALE).round().max(0.0) as u64
}

fn pair(a: u32, b: u32) -> (u32, u32) {
    if a <= b { (a, b) } else { (b, a) }
}

/// Absolute polygon area via the shoelace formula.
fn abs_area(pts: &[Vec2]) -> f32 {
    if pts.len() < 3 {
        return 0.0;
    }
    let mut acc = 0.0;
    for i in 0..pts.len() {
        let a = pts[i];
        let b = pts[(i + 1) % pts.len()];
        acc += a.x * b.y - b.x * a.y;
    }
    (acc * 0.5).abs()
}

/// Segment (a,b) intersects the convex polygon's interior/boundary.
fn seg_hits(a: Vec2, b: Vec2, poly: &ConvexPolygon) -> bool {
    let seg = Segment::new(a, b);
    intersection_test(&Pose::IDENTITY, &seg, &Pose::IDENTITY, poly).unwrap_or(true)
}

/// Offset a footprint outward by `radius`, returning the expanded corner nodes
/// (nudged just outside the boundary) plus a convex polygon used for LoS. The
/// offset sign is chosen by area so the caller's winding does not matter.
fn expand_footprint(footprint: &[Vec2], radius: f32) -> Option<(Vec<Vec2>, ConvexPolygon)> {
    if footprint.len() < 3 || radius <= 0.0 {
        // Degenerate: fall back to the raw footprint as its own obstacle.
        let pts: Vec<Vec2> = footprint.to_vec();
        let poly = ConvexPolygon::from_convex_hull(&pts)?;
        return Some((pts, poly));
    }

    let mut pl: Polyline<f64> = Polyline::new();
    for p in footprint {
        pl.add(p.x as f64, p.y as f64, 0.0);
    }
    pl.set_is_closed(true);

    // The outward expansion is whichever signed offset grows the area.
    let mut candidates = pl.parallel_offset(radius as f64);
    candidates.extend(pl.parallel_offset(-(radius as f64)));

    let flattened: Vec<Vec<Vec2>> = candidates
        .into_iter()
        .map(|c| {
            let lines = c.arcs_to_approx_lines(ARC_ERROR).unwrap_or(c);
            lines
                .iter_vertexes()
                .map(|v| Vec2::new(v.x as f32, v.y as f32))
                .collect::<Vec<_>>()
        })
        .collect();

    let expanded = flattened
        .into_iter()
        .max_by(|a, b| abs_area(a).partial_cmp(&abs_area(b)).unwrap())?;
    if expanded.len() < 3 {
        return None;
    }

    let poly = ConvexPolygon::from_convex_hull(&expanded)?;
    let centroid = expanded.iter().copied().sum::<Vec2>() / expanded.len() as f32;
    let nodes: Vec<Vec2> = expanded
        .iter()
        .map(|&c| {
            let out = (c - centroid).normalize_or_zero();
            c + out * NODE_NUDGE
        })
        .collect();
    Some((nodes, poly))
}

struct Node {
    pos: Vec2,
}

/// A visibility graph over expanded footprint corners for one clearance radius.
/// Node ids are stable slab indices (never reused while live) so the
/// blocker map survives incremental add/remove without re-indexing.
struct Graph {
    radius: f32,
    slots: Vec<Option<Node>>,
    free: Vec<u32>,
    polys: HashMap<u32, ConvexPolygon>,
    building_nodes: HashMap<u32, Vec<u32>>,
    /// Per node-pair: the set of building ids whose expanded polygon blocks the
    /// segment between them. An edge is clear iff its blocker set is empty
    /// (or, for an exempt query, contains only the exempt building).
    blockers: HashMap<(u32, u32), Vec<u32>>,
}

impl Graph {
    fn new(radius: f32) -> Graph {
        Graph {
            radius,
            slots: Vec::new(),
            free: Vec::new(),
            polys: HashMap::new(),
            building_nodes: HashMap::new(),
            blockers: HashMap::new(),
        }
    }

    fn alloc(&mut self, node: Node) -> u32 {
        if let Some(id) = self.free.pop() {
            self.slots[id as usize] = Some(node);
            id
        } else {
            self.slots.push(Some(node));
            (self.slots.len() - 1) as u32
        }
    }

    fn node(&self, id: u32) -> &Node {
        self.slots[id as usize].as_ref().unwrap()
    }

    fn live_ids(&self) -> Vec<u32> {
        (0..self.slots.len() as u32)
            .filter(|&i| self.slots[i as usize].is_some())
            .collect()
    }

    /// Building ids (other than the two endpoint owners) whose polygon blocks
    /// segment (a,b).
    fn compute_blockers(&self, a: u32, b: u32) -> Vec<u32> {
        let pa = self.node(a).pos;
        let pb = self.node(b).pos;
        let mut out = Vec::new();
        for (bid, poly) in &self.polys {
            if seg_hits(pa, pb, poly) {
                out.push(*bid);
            }
        }
        out.sort_unstable();
        out
    }

    fn add(&mut self, id: u32, footprint: &[Vec2]) {
        if self.building_nodes.contains_key(&id) {
            self.remove(id);
        }
        let Some((corners, poly)) = expand_footprint(footprint, self.radius) else {
            return;
        };
        self.polys.insert(id, poly);

        let pre_existing = self.live_ids();
        let mut new_ids = Vec::with_capacity(corners.len());
        for c in &corners {
            new_ids.push(self.alloc(Node { pos: *c }));
        }
        self.building_nodes.insert(id, new_ids.clone());

        // New pairs: (new, pre-existing) and (new, new). Full LoS vs all polys.
        for (idx, &n) in new_ids.iter().enumerate() {
            for &m in pre_existing.iter().chain(new_ids[idx + 1..].iter()) {
                let key = pair(n, m);
                let bs = self.compute_blockers(key.0, key.1);
                self.blockers.insert(key, bs);
            }
        }

        // Pre-existing pairs: only the *new* obstacle can newly block them.
        let new_poly = &self.polys[&id];
        let keys: Vec<(u32, u32)> = self
            .blockers
            .keys()
            .filter(|k| !new_ids.contains(&k.0) && !new_ids.contains(&k.1))
            .copied()
            .collect();
        for key in keys {
            let pa = self.node(key.0).pos;
            let pb = self.node(key.1).pos;
            if seg_hits(pa, pb, new_poly) {
                let bs = self.blockers.get_mut(&key).unwrap();
                if let Err(pos) = bs.binary_search(&id) {
                    bs.insert(pos, id);
                }
            }
        }
    }

    fn remove(&mut self, id: u32) {
        let Some(node_ids) = self.building_nodes.remove(&id) else {
            return;
        };
        self.polys.remove(&id);
        let removed: std::collections::HashSet<u32> = node_ids.iter().copied().collect();
        for &nid in &node_ids {
            self.slots[nid as usize] = None;
            self.free.push(nid);
        }
        // Drop pairs touching removed nodes; clear `id` from the rest.
        let keys: Vec<(u32, u32)> = self.blockers.keys().copied().collect();
        for key in keys {
            if removed.contains(&key.0) || removed.contains(&key.1) {
                self.blockers.remove(&key);
            } else if let Some(bs) = self.blockers.get_mut(&key) {
                if let Ok(pos) = bs.binary_search(&id) {
                    bs.remove(pos);
                }
            }
        }
    }

    /// Whether an edge is usable given an optional exempt building whose
    /// clearance is ignored (so a unit can path to the building it is entering).
    fn edge_ok(blockers: &[u32], exempt: Option<u32>) -> bool {
        match exempt {
            None => blockers.is_empty(),
            Some(e) => blockers.iter().all(|&b| b == e),
        }
    }

    /// Project a point out of any expanded polygon it lies inside, skipping the
    /// exempt building. Returns the (possibly recovered) point.
    fn recover(&self, p: Vec2, exempt: Option<u32>) -> Vec2 {
        for (bid, poly) in &self.polys {
            if exempt == Some(*bid) {
                continue;
            }
            let proj = poly.project_local_point(p, false);
            if proj.is_inside {
                // Push a hair past the boundary so LoS from here is clean.
                let dir = (proj.point - p).normalize_or_zero();
                return proj.point + dir * NODE_NUDGE;
            }
        }
        p
    }

    /// Shortest clearance-respecting path from `start` to `goal`. Returns the
    /// waypoint polyline (>=2 points) or an empty vec if unreachable.
    fn find(&self, start: Vec2, goal: Vec2, exempt: Option<u32>) -> Vec<Vec2> {
        let s = self.recover(start, exempt);
        let g = self.recover(goal, exempt);

        // Dense node array: corner nodes first, then start (S), then goal (G).
        let live = self.live_ids();
        let mut pos: Vec<Vec2> = live.iter().map(|&id| self.node(id).pos).collect();
        let slab_to_dense: HashMap<u32, usize> =
            live.iter().enumerate().map(|(d, &id)| (id, d)).collect();
        let s_idx = pos.len();
        pos.push(s);
        let g_idx = pos.len();
        pos.push(g);

        let n = pos.len();
        let mut adj: Vec<Vec<(usize, u64)>> = vec![Vec::new(); n];

        // Corner-corner edges from the maintained blocker map.
        for (key, bs) in &self.blockers {
            if Graph::edge_ok(bs, exempt) {
                let (a, b) = (slab_to_dense[&key.0], slab_to_dense[&key.1]);
                let w = cost(pos[a].distance(pos[b]));
                adj[a].push((b, w));
                adj[b].push((a, w));
            }
        }

        // Start/goal edges: LoS vs every obstacle (skip the exempt building).
        let query_poly = |a: Vec2, b: Vec2| -> bool {
            self.polys
                .iter()
                .filter(|(bid, _)| exempt != Some(**bid))
                .any(|(_, poly)| seg_hits(a, b, poly))
        };
        for d in 0..n {
            if d == s_idx {
                continue;
            }
            if !query_poly(s, pos[d]) {
                let w = cost(s.distance(pos[d]));
                adj[s_idx].push((d, w));
                adj[d].push((s_idx, w));
            }
        }
        for d in 0..n {
            if d == g_idx || d == s_idx {
                continue;
            }
            if !query_poly(g, pos[d]) {
                let w = cost(g.distance(pos[d]));
                adj[g_idx].push((d, w));
                adj[d].push((g_idx, w));
            }
        }
        // Direct start->goal shot.
        if !query_poly(s, g) {
            let w = cost(s.distance(g));
            adj[s_idx].push((g_idx, w));
            adj[g_idx].push((s_idx, w));
        }

        let result = astar(
            &s_idx,
            |&i| adj[i].iter().copied().collect::<Vec<_>>(),
            |&i| cost(pos[i].distance(g)),
            |&i| i == g_idx,
        );
        match result {
            Some((path, _)) => path.into_iter().map(|i| pos[i]).collect(),
            None => Vec::new(),
        }
    }
}

/// The path service handed to C++ as an opaque `void* ctx`. Holds raw
/// footprints plus one incrementally-maintained visibility graph per clearance
/// radius that has been queried.
pub struct NavContext {
    footprints: HashMap<u32, Vec<Vec2>>,
    graphs: HashMap<u32, Graph>,
}

impl NavContext {
    pub fn new() -> NavContext {
        NavContext {
            footprints: HashMap::new(),
            graphs: HashMap::new(),
        }
    }

    fn quant(radius: f32) -> u32 {
        (radius * RADIUS_QUANT).round().max(1.0) as u32
    }

    pub fn add_obstacle(&mut self, id: u32, footprint: Vec<Vec2>) {
        for g in self.graphs.values_mut() {
            g.add(id, &footprint);
        }
        self.footprints.insert(id, footprint);
    }

    pub fn remove_obstacle(&mut self, id: u32) {
        self.footprints.remove(&id);
        for g in self.graphs.values_mut() {
            g.remove(id);
        }
    }

    pub fn find_path(&mut self, start: Vec2, goal: Vec2, radius: f32, exempt: Option<u32>) -> Vec<Vec2> {
        let key = NavContext::quant(radius);
        if !self.graphs.contains_key(&key) {
            // First query at this radius: build the graph from current obstacles.
            let mut g = Graph::new(radius);
            for (id, fp) in &self.footprints {
                g.add(*id, fp);
            }
            self.graphs.insert(key, g);
        }
        self.graphs[&key].find(start, goal, exempt)
    }
}

// ---------------------------------------------------------------------------
// C ABI: the GamePathfinder vtable thunks. `ctx` is a `*mut NavContext`.
// ---------------------------------------------------------------------------

/// # Safety
/// `ctx` must be a live `NavContext` pointer; `poly_xz` must point to
/// `2 * n_verts` floats.
pub unsafe extern "C" fn nav_add_obstacle(
    ctx: *mut c_void,
    building_id: u32,
    poly_xz: *const f32,
    n_verts: i32,
) {
    if ctx.is_null() || poly_xz.is_null() || n_verts < 3 {
        return;
    }
    let nav = unsafe { &mut *(ctx as *mut NavContext) };
    let n = n_verts as usize;
    let flat = unsafe { std::slice::from_raw_parts(poly_xz, n * 2) };
    let footprint: Vec<Vec2> = (0..n).map(|i| Vec2::new(flat[i * 2], flat[i * 2 + 1])).collect();
    nav.add_obstacle(building_id, footprint);
}

/// # Safety
/// `ctx` must be a live `NavContext` pointer.
pub unsafe extern "C" fn nav_remove_obstacle(ctx: *mut c_void, building_id: u32) {
    if ctx.is_null() {
        return;
    }
    let nav = unsafe { &mut *(ctx as *mut NavContext) };
    nav.remove_obstacle(building_id);
}

/// # Safety
/// `ctx` must be a live `NavContext` pointer; `out_xz` must have room for
/// `2 * cap` floats.
pub unsafe extern "C" fn nav_find_path(
    ctx: *mut c_void,
    sx: f32,
    sz: f32,
    gx: f32,
    gz: f32,
    radius: f32,
    exempt_building: u32,
    out_xz: *mut f32,
    cap: i32,
) -> i32 {
    if ctx.is_null() {
        return 0;
    }
    let nav = unsafe { &mut *(ctx as *mut NavContext) };
    let exempt = (exempt_building != u32::MAX).then_some(exempt_building);
    let waypoints = nav.find_path(Vec2::new(sx, sz), Vec2::new(gx, gz), radius, exempt);
    let count = waypoints.len();
    if !out_xz.is_null() && cap > 0 {
        let out = unsafe { std::slice::from_raw_parts_mut(out_xz, (cap as usize) * 2) };
        for (i, wp) in waypoints.iter().take(cap as usize).enumerate() {
            out[i * 2] = wp.x;
            out[i * 2 + 1] = wp.y;
        }
    }
    count as i32
}

#[cfg(test)]
mod tests {
    use super::*;

    // Axis-aligned square footprint centered at `c` with half-extent `h`.
    fn square(cx: f32, cz: f32, h: f32) -> Vec<Vec2> {
        vec![
            Vec2::new(cx - h, cz - h),
            Vec2::new(cx + h, cz - h),
            Vec2::new(cx + h, cz + h),
            Vec2::new(cx - h, cz + h),
        ]
    }

    fn seg_clear_of_all(a: Vec2, b: Vec2, nav: &NavContext, radius: f32) -> bool {
        // Reconstruct expanded polys and check the segment misses all of them.
        nav.footprints.values().all(|fp| {
            let (_, poly) = expand_footprint(fp, radius).unwrap();
            !seg_hits(a, b, &poly)
        })
    }

    #[test]
    fn straight_shot_is_two_points() {
        let mut nav = NavContext::new();
        let path = nav.find_path(Vec2::new(-5.0, 0.0), Vec2::new(5.0, 0.0), 0.5, None);
        assert_eq!(path.len(), 2);
        assert!((path[0] - Vec2::new(-5.0, 0.0)).length() < 1e-3);
        assert!((path[1] - Vec2::new(5.0, 0.0)).length() < 1e-3);
    }

    #[test]
    fn routes_around_a_building() {
        let mut nav = NavContext::new();
        // Wall straddling the straight line between start and goal.
        nav.add_obstacle(1, square(0.0, 0.0, 2.0));
        let start = Vec2::new(-6.0, 0.0);
        let goal = Vec2::new(6.0, 0.0);
        let radius = 0.5;
        let path = nav.find_path(start, goal, radius, None);
        assert!(path.len() >= 3, "expected a detour, got {:?}", path);
        // Every segment clears the expanded footprint.
        for w in path.windows(2) {
            assert!(seg_clear_of_all(w[0], w[1], &nav, radius), "segment {:?} hits obstacle", w);
        }
        // Path is longer than the straight line but not absurdly so.
        let len: f32 = path.windows(2).map(|w| w[0].distance(w[1])).sum();
        let straight = start.distance(goal);
        assert!(len > straight && len < straight * 2.0, "len {len} straight {straight}");
    }

    #[test]
    fn incremental_add_blocks_and_remove_restores() {
        let mut nav = NavContext::new();
        let start = Vec2::new(-6.0, 0.0);
        let goal = Vec2::new(6.0, 0.0);
        let radius = 0.5;

        // Clear line first.
        assert_eq!(nav.find_path(start, goal, radius, None).len(), 2);

        // Drop a wall across the route: path must detour.
        nav.add_obstacle(7, square(0.0, 0.0, 2.0));
        assert!(nav.find_path(start, goal, radius, None).len() >= 3);

        // Remove it: straight shot restored (incremental teardown).
        nav.remove_obstacle(7);
        assert_eq!(nav.find_path(start, goal, radius, None).len(), 2);
    }

    #[test]
    fn incremental_matches_from_scratch() {
        let radius = 0.5;
        let start = Vec2::new(-8.0, -1.0);
        let goal = Vec2::new(8.0, 1.0);

        // Built incrementally.
        let mut a = NavContext::new();
        a.add_obstacle(1, square(-2.0, 0.0, 1.5));
        a.add_obstacle(2, square(2.0, 0.0, 1.5));
        a.add_obstacle(3, square(0.0, 4.0, 1.0));
        a.remove_obstacle(3);
        let pa = a.find_path(start, goal, radius, None);

        // Built from scratch with the same surviving obstacles.
        let mut b = NavContext::new();
        b.add_obstacle(1, square(-2.0, 0.0, 1.5));
        b.add_obstacle(2, square(2.0, 0.0, 1.5));
        let pb = b.find_path(start, goal, radius, None);

        let la: f32 = pa.windows(2).map(|w| w[0].distance(w[1])).sum();
        let lb: f32 = pb.windows(2).map(|w| w[0].distance(w[1])).sum();
        assert!((la - lb).abs() < 1e-2, "incremental {la} vs scratch {lb}");
    }

    #[test]
    fn no_path_when_goal_enclosed() {
        // Four walls boxing in the goal with gaps narrower than 2*radius.
        let mut nav = NavContext::new();
        let big = 1.0; // large radius seals the 0.2-unit gaps
        // Build a ring of blocks around (0,0) leaving sub-2r gaps.
        nav.add_obstacle(1, square(0.0, -3.0, 2.4));
        nav.add_obstacle(2, square(0.0, 3.0, 2.4));
        nav.add_obstacle(3, square(-3.0, 0.0, 2.4));
        nav.add_obstacle(4, square(3.0, 0.0, 2.4));
        let path = nav.find_path(Vec2::new(-10.0, 0.0), Vec2::new(0.0, 0.0), big, None);
        assert!(path.is_empty(), "goal should be unreachable, got {:?}", path);
    }

    #[test]
    fn radius_clearance_grows_the_detour() {
        let mut nav = NavContext::new();
        nav.add_obstacle(1, square(0.0, 0.0, 2.0));
        let start = Vec2::new(-6.0, 0.0);
        let goal = Vec2::new(6.0, 0.0);
        let small: f32 = nav.find_path(start, goal, 0.2, None).windows(2).map(|w| w[0].distance(w[1])).sum();
        let large: f32 = nav.find_path(start, goal, 1.0, None).windows(2).map(|w| w[0].distance(w[1])).sum();
        assert!(large > small, "larger radius should detour wider: {large} vs {small}");
    }
}
