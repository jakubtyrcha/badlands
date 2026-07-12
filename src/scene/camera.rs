// Port of sampo src/core/camera.hpp: struct Camera (reversed-Z projection) and
// struct UniformData, which must match FrameUniforms in shaders/common/frame.wesl
// field-for-field (@group(0) @binding(0)).

use bytemuck::{Pod, Zeroable};
use glam::{Mat4, Vec2, Vec3, Vec4};

#[derive(Clone, Copy, Debug)]
pub struct Camera {
    pub position: Vec3,
    pub direction: Vec3, // normalized look direction
    pub up: Vec3,
    pub fov: f32, // vertical, degrees
    pub aspect: f32,
    pub near_plane: f32,
    pub far_plane: f32,
}

impl Default for Camera {
    fn default() -> Self {
        Camera {
            position: Vec3::ZERO,
            direction: Vec3::NEG_Z,
            up: Vec3::Y,
            fov: 45.0,
            aspect: 1.0,
            near_plane: 0.1,
            far_plane: 10000.0,
        }
    }
}

impl Camera {
    // World-camera-offseted convention (frame.wesl): the view matrix has the
    // camera at the origin; world positions are rebased by camera_world_pos.
    pub fn view_at_origin(&self) -> Mat4 {
        glam::camera::rh::view::look_at_mat4(Vec3::ZERO, self.direction, self.up)
    }

    // Port of Camera::GetProj(): GL-style ([-1,1] NDC Z) perspective remapped
    // with z' = w - z. Far maps to depth 0; the configured near plane maps to
    // depth 2, so the effective near clip (depth 1) sits at
    // 2*near*far/(far+near) ~= 2*near_plane. This is the sampo engine's own
    // convention — frame.wesl's reconstructLinearZ encodes exactly this
    // matrix, so don't "fix" it here without changing the shaders.
    pub fn proj(&self) -> Mat4 {
        let proj = glam::camera::rh::proj::opengl::perspective(
            self.fov.to_radians(),
            self.aspect,
            self.near_plane,
            self.far_plane,
        );
        let remap = Mat4::from_cols(
            Vec4::X,
            Vec4::Y,
            Vec4::new(0.0, 0.0, -1.0, 0.0),
            Vec4::new(0.0, 0.0, 1.0, 1.0),
        );
        remap * proj
    }
}

// Port of struct UniformData (camera.hpp). Matches FrameUniforms in
// common/frame.wesl; size asserted below.
#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
pub struct UniformData {
    pub view: Mat4, // world-offset view (camera at origin)
    pub proj: Mat4,
    pub view_prev: Mat4,
    pub proj_prev: Mat4,
    pub light_view_proj: Mat4,
    pub camera_world_pos: Vec4,
    pub sun_dir: Vec4, // xyz = direction toward the sun
    pub sun_color: Vec4,
    pub ambient_sh: [Vec4; 9],
    pub sphere_offset: Vec4,
    pub jitter: Vec2,
    pub jitter_prev: Vec2,
    pub near_plane: f32,
    pub far_plane: f32,
    pub screen_size: Vec2,
    pub enable_gtao: u32,
    pub tonemap_mode: u32,
    pub output_is_linear: u32,
    pub padding0: f32,
}

const _: () = assert!(
    std::mem::size_of::<UniformData>() == 576,
    "UniformData must match FrameUniforms in common/frame.wesl"
);

impl UniformData {
    pub fn from_camera(camera: &Camera) -> UniformData {
        let view = camera.view_at_origin();
        let proj = camera.proj();
        UniformData {
            view,
            proj,
            view_prev: view,
            proj_prev: proj,
            light_view_proj: Mat4::IDENTITY,
            camera_world_pos: camera.position.extend(0.0),
            sun_dir: Vec4::new(0.0, 1.0, 0.0, 0.0),
            sun_color: Vec4::ONE,
            ambient_sh: [Vec4::ZERO; 9],
            sphere_offset: Vec4::ZERO,
            jitter: Vec2::ZERO,
            jitter_prev: Vec2::ZERO,
            near_plane: camera.near_plane,
            far_plane: camera.far_plane,
            screen_size: Vec2::ZERO,
            enable_gtao: 0,
            tonemap_mode: 0,
            output_is_linear: 0,
            padding0: 1.0,
        }
    }
}
