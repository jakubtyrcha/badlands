// Flexbox layout over the element batch, via the `panes` crate.
//
// panes models containers as INVISIBLE (Node::Row/Col) and panels as LEAVES:
// ResolvedLayout only exposes rects for leaf panels, so a container has no rect
// of its own. Our model wants the opposite — a UI_ELEM_PANEL is a container
// that paints a background, so it needs an exact rect.
//
// Rather than reconstructing container rects from the union of their children
// (wrong: a `grow` container is sized by its parent, and its children need not
// fill it — a 40px top bar holding one short label would get a label-sized
// background), we resolve LEVEL BY LEVEL. Each container is solved on its own,
// with its direct children as leaf panels, against its own already-known rect;
// child rects come back relative and are offset by the container's origin. Every
// element therefore gets an exact rect, containers included. Each solve is tiny
// and there are only a handful per frame.

use panes::{Constraints, LayoutTree, Rect};

use crate::element::{Element, Kind};

// Solve a rect (physical px) for every element. rects[i] corresponds to
// elements[i]. `viewport` is the full surface; element 0 is laid out against it.
pub fn solve(elements: &[Element], viewport: Rect, scale: f32) -> Option<Vec<Rect>> {
    let n = elements.len();
    let mut rects = vec![Rect::default(); n];
    if n == 0 {
        return Some(rects);
    }
    rects[0] = viewport;

    // Direct children, in declaration order. The caller-side validation has
    // already guaranteed parent < index, so a single forward pass suffices and
    // the tree is acyclic by construction.
    let mut children: Vec<Vec<usize>> = vec![Vec::new(); n];
    for (i, e) in elements.iter().enumerate().skip(1) {
        children[e.parent as usize].push(i);
    }

    // Parents precede children, so iterating forward means a container's own
    // rect is always final before we solve its children.
    for i in 0..n {
        if children[i].is_empty() {
            continue;
        }
        solve_children(elements, &children[i], rects[i], scale, &mut rects)?;
    }
    Some(rects)
}

fn solve_children(
    elements: &[Element],
    kids: &[usize],
    parent_rect: Rect,
    scale: f32,
    rects: &mut [Rect],
) -> Option<()> {
    // The container whose children these are: every kid shares the same parent.
    let container = &elements[elements[kids[0]].parent as usize];

    let pad = container.pad * scale;
    let gap = container.gap * scale;
    // Content box: the container's rect inset by its padding, floored at zero so
    // an over-padded container degenerates rather than producing negative sizes.
    let inner = Rect {
        x: parent_rect.x + pad,
        y: parent_rect.y + pad,
        w: (parent_rect.w - 2.0 * pad).max(0.0),
        h: (parent_rect.h - 2.0 * pad).max(0.0),
    };

    let mut tree = LayoutTree::new();
    let mut panel_ids = Vec::with_capacity(kids.len());
    let mut node_ids = Vec::with_capacity(kids.len());
    for &k in kids {
        let (pid, nid) = tree.add_panel("e", constraints_for(&elements[k], scale)).ok()?;
        panel_ids.push(pid);
        node_ids.push(nid);
    }

    // PANEL is a COL that also paints a background, so it lays out like one.
    let root = match container.kind {
        Kind::Row => tree.add_row_constrained(gap, None, node_ids).ok()?,
        _ => tree.add_col_constrained(gap, None, node_ids).ok()?,
    };
    tree.set_root(root);

    let resolved = tree.resolve(inner.w, inner.h).ok()?;
    for (&k, pid) in kids.iter().zip(panel_ids) {
        let r = resolved.get(pid)?;
        rects[k] = Rect {
            x: inner.x + r.x,
            y: inner.y + r.y,
            w: r.w,
            h: r.h,
        };
    }
    Some(())
}

fn constraints_for(e: &Element, scale: f32) -> Constraints {
    if e.fixed > 0.0 {
        panes::fixed(e.fixed * scale)
    } else if e.grow > 0.0 {
        panes::grow(e.grow)
    } else {
        // Neither given: share the remaining space evenly. panes rejects a
        // node with no sizing at all, and "fill what's left" is the least
        // surprising default for a UI element the caller didn't size.
        panes::grow(1.0)
    }
}
