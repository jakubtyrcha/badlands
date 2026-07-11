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
