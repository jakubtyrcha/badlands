// Port of the kTexturedMesh vertex layout (sampo src/rendering/vertex_layout.cpp
// / StaticTexturedMeshComponent): interleaved pos(3) + uv(2) + normal(3) +
// tangent(3), 44-byte stride, triangle list, CCW front faces.

use glam::Vec3;

pub struct MeshData {
    pub vertices: Vec<f32>,
    pub vertex_count: u32,
}

impl MeshData {
    fn push_vertex(&mut self, pos: Vec3, uv: [f32; 2], normal: Vec3, tangent: Vec3) {
        self.vertices.extend_from_slice(&[
            pos.x, pos.y, pos.z, uv[0], uv[1], normal.x, normal.y, normal.z, tangent.x, tangent.y,
            tangent.z,
        ]);
        self.vertex_count += 1;
    }

    pub fn upload(&self, device: &wgpu::Device, label: &str) -> wgpu::Buffer {
        use wgpu::util::DeviceExt;
        device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some(label),
            contents: bytemuck::cast_slice(&self.vertices),
            usage: wgpu::BufferUsages::VERTEX,
        })
    }
}

// Ground plane at y=0, centered at the origin, viewed from +Y (normal up).
// UVs tile `uv_tiles` times across the quad.
pub fn build_ground_quad(half_extent: f32, uv_tiles: f32) -> MeshData {
    let mut mesh = MeshData {
        vertices: Vec::new(),
        vertex_count: 0,
    };
    let e = half_extent;
    let t = uv_tiles;
    let n = Vec3::Y;
    let tan = Vec3::X;
    // Two CCW triangles as seen from above (+Y looking down -Y).
    let corners = [
        (Vec3::new(-e, 0.0, -e), [0.0, 0.0]),
        (Vec3::new(-e, 0.0, e), [0.0, t]),
        (Vec3::new(e, 0.0, e), [t, t]),
        (Vec3::new(e, 0.0, -e), [t, 0.0]),
    ];
    for &index in &[0usize, 1, 2, 0, 2, 3] {
        let (pos, uv) = corners[index];
        mesh.push_vertex(pos, uv, n, tan);
    }
    mesh
}

// Unit cube: XZ centered at origin, base at y=0, top at y=1. Scaled per
// building by its model matrix. Face normals for directional lighting.
pub fn build_unit_cube() -> MeshData {
    let mut mesh = MeshData {
        vertices: Vec::new(),
        vertex_count: 0,
    };

    struct Face {
        normal: Vec3,
        tangent: Vec3,
        // corners in CCW order when viewed from outside
        corners: [Vec3; 4],
    }

    let lo = -0.5;
    let hi = 0.5;
    let faces = [
        // +Y (top)
        Face {
            normal: Vec3::Y,
            tangent: Vec3::X,
            corners: [
                Vec3::new(lo, 1.0, lo),
                Vec3::new(lo, 1.0, hi),
                Vec3::new(hi, 1.0, hi),
                Vec3::new(hi, 1.0, lo),
            ],
        },
        // +X
        Face {
            normal: Vec3::X,
            tangent: Vec3::NEG_Z,
            corners: [
                Vec3::new(hi, 1.0, hi),
                Vec3::new(hi, 0.0, hi),
                Vec3::new(hi, 0.0, lo),
                Vec3::new(hi, 1.0, lo),
            ],
        },
        // -X
        Face {
            normal: Vec3::NEG_X,
            tangent: Vec3::Z,
            corners: [
                Vec3::new(lo, 1.0, lo),
                Vec3::new(lo, 0.0, lo),
                Vec3::new(lo, 0.0, hi),
                Vec3::new(lo, 1.0, hi),
            ],
        },
        // +Z
        Face {
            normal: Vec3::Z,
            tangent: Vec3::X,
            corners: [
                Vec3::new(lo, 1.0, hi),
                Vec3::new(lo, 0.0, hi),
                Vec3::new(hi, 0.0, hi),
                Vec3::new(hi, 1.0, hi),
            ],
        },
        // -Z
        Face {
            normal: Vec3::NEG_Z,
            tangent: Vec3::NEG_X,
            corners: [
                Vec3::new(hi, 1.0, lo),
                Vec3::new(hi, 0.0, lo),
                Vec3::new(lo, 0.0, lo),
                Vec3::new(lo, 1.0, lo),
            ],
        },
        // -Y (bottom, rarely visible from a top-down camera)
        Face {
            normal: Vec3::NEG_Y,
            tangent: Vec3::X,
            corners: [
                Vec3::new(lo, 0.0, lo),
                Vec3::new(hi, 0.0, lo),
                Vec3::new(hi, 0.0, hi),
                Vec3::new(lo, 0.0, hi),
            ],
        },
    ];

    let uvs = [[0.0, 0.0], [0.0, 1.0], [1.0, 1.0], [1.0, 0.0]];
    for face in &faces {
        for &index in &[0usize, 1, 2, 0, 2, 3] {
            mesh.push_vertex(face.corners[index], uvs[index], face.normal, face.tangent);
        }
    }
    mesh
}

// Unit capsule for characters: XZ radius 0.5, base at y=0, top at y=2 (two
// hemispherical caps of radius 0.5 around a cylinder of height 1). The canonical
// height is 2 so scaling by (size_x, size_y*0.5, size_z) yields radius
// 0.5*size_x and total height size_y (spherical caps when size_y == 2*size_x).
pub fn build_unit_capsule() -> MeshData {
    const SEG: usize = 20; // segments around
    const CAP: usize = 5; // latitude rings per hemisphere
    let r = 0.5f32;
    let bottom_center = 0.5f32;
    let top_center = 1.5f32;

    // Ring definitions: (y, xz_radius, normal_y). The bottom-hemisphere equator
    // and the top-hemisphere equator (both normal_y = 0) bound the cylinder.
    let mut rings: Vec<(f32, f32, f32)> = Vec::new();
    for i in 0..=CAP {
        let phi = -std::f32::consts::FRAC_PI_2 + std::f32::consts::FRAC_PI_2 * (i as f32 / CAP as f32);
        rings.push((bottom_center + r * phi.sin(), r * phi.cos(), phi.sin()));
    }
    for i in 0..=CAP {
        let phi = std::f32::consts::FRAC_PI_2 * (i as f32 / CAP as f32);
        rings.push((top_center + r * phi.sin(), r * phi.cos(), phi.sin()));
    }

    let mut mesh = MeshData {
        vertices: Vec::new(),
        vertex_count: 0,
    };
    let vert = |y: f32, rad: f32, ny: f32, j: usize| -> (Vec3, [f32; 2], Vec3, Vec3) {
        let theta = std::f32::consts::TAU * (j as f32 / SEG as f32);
        let (st, ct) = theta.sin_cos();
        let nxz = (1.0 - ny * ny).max(0.0).sqrt();
        (
            Vec3::new(rad * ct, y, rad * st),
            [j as f32 / SEG as f32, y * 0.5],
            Vec3::new(nxz * ct, ny, nxz * st),
            Vec3::new(-st, 0.0, ct),
        )
    };
    for i in 0..rings.len() - 1 {
        let (y0, r0, ny0) = rings[i];
        let (y1, r1, ny1) = rings[i + 1];
        for j in 0..SEG {
            let a = vert(y0, r0, ny0, j);
            let b = vert(y1, r1, ny1, j);
            let c = vert(y1, r1, ny1, j + 1);
            let d = vert(y0, r0, ny0, j + 1);
            for v in [a, b, c, a, c, d] {
                mesh.push_vertex(v.0, v.1, v.2, v.3);
            }
        }
    }
    mesh
}
