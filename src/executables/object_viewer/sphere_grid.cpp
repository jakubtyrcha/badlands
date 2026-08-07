#include "executables/object_viewer/sphere_grid.hpp"

#include <cmath>
#include <vector>

namespace badlands::object_viewer {

namespace {

constexpr float kPi = 3.14159265358979323846f;

}  // namespace

// The grid spans (columns-1) * spacing across and (rows-1) down, centred on the
// origin. Shared by the mesh build, the extent query and the headless oracle,
// so none of the three can disagree about where a sphere is.
glm::vec3 SphereGridCenter(uint32_t column, uint32_t row, float spacing) {
  const float x = (float(column) - float(kRoughnessSteps - 1) * 0.5f) * spacing;
  const float y = (float(kMetallicSteps - 1) * 0.5f - float(row)) * spacing;
  return {x, y, 0.0f};
}

float SphereRoughness(uint32_t column) {
  // 0 at the left, 1 at the right. The endpoints are included deliberately:
  // roughness 0 is the mirror that must reproduce the environment, and 1 is the
  // fully rough end where the multi-scatter energy deficit shows.
  return float(column) / float(kRoughnessSteps - 1);
}

float SphereMetallic(uint32_t row) { return row == 0 ? 0.0f : 1.0f; }

SphereGridBounds SphereGridExtent(float sphere_radius, float spacing) {
  SphereGridBounds b;
  b.sphere_radius = sphere_radius;
  const glm::vec3 first = SphereGridCenter(0, 0, spacing);
  const glm::vec3 last = SphereGridCenter(kRoughnessSteps - 1, kMetallicSteps - 1,
                                      spacing);
  b.center = (first + last) * 0.5f;
  b.radius = glm::length(last - b.center) + sphere_radius;
  return b;
}

namespace {

// The fold itself, taking the seam branch as an ARGUMENT.
//
// `n` is already L1-normalized. For the lower hemisphere the map reflects into
// the four corners of the square, and WHICH corner is not a function of the
// direction alone: the four edges running from the -Y apex to the equator lie
// exactly in n.x == 0 or n.z == 0, where the map is genuinely TWO-VALUED. The
// correct answer there depends on which octahedron face the point belongs to.
glm::vec2 OctFold(glm::vec3 n, float fold_x, float fold_z) {
  glm::vec2 p(n.x, n.z);
  if (n.y < 0.0f) {
    p = glm::vec2((1.0f - std::abs(n.z)) * fold_x,
                  (1.0f - std::abs(n.x)) * fold_z);
  }
  return p * 0.5f + 0.5f;
}

glm::vec3 L1Normalize(glm::vec3 dir, bool& ok) {
  const float l1 = std::abs(dir.x) + std::abs(dir.y) + std::abs(dir.z);
  ok = l1 > 0.0f;
  return ok ? dir / l1 : glm::vec3(0.0f);
}

}  // namespace

glm::vec2 OctEncode(glm::vec3 dir) {
  bool ok = false;
  const glm::vec3 n = L1Normalize(dir, ok);
  if (!ok) return glm::vec2(0.5f);
  // Seam branch recovered from the direction, which is all a shader has. AMBIGUOUS
  // on the four lower seams by construction -- see OctFold. Mesh generation must
  // NOT use this form; it knows the face and OctEncodeOnFace takes it.
  return OctFold(n, n.x >= 0.0f ? 1.0f : -1.0f, n.z >= 0.0f ? 1.0f : -1.0f);
}

glm::vec2 OctEncodeOnFace(glm::vec3 on_face, float face_sx, float face_sz) {
  bool ok = false;
  const glm::vec3 n = L1Normalize(on_face, ok);
  if (!ok) return glm::vec2(0.5f);
  // THE FACE decides the branch, so a vertex sitting exactly on a seam gets the
  // value its own face needs. Without this, every lower face whose sx is -1
  // takes the +X face's answer on its n.x == 0 edge: a whole lattice row jumps
  // ~0.77 in u against its neighbours, wrapping the texture across the entire
  // map on a strip two triangles wide. That is exactly the seam artifact the
  // octahedron is here to remove, and it is why the map is not a pure function
  // of direction at generation time.
  return OctFold(n, face_sx, face_sz);
}

namespace {

// One subdivided octahedron face. Corners are unit axis vectors; the lattice is
// generated on the octahedron's FLAT face and normalized afterwards, which is
// what makes the octahedral UV exact rather than fitted -- the map is defined
// on exactly those flat points.
//
// Faces do NOT share vertices. The octahedron's edges are precisely where the
// UV map is discontinuous, so a shared vertex there would have to carry two
// UVs; duplicating along the seams costs ~8% more vertices and removes the
// whole class of seam artifact.
void AppendOctaFace(SceneMesh& mesh, glm::vec3 a, glm::vec3 b, glm::vec3 c,
                    float face_sx, float face_sz, float radius) {
  const uint32_t n = kSphereSubdivisions;
  const uint32_t base = uint32_t(mesh.vertices.size());

  // Row-major triangular lattice: row `i` holds n - i + 1 vertices.
  auto index_of = [&](uint32_t row, uint32_t col) {
    // Sum of the lengths of the preceding rows.
    const uint32_t before = row * (n + 1) - (row > 0 ? (row - 1) * row / 2 : 0);
    return base + before + col;
  };

  for (uint32_t row = 0; row <= n; ++row) {
    for (uint32_t col = 0; col + row <= n; ++col) {
      const float wb = float(row) / float(n);
      const float wc = float(col) / float(n);
      const float wa = 1.0f - wb - wc;
      const glm::vec3 on_face = a * wa + b * wb + c * wc;
      const glm::vec3 normal = glm::normalize(on_face);
      const glm::vec2 uv = OctEncodeOnFace(on_face, face_sx, face_sz);

      MeshVertex v;
      const glm::vec3 p = normal * radius;
      v.pos_nx = {p.x, p.y, p.z, normal.x};
      v.nyz_uv = {normal.y, normal.z, uv.x, uv.y};
      // Filled in below from the UV gradient. A sphere under an octahedral map
      // has no closed-form tangent worth writing out, and deriving it from the
      // triangles is both shorter and correct for any map.
      v.tangent = {0.0f, 0.0f, 0.0f, 1.0f};
      mesh.vertices.push_back(v);
    }
  }

  for (uint32_t row = 0; row < n; ++row) {
    for (uint32_t col = 0; col + row < n; ++col) {
      const uint32_t v0 = index_of(row, col);
      const uint32_t v1 = index_of(row, col + 1);
      const uint32_t v2 = index_of(row + 1, col);
      // (a, b, c) is ordered so the FACE faces outward; the lattice's own
      // orientation is the opposite of the naive (v0, v1, v2), so the pair is
      // spelled out here rather than assumed. The winding test is what settles
      // it -- a reversed sphere still renders as a filled disc.
      mesh.indices.insert(mesh.indices.end(), {v0, v2, v1});
      if (col + row + 1 < n) {
        const uint32_t v3 = index_of(row + 1, col + 1);
        mesh.indices.insert(mesh.indices.end(), {v1, v2, v3});
      }
    }
  }
}

// Per-vertex tangents from the UV gradient, accumulated over the triangles that
// share each vertex and then orthonormalized against the normal.
//
// tangent.w carries the handedness, which three components cannot express --
// negating T inverts the normal map's U response instead of its V. Derived from
// the accumulated bitangent rather than assumed, because the octahedral map's
// lower hemisphere is MIRRORED and therefore has the opposite handedness.
void ComputeTangents(SceneMesh& mesh) {
  std::vector<glm::vec3> tan(mesh.vertices.size(), glm::vec3(0.0f));
  std::vector<glm::vec3> bitan(mesh.vertices.size(), glm::vec3(0.0f));

  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const uint32_t i0 = mesh.indices[t], i1 = mesh.indices[t + 1],
                   i2 = mesh.indices[t + 2];
    const glm::vec3 p0(mesh.vertices[i0].pos_nx);
    const glm::vec3 p1(mesh.vertices[i1].pos_nx);
    const glm::vec3 p2(mesh.vertices[i2].pos_nx);
    const glm::vec2 uv0(mesh.vertices[i0].nyz_uv.z, mesh.vertices[i0].nyz_uv.w);
    const glm::vec2 uv1(mesh.vertices[i1].nyz_uv.z, mesh.vertices[i1].nyz_uv.w);
    const glm::vec2 uv2(mesh.vertices[i2].nyz_uv.z, mesh.vertices[i2].nyz_uv.w);

    const glm::vec3 e1 = p1 - p0, e2 = p2 - p0;
    const glm::vec2 d1 = uv1 - uv0, d2 = uv2 - uv0;
    const float det = d1.x * d2.y - d2.x * d1.y;
    // A degenerate UV triangle contributes nothing rather than a NaN that would
    // poison every vertex it touches.
    if (std::abs(det) < 1e-12f) continue;
    const float r = 1.0f / det;
    const glm::vec3 t_dir = (e1 * d2.y - e2 * d1.y) * r;
    const glm::vec3 b_dir = (e2 * d1.x - e1 * d2.x) * r;
    for (uint32_t i : {i0, i1, i2}) {
      tan[i] += t_dir;
      bitan[i] += b_dir;
    }
  }

  for (size_t i = 0; i < mesh.vertices.size(); ++i) {
    const glm::vec3 n(mesh.vertices[i].pos_nx.w, mesh.vertices[i].nyz_uv.x,
                      mesh.vertices[i].nyz_uv.y);
    glm::vec3 t = tan[i] - n * glm::dot(n, tan[i]);
    if (glm::length(t) < 1e-8f) {
      // No usable gradient here: pick any tangent perpendicular to the normal
      // rather than emitting a zero one, which would collapse the frame.
      const glm::vec3 axis =
          std::abs(n.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
      t = glm::normalize(glm::cross(axis, n));
    } else {
      t = glm::normalize(t);
    }
    const float w = glm::dot(glm::cross(n, t), bitan[i]) < 0.0f ? -1.0f : 1.0f;
    mesh.vertices[i].tangent = {t.x, t.y, t.z, w};
  }
}

}  // namespace

SceneMesh BuildSphereGrid(float sphere_radius, float spacing) {
  SceneMesh mesh;

  // The eight octahedron faces, one per sign combination.
  //
  // THE CORNER ORDER DEPENDS ON THE SIGN PRODUCT. Taking (X, Y, Z) always would
  // wind four of the eight faces inward, and a convex sphere with reversed
  // winding still renders as a filled disc -- you simply see its inside back
  // surface. The image cannot show that, which is why the winding is derived
  // rather than eyeballed and a test asserts it.
  for (int sx = -1; sx <= 1; sx += 2) {
    for (int sy = -1; sy <= 1; sy += 2) {
      for (int sz = -1; sz <= 1; sz += 2) {
        const glm::vec3 x(float(sx), 0.0f, 0.0f);
        const glm::vec3 y(0.0f, float(sy), 0.0f);
        const glm::vec3 z(0.0f, 0.0f, float(sz));
        const float fx = float(sx), fz = float(sz);
        if (sx * sy * sz > 0) {
          AppendOctaFace(mesh, x, y, z, fx, fz, sphere_radius);
        } else {
          AppendOctaFace(mesh, x, z, y, fx, fz, sphere_radius);
        }
      }
    }
  }
  ComputeTangents(mesh);

  // One instance per sphere. The draw slot IS the instance id, which is what
  // the visibility buffer packs, so no per-draw uniform is needed at all.
  mesh.draws.reserve(kSphereCount);
  for (uint32_t row = 0; row < kMetallicSteps; ++row) {
    for (uint32_t col = 0; col < kRoughnessSteps; ++col) {
      DrawInfo d;
      d.offset = glm::vec4(SphereGridCenter(col, row, spacing), 0.0f);
      d.material = glm::vec4(SphereRoughness(col), SphereMetallic(row), 0, 0);
      // BOTH overridden: the point of the chart is a controlled sweep, so the
      // pack's ARM map must not decide either axis.
      d.override_mask = kOverrideRoughness | kOverrideMetallic;
      mesh.draws.push_back(d);
    }
  }
  return mesh;
}

}  // namespace badlands::object_viewer
