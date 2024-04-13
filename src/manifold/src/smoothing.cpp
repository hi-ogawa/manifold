// Copyright 2021 The Manifold Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <map>

#include "impl.h"
#include "par.h"

template <>
struct std::hash<glm::ivec3> {
  size_t operator()(const glm::ivec3& p) const {
    return std::hash<int>()(p.x) ^ std::hash<int>()(p.y) ^
           std::hash<int>()(p.z);
  }
};

namespace {
using namespace manifold;

glm::vec3 OrthogonalTo(glm::vec3 in, glm::vec3 ref) {
  in -= glm::dot(in, ref) * ref;
  return in;
}

/**
 * Retained verts are part of several triangles, and it doesn't matter which one
 * the vertBary refers to. Here, whichever is last will win and it's done on the
 * CPU for simplicity for now. Using AtomicCAS on .tri should work for a GPU
 * version if desired.
 */
void FillRetainedVerts(Vec<Barycentric>& vertBary,
                       const Vec<Halfedge>& halfedge_) {
  const int numTri = halfedge_.size() / 3;
  for (int tri = 0; tri < numTri; ++tri) {
    for (const int i : {0, 1, 2}) {
      glm::vec3 uvw(0);
      uvw[i] = 1;
      vertBary[halfedge_[3 * tri + i].startVert] = {tri, uvw};
    }
  }
}

// Calculate a tangent vector in the form of a weighted cubic Bezier taking as
// input the desired tangent direction (length doesn't matter) and the edge
// vector to the neighboring vertex. In a symmetric situation where the tangents
// at each end are mirror images of each other, this will result in a circular
// arc.
glm::vec4 CircularTangent(const glm::vec3& tangent, const glm::vec3& edgeVec) {
  const glm::vec3 dir = SafeNormalize(tangent);

  float weight = glm::abs(glm::dot(dir, SafeNormalize(edgeVec)));
  if (weight == 0) {
    weight = 1;
  }
  // Quadratic weighted bezier for circular interpolation
  const glm::vec4 bz2 =
      weight * glm::vec4(dir * glm::length(edgeVec) / (2 * weight), 1);
  // Equivalent cubic weighted bezier
  const glm::vec4 bz3 = glm::mix(glm::vec4(0, 0, 0, 1), bz2, 2 / 3.0f);
  // Convert from homogeneous form to geometric form
  return glm::vec4(glm::vec3(bz3) / bz3.w, bz3.w);
}

struct ReindexHalfedge {
  VecView<int> half2Edge;
  const VecView<Halfedge> halfedges;

  void operator()(thrust::tuple<int, TmpEdge> in) {
    const int edge = thrust::get<0>(in);
    const int halfedge = thrust::get<1>(in).halfedgeIdx;

    half2Edge[halfedge] = edge;
    half2Edge[halfedges[halfedge].pairedHalfedge] = edge;
  }
};

struct SmoothBezier {
  VecView<const glm::vec3> vertPos;
  VecView<const glm::vec3> triNormal;
  VecView<const glm::vec3> vertNormal;
  VecView<const Halfedge> halfedge;

  void operator()(thrust::tuple<glm::vec4&, Halfedge> inOut) {
    glm::vec4& tangent = thrust::get<0>(inOut);
    const Halfedge edge = thrust::get<1>(inOut);

    const glm::vec3 edgeVec = vertPos[edge.endVert] - vertPos[edge.startVert];
    const glm::vec3 edgeNormal =
        (triNormal[edge.face] + triNormal[halfedge[edge.pairedHalfedge].face]) /
        2.0f;
    glm::vec3 dir =
        glm::cross(glm::cross(edgeNormal, edgeVec), vertNormal[edge.startVert]);
    tangent = CircularTangent(dir, edgeVec);
  }
};

struct InterpTri {
  VecView<const Halfedge> halfedge;
  VecView<const glm::vec4> halfedgeTangent;
  VecView<const glm::vec3> vertPos;

  glm::vec4 Homogeneous(glm::vec4 v) const {
    v.x *= v.w;
    v.y *= v.w;
    v.z *= v.w;
    return v;
  }

  glm::vec4 Homogeneous(glm::vec3 v) const { return glm::vec4(v, 1.0f); }

  glm::vec3 HNormalize(glm::vec4 v) const { return glm::vec3(v) / v.w; }

  glm::vec4 Bezier(glm::vec3 point, glm::vec4 tangent) const {
    return Homogeneous(glm::vec4(point, 0) + tangent);
  }

  glm::mat2x4 CubicBezier2Linear(glm::vec4 p0, glm::vec4 p1, glm::vec4 p2,
                                 glm::vec4 p3, float x) const {
    glm::mat2x4 out;
    glm::vec4 p12 = glm::mix(p1, p2, x);
    out[0] = glm::mix(glm::mix(p0, p1, x), p12, x);
    out[1] = glm::mix(p12, glm::mix(p2, p3, x), x);
    return out;
  }

  glm::vec3 BezierPoint(glm::mat2x4 points, float x) const {
    return HNormalize(glm::mix(points[0], points[1], x));
  }

  glm::vec3 BezierTangent(glm::mat2x4 points) const {
    return glm::normalize(HNormalize(points[1]) - HNormalize(points[0]));
  }

  void operator()(thrust::tuple<glm::vec3&, Barycentric> inOut) {
    glm::vec3& pos = thrust::get<0>(inOut);
    const int tri = thrust::get<1>(inOut).tri;
    const glm::vec3 uvw = thrust::get<1>(inOut).uvw;

    glm::vec4 posH(0);
    const glm::mat3 corners = {vertPos[halfedge[3 * tri].startVert],
                               vertPos[halfedge[3 * tri + 1].startVert],
                               vertPos[halfedge[3 * tri + 2].startVert]};

    for (const int i : {0, 1, 2}) {
      if (uvw[i] == 1) {
        pos = glm::vec3(corners[i]);
        return;
      }
    }

    const glm::mat3x4 tangentR = {halfedgeTangent[3 * tri],
                                  halfedgeTangent[3 * tri + 1],
                                  halfedgeTangent[3 * tri + 2]};
    const glm::mat3x4 tangentL = {
        halfedgeTangent[halfedge[3 * tri + 2].pairedHalfedge],
        halfedgeTangent[halfedge[3 * tri].pairedHalfedge],
        halfedgeTangent[halfedge[3 * tri + 1].pairedHalfedge]};

    for (const int i : {0, 1, 2}) {
      const int j = (i + 1) % 3;
      const int k = (i + 2) % 3;
      const float x = uvw[k] / (1 - uvw[i]);

      const glm::mat2x4 bez = CubicBezier2Linear(
          Homogeneous(corners[j]), Bezier(corners[j], tangentR[j]),
          Bezier(corners[k], tangentL[k]), Homogeneous(corners[k]), x);
      const glm::vec3 end = BezierPoint(bez, x);
      const glm::vec3 tangent = BezierTangent(bez);

      const glm::vec3 jBitangent = SafeNormalize(OrthogonalTo(
          glm::vec3(tangentL[j]), SafeNormalize(glm::vec3(tangentR[j]))));
      const glm::vec3 kBitangent = SafeNormalize(OrthogonalTo(
          glm::vec3(tangentR[k]), -SafeNormalize(glm::vec3(tangentL[k]))));
      const glm::vec3 normal = SafeNormalize(
          glm::cross(glm::mix(jBitangent, kBitangent, x), tangent));
      const glm::vec3 delta = OrthogonalTo(
          glm::mix(glm::vec3(tangentL[j]), glm::vec3(tangentR[k]), x), normal);
      const float deltaW = glm::mix(tangentL[j].w, tangentR[k].w, x);

      const glm::mat2x4 bez1 = CubicBezier2Linear(
          Homogeneous(end), Homogeneous(glm::vec4(end + delta, deltaW)),
          Bezier(corners[i], glm::mix(tangentR[i], tangentL[i], x)),
          Homogeneous(corners[i]), uvw[i]);
      const glm::vec3 p = BezierPoint(bez1, uvw[i]);
      float w = uvw[j] * uvw[j] * uvw[k] * uvw[k];
      posH += Homogeneous(glm::vec4(p, w));
    }
    pos = HNormalize(posH);
  }
};

class Partition {
 public:
  // The cached partitions don't have idx - it's added to the copy returned
  // from GetPartition that contains the mapping of the input divisions into the
  // sorted divisions that are uniquely cached.
  glm::ivec3 idx;
  glm::ivec3 sortedDivisions;
  Vec<glm::vec3> vertBary;
  Vec<glm::ivec3> triVert;

  int InteriorOffset() const {
    return sortedDivisions[0] + sortedDivisions[1] + sortedDivisions[2];
  }

  int NumInterior() const { return vertBary.size() - InteriorOffset(); }

  static Partition GetPartition(glm::ivec3 divisions) {
    glm::ivec3 sortedDiv = divisions;
    glm::ivec3 triIdx = {0, 1, 2};
    if (sortedDiv[2] > sortedDiv[1]) {
      std::swap(sortedDiv[2], sortedDiv[1]);
      std::swap(triIdx[2], triIdx[1]);
    }
    if (sortedDiv[1] > sortedDiv[0]) {
      std::swap(sortedDiv[1], sortedDiv[0]);
      std::swap(triIdx[1], triIdx[0]);
      if (sortedDiv[2] > sortedDiv[1]) {
        std::swap(sortedDiv[2], sortedDiv[1]);
        std::swap(triIdx[2], triIdx[1]);
      }
    }

    Partition partition = GetCachedPartition(sortedDiv);
    partition.idx = triIdx;

    return partition;
  }

  Vec<glm::ivec3> Reindex(glm::ivec3 tri, glm::ivec3 edgeOffsets,
                          glm::bvec3 edgeFwd, int interiorOffset) const {
    Vec<int> newVerts;
    newVerts.reserve(vertBary.size());
    glm::ivec3 triIdx = idx;
    glm::ivec3 outTri = {0, 1, 2};
    if (idx[1] != Next3(idx[0])) {
      triIdx = {idx[2], idx[0], idx[1]};
      edgeFwd = glm::not_(edgeFwd);
      std::swap(outTri[0], outTri[1]);
    }
    for (const int i : {0, 1, 2}) {
      newVerts.push_back(tri[triIdx[i]]);
    }
    for (const int i : {0, 1, 2}) {
      const int n = sortedDivisions[i] - 1;
      int offset = edgeOffsets[idx[i]] + (edgeFwd[idx[i]] ? 0 : n - 1);
      for (int j = 0; j < n; ++j) {
        newVerts.push_back(offset);
        offset += edgeFwd[idx[i]] ? 1 : -1;
      }
    }
    const int offset = interiorOffset - newVerts.size();
    for (int i = newVerts.size(); i < vertBary.size(); ++i) {
      newVerts.push_back(i + offset);
    }

    const int numTri = triVert.size();
    Vec<glm::ivec3> newTriVert(numTri);
    for_each_n(
        autoPolicy(numTri), zip(newTriVert.begin(), triVert.begin()), numTri,
        [&outTri, &newVerts](thrust::tuple<glm::ivec3&, glm::ivec3> inOut) {
          for (const int j : {0, 1, 2}) {
            thrust::get<0>(inOut)[outTri[j]] =
                newVerts[thrust::get<1>(inOut)[j]];
          }
        });
    return newTriVert;
  }

 private:
  static inline auto cacheLock = std::mutex();
  static inline auto cache =
      std::unordered_map<glm::ivec3, std::unique_ptr<Partition>>();

  // This triangulation is purely topological - it depends only on the number of
  // divisions of the three sides of the triangle. This allows them to be cached
  // and reused for similar triangles. The shape of the final surface is defined
  // by the tangents and the barycentric coordinates of the new verts. The input
  // must be sorted: n[0] >= n[1] >= n[2] > 0
  static Partition GetCachedPartition(glm::ivec3 n) {
    {
      auto lockGuard = std::lock_guard<std::mutex>(cacheLock);
      auto cached = cache.find(n);
      if (cached != cache.end()) {
        return *cached->second;
      }
    }
    Partition partition;
    partition.sortedDivisions = n;
    partition.vertBary.push_back({1, 0, 0});
    partition.vertBary.push_back({0, 1, 0});
    partition.vertBary.push_back({0, 0, 1});
    for (const int i : {0, 1, 2}) {
      const glm::vec3 nextBary = partition.vertBary[(i + 1) % 3];
      for (int j = 1; j < n[i]; ++j) {
        partition.vertBary.push_back(
            glm::mix(partition.vertBary[i], nextBary, (float)j / n[i]));
      }
    }
    const glm::ivec3 edgeOffsets = {3, 3 + n[0] - 1, 3 + n[0] - 1 + n[1] - 1};

    const float f = n[2] * n[2] + n[0] * n[0];
    if (n[1] == 1) {
      if (n[0] == 1) {
        partition.triVert.push_back({0, 1, 2});
      } else {
        PartitionFan(partition.triVert, {0, 1, 2}, n[0] - 1, edgeOffsets[0]);
      }
    } else if (n[1] * n[1] > f - glm::sqrt(2.0f) * n[0] * n[2]) {  // acute-ish
      partition.triVert.push_back({edgeOffsets[1] - 1, 1, edgeOffsets[1]});
      PartitionQuad(partition.triVert, partition.vertBary,
                    {edgeOffsets[1] - 1, edgeOffsets[1], 2, 0},
                    {-1, edgeOffsets[1] + 1, edgeOffsets[2], edgeOffsets[0]},
                    {0, n[1] - 2, n[2] - 1, n[0] - 2},
                    {true, true, true, true});
    } else {  // obtuse -> spit into two acute
      // portion of n[0] under n[2]
      const int ns =
          glm::min(n[0] - 2, (int)glm::round((f - n[1] * n[1]) / (2 * n[0])));
      // height from n[0]: nh <= n[2]
      const int nh = glm::max(1., glm::round(glm::sqrt(n[2] * n[2] - ns * ns)));

      const int hOffset = partition.vertBary.size();
      const glm::vec3 middleBary = partition.vertBary[edgeOffsets[0] + ns - 1];
      for (int j = 1; j < nh; ++j) {
        partition.vertBary.push_back(
            glm::mix(partition.vertBary[2], middleBary, (float)j / nh));
      }

      partition.triVert.push_back({edgeOffsets[1] - 1, 1, edgeOffsets[1]});
      PartitionQuad(
          partition.triVert, partition.vertBary,
          {edgeOffsets[1] - 1, edgeOffsets[1], 2, edgeOffsets[0] + ns - 1},
          {-1, edgeOffsets[1] + 1, hOffset, edgeOffsets[0] + ns},
          {0, n[1] - 2, nh - 1, n[0] - ns - 2}, {true, true, true, true});

      if (n[2] == 1) {
        PartitionFan(partition.triVert, {0, edgeOffsets[0] + ns - 1, 2}, ns - 1,
                     edgeOffsets[0]);
      } else {
        if (ns == 1) {
          partition.triVert.push_back({hOffset, 2, edgeOffsets[2]});
          PartitionQuad(partition.triVert, partition.vertBary,
                        {hOffset, edgeOffsets[2], 0, edgeOffsets[0]},
                        {-1, edgeOffsets[2] + 1, -1, hOffset + nh - 2},
                        {0, n[2] - 2, ns - 1, nh - 2},
                        {true, true, true, false});
        } else {
          partition.triVert.push_back({hOffset - 1, 0, edgeOffsets[0]});
          PartitionQuad(
              partition.triVert, partition.vertBary,
              {hOffset - 1, edgeOffsets[0], edgeOffsets[0] + ns - 1, 2},
              {-1, edgeOffsets[0] + 1, hOffset + nh - 2, edgeOffsets[2]},
              {0, ns - 2, nh - 1, n[2] - 2}, {true, true, false, true});
        }
      }
    }

    auto lockGuard = std::lock_guard<std::mutex>(cacheLock);
    cache.insert({n, std::make_unique<Partition>(partition)});
    return partition;
  }

  // Side 0 has added edges while sides 1 and 2 do not. Fan spreads from vert 2.
  static void PartitionFan(Vec<glm::ivec3>& triVert, glm::ivec3 cornerVerts,
                           int added, int edgeOffset) {
    int last = cornerVerts[0];
    for (int i = 0; i < added; ++i) {
      const int next = edgeOffset + i;
      triVert.push_back({last, next, cornerVerts[2]});
      last = next;
    }
    triVert.push_back({last, cornerVerts[1], cornerVerts[2]});
  }

  // Partitions are parallel to the first edge unless two consecutive edgeAdded
  // are zero, in which case a terminal triangulation is performed.
  static void PartitionQuad(Vec<glm::ivec3>& triVert, Vec<glm::vec3>& vertBary,
                            glm::ivec4 cornerVerts, glm::ivec4 edgeOffsets,
                            glm::ivec4 edgeAdded, glm::bvec4 edgeFwd) {
    auto GetEdgeVert = [&](int edge, int idx) {
      return edgeOffsets[edge] + (edgeFwd[edge] ? 1 : -1) * idx;
    };

    ASSERT(glm::all(glm::greaterThanEqual(edgeAdded, glm::ivec4(0))), logicErr,
           "negative divisions!");

    int corner = -1;
    int last = 3;
    int maxEdge = -1;
    for (const int i : {0, 1, 2, 3}) {
      if (corner == -1 && edgeAdded[i] == 0 && edgeAdded[last] == 0) {
        corner = i;
      }
      if (edgeAdded[i] > 0) {
        maxEdge = maxEdge == -1 ? i : -2;
      }
      last = i;
    }
    if (corner >= 0) {  // terminate
      if (maxEdge >= 0) {
        glm::ivec4 edge = (glm::ivec4(0, 1, 2, 3) + maxEdge) % 4;
        const int middle = edgeAdded[maxEdge] / 2;
        triVert.push_back({cornerVerts[edge[2]], cornerVerts[edge[3]],
                           GetEdgeVert(maxEdge, middle)});
        int last = cornerVerts[edge[0]];
        for (int i = 0; i <= middle; ++i) {
          const int next = GetEdgeVert(maxEdge, i);
          triVert.push_back({cornerVerts[edge[3]], last, next});
          last = next;
        }
        last = cornerVerts[edge[1]];
        for (int i = edgeAdded[maxEdge] - 1; i >= middle; --i) {
          const int next = GetEdgeVert(maxEdge, i);
          triVert.push_back({cornerVerts[edge[2]], next, last});
          last = next;
        }
      } else {
        int sideVert = cornerVerts[0];  // initial value is unused
        for (const int j : {1, 2}) {
          const int side = (corner + j) % 4;
          if (j == 2 && edgeAdded[side] > 0) {
            triVert.push_back(
                {cornerVerts[side], GetEdgeVert(side, 0), sideVert});
          } else {
            sideVert = cornerVerts[side];
          }
          for (int i = 0; i < edgeAdded[side]; ++i) {
            const int nextVert = GetEdgeVert(side, i);
            triVert.push_back({cornerVerts[corner], sideVert, nextVert});
            sideVert = nextVert;
          }
          if (j == 2 || edgeAdded[side] == 0) {
            triVert.push_back({cornerVerts[corner], sideVert,
                               cornerVerts[(corner + j + 1) % 4]});
          }
        }
      }
      return;
    }
    // recursively partition
    const int partitions = 1 + glm::min(edgeAdded[1], edgeAdded[3]);
    glm::ivec4 newCornerVerts = {cornerVerts[1], -1, -1, cornerVerts[0]};
    glm::ivec4 newEdgeOffsets = {
        edgeOffsets[1], -1, GetEdgeVert(3, edgeAdded[3] + 1), edgeOffsets[0]};
    glm::ivec4 newEdgeAdded = {0, -1, 0, edgeAdded[0]};
    glm::bvec4 newEdgeFwd = {edgeFwd[1], true, edgeFwd[3], edgeFwd[0]};

    for (int i = 1; i < partitions; ++i) {
      const int cornerOffset1 = (edgeAdded[1] * i) / partitions;
      const int cornerOffset3 =
          edgeAdded[3] - 1 - (edgeAdded[3] * i) / partitions;
      const int nextOffset1 = GetEdgeVert(1, cornerOffset1 + 1);
      const int nextOffset3 = GetEdgeVert(3, cornerOffset3 + 1);
      const int added = glm::round(glm::mix(
          (float)edgeAdded[0], (float)edgeAdded[2], (float)i / partitions));

      newCornerVerts[1] = GetEdgeVert(1, cornerOffset1);
      newCornerVerts[2] = GetEdgeVert(3, cornerOffset3);
      newEdgeAdded[0] = std::abs(nextOffset1 - newEdgeOffsets[0]) - 1;
      newEdgeAdded[1] = added;
      newEdgeAdded[2] = std::abs(nextOffset3 - newEdgeOffsets[2]) - 1;
      newEdgeOffsets[1] = vertBary.size();
      newEdgeOffsets[2] = nextOffset3;

      for (int j = 0; j < added; ++j) {
        vertBary.push_back(glm::mix(vertBary[newCornerVerts[1]],
                                    vertBary[newCornerVerts[2]],
                                    (j + 1.0f) / (added + 1.0f)));
      }

      PartitionQuad(triVert, vertBary, newCornerVerts, newEdgeOffsets,
                    newEdgeAdded, newEdgeFwd);

      newCornerVerts[0] = newCornerVerts[1];
      newCornerVerts[3] = newCornerVerts[2];
      newEdgeAdded[3] = newEdgeAdded[1];
      newEdgeOffsets[0] = nextOffset1;
      newEdgeOffsets[3] = newEdgeOffsets[1] + newEdgeAdded[1] - 1;
      newEdgeFwd[3] = false;
    }

    newCornerVerts[1] = cornerVerts[2];
    newCornerVerts[2] = cornerVerts[3];
    newEdgeOffsets[1] = edgeOffsets[2];
    newEdgeAdded[0] =
        edgeAdded[1] - std::abs(newEdgeOffsets[0] - edgeOffsets[1]);
    newEdgeAdded[1] = edgeAdded[2];
    newEdgeAdded[2] = std::abs(newEdgeOffsets[2] - edgeOffsets[3]) - 1;
    newEdgeOffsets[2] = edgeOffsets[3];
    newEdgeFwd[1] = edgeFwd[2];

    PartitionQuad(triVert, vertBary, newCornerVerts, newEdgeOffsets,
                  newEdgeAdded, newEdgeFwd);
  }
};
}  // namespace

namespace manifold {

glm::vec3 Manifold::Impl::GetNormal(int halfedge, int normalIdx) const {
  const int tri = halfedge / 3;
  const int j = halfedge % 3;
  const int prop = meshRelation_.triProperties[tri][j];
  glm::vec3 normal;
  for (const int i : {0, 1, 2}) {
    normal[i] =
        meshRelation_.properties[prop * meshRelation_.numProp + normalIdx + i];
  }
  return normal;
}

// sharpenedEdges are referenced to the input Mesh, but the triangles have
// been sorted in creating the Manifold, so the indices are converted using
// meshRelation_.
std::vector<Smoothness> Manifold::Impl::UpdateSharpenedEdges(
    const std::vector<Smoothness>& sharpenedEdges) const {
  std::unordered_map<int, int> oldHalfedge2New;
  for (int tri = 0; tri < NumTri(); ++tri) {
    int oldTri = meshRelation_.triRef[tri].tri;
    for (int i : {0, 1, 2}) oldHalfedge2New[3 * oldTri + i] = 3 * tri + i;
  }
  std::vector<Smoothness> newSharp = sharpenedEdges;
  for (Smoothness& edge : newSharp) {
    edge.halfedge = oldHalfedge2New[edge.halfedge];
  }
  return newSharp;
}

// Find faces containing at least 3 triangles - these will not have
// interpolated normals - all their vert normals must match their face normal.
Vec<bool> Manifold::Impl::FlatFaces() const {
  const int numTri = NumTri();
  Vec<bool> triIsFlatFace(numTri, false);
  for_each_n(
      autoPolicy(numTri), countAt(0), numTri,
      [this, &triIsFlatFace](const int tri) {
        const TriRef& ref = this->meshRelation_.triRef[tri];
        int faceNeighbors = 0;
        glm::ivec3 faceTris = {-1, -1, -1};
        for (const int j : {0, 1, 2}) {
          const int neighborTri =
              this->halfedge_[this->halfedge_[3 * tri + j].pairedHalfedge].face;
          const TriRef& jRef = this->meshRelation_.triRef[neighborTri];
          if (jRef.SameFace(ref)) {
            ++faceNeighbors;
            faceTris[j] = neighborTri;
          }
        }
        if (faceNeighbors > 1) {
          triIsFlatFace[tri] = true;
          for (const int j : {0, 1, 2}) {
            if (faceTris[j] >= 0) {
              triIsFlatFace[faceTris[j]] = true;
            }
          }
        }
      });
  return triIsFlatFace;
}

// Returns a vector of length numVert that has a tri that is part of a
// neighboring flat face if there is only one flat face. If there are none it
// gets -1, and if there are more than one it gets -2.
Vec<int> Manifold::Impl::VertFlatFace(const Vec<bool>& flatFaces) const {
  Vec<int> vertFlatFace(NumVert(), -1);
  Vec<TriRef> vertRef(NumVert(), {-1, -1, -1});
  for (int tri = 0; tri < NumTri(); ++tri) {
    if (flatFaces[tri]) {
      for (const int j : {0, 1, 2}) {
        const int vert = halfedge_[3 * tri + j].startVert;
        if (vertRef[vert].SameFace(meshRelation_.triRef[tri])) continue;
        vertRef[vert] = meshRelation_.triRef[tri];
        vertFlatFace[vert] = vertFlatFace[vert] == -1 ? tri : -2;
      }
    }
  }
  return vertFlatFace;
}

std::vector<Smoothness> Manifold::Impl::SharpenEdges(
    float minSharpAngle, float minSmoothness) const {
  std::vector<Smoothness> sharpenedEdges;
  const float minRadians = glm::radians(minSharpAngle);
  for (int e = 0; e < halfedge_.size(); ++e) {
    if (!halfedge_[e].IsForward()) continue;
    const int pair = halfedge_[e].pairedHalfedge;
    const float dihedral =
        glm::acos(glm::dot(faceNormal_[e / 3], faceNormal_[pair / 3]));
    if (dihedral > minRadians) {
      sharpenedEdges.push_back({e, minSmoothness});
      sharpenedEdges.push_back({pair, minSmoothness});
    }
  }
  return sharpenedEdges;
}

/**
 * Instead of calculating the internal shared normals like CalculateNormals
 * does, this method fills in vertex properties, unshared across edges that
 * are bent more than minSharpAngle.
 */
void Manifold::Impl::SetNormals(int normalIdx, float minSharpAngle) {
  if (IsEmpty()) return;
  if (normalIdx < 0) return;

  const int oldNumProp = NumProp();
  const int numTri = NumTri();

  Vec<bool> triIsFlatFace = FlatFaces();
  Vec<int> vertFlatFace = VertFlatFace(triIsFlatFace);
  Vec<int> vertNumSharp(NumVert(), 0);
  for (int e = 0; e < halfedge_.size(); ++e) {
    if (!halfedge_[e].IsForward()) continue;
    const int pair = halfedge_[e].pairedHalfedge;
    const int tri1 = e / 3;
    const int tri2 = pair / 3;
    const float dihedral =
        glm::degrees(glm::acos(glm::dot(faceNormal_[tri1], faceNormal_[tri2])));
    if (dihedral > minSharpAngle) {
      ++vertNumSharp[halfedge_[e].startVert];
      ++vertNumSharp[halfedge_[e].endVert];
    } else {
      const bool faceSplit =
          triIsFlatFace[tri1] != triIsFlatFace[tri2] ||
          (triIsFlatFace[tri1] && triIsFlatFace[tri2] &&
           !meshRelation_.triRef[tri1].SameFace(meshRelation_.triRef[tri2]));
      if (vertFlatFace[halfedge_[e].startVert] == -2 && faceSplit) {
        ++vertNumSharp[halfedge_[e].startVert];
      }
      if (vertFlatFace[halfedge_[e].endVert] == -2 && faceSplit) {
        ++vertNumSharp[halfedge_[e].endVert];
      }
    }
  }

  const int numProp = glm::max(oldNumProp, normalIdx + 3);
  Vec<float> oldProperties(numProp * NumPropVert(), 0);
  meshRelation_.properties.swap(oldProperties);
  meshRelation_.numProp = numProp;
  if (meshRelation_.triProperties.size() == 0) {
    meshRelation_.triProperties.resize(numTri);
    for_each_n(autoPolicy(numTri), countAt(0), numTri, [this](int tri) {
      for (const int j : {0, 1, 2})
        this->meshRelation_.triProperties[tri][j] =
            this->halfedge_[3 * tri + j].startVert;
    });
  }
  Vec<glm::ivec3> oldTriProp(numTri, {-1, -1, -1});
  meshRelation_.triProperties.swap(oldTriProp);

  for (int tri = 0; tri < numTri; ++tri) {
    for (const int i : {0, 1, 2}) {
      if (meshRelation_.triProperties[tri][i] >= 0) continue;
      int current = 3 * tri + i;
      const int startEdge = current;
      const int vert = halfedge_[current].startVert;

      if (vertNumSharp[vert] < 2) {
        const glm::vec3 normal = vertFlatFace[vert] >= 0
                                     ? faceNormal_[vertFlatFace[vert]]
                                     : vertNormal_[vert];
        int lastProp = -1;
        do {
          current = NextHalfedge(halfedge_[current].pairedHalfedge);
          const int thisTri = current / 3;
          const int j = current - 3 * thisTri;
          const int prop = oldTriProp[thisTri][j];
          meshRelation_.triProperties[thisTri][j] = prop;
          if (prop == lastProp) continue;
          lastProp = prop;
          auto start = oldProperties.begin() + prop * oldNumProp;
          std::copy(start, start + oldNumProp,
                    meshRelation_.properties.begin() + prop * numProp);
          for (const int i : {0, 1, 2})
            meshRelation_.properties[prop * numProp + normalIdx + i] =
                normal[i];
        } while (current != startEdge);
      } else {
        const glm::vec3 centerPos = vertPos_[vert];
        // Length degree
        std::vector<int> group;
        // Length number of normals
        std::vector<glm::vec3> normals;
        int prevFace = halfedge_[current].face;

        do {
          int next = NextHalfedge(halfedge_[current].pairedHalfedge);
          const int face = halfedge_[next].face;

          const float dihedral = glm::degrees(
              glm::acos(glm::dot(faceNormal_[face], faceNormal_[prevFace])));
          if (dihedral > minSharpAngle ||
              triIsFlatFace[face] != triIsFlatFace[prevFace] ||
              (triIsFlatFace[face] && triIsFlatFace[prevFace] &&
               !meshRelation_.triRef[face].SameFace(
                   meshRelation_.triRef[prevFace]))) {
            break;
          }
          current = next;
          prevFace = face;
        } while (current != startEdge);

        const int endEdge = current;
        glm::vec3 prevEdgeVec =
            glm::normalize(vertPos_[halfedge_[current].endVert] - centerPos);

        do {
          current = NextHalfedge(halfedge_[current].pairedHalfedge);
          const int face = halfedge_[current].face;

          const float dihedral = glm::degrees(
              glm::acos(glm::dot(faceNormal_[face], faceNormal_[prevFace])));
          if (dihedral > minSharpAngle ||
              triIsFlatFace[face] != triIsFlatFace[prevFace] ||
              (triIsFlatFace[face] && triIsFlatFace[prevFace] &&
               !meshRelation_.triRef[face].SameFace(
                   meshRelation_.triRef[prevFace]))) {
            normals.push_back(glm::vec3(0));
          }
          group.push_back(normals.size() - 1);

          const glm::vec3 edgeVec =
              glm::normalize(vertPos_[halfedge_[current].endVert] - centerPos);
          float dot = glm::dot(prevEdgeVec, edgeVec);
          const float phi =
              dot >= 1 ? 0 : (dot <= -1 ? glm::pi<float>() : glm::acos(dot));
          normals.back() += faceNormal_[face] * phi;

          prevFace = face;
          prevEdgeVec = edgeVec;
        } while (current != endEdge);

        for (auto& normal : normals) {
          normal = glm::normalize(normal);
        }

        int lastGroup = 0;
        int lastProp = -1;
        int newProp = -1;
        int idx = 0;
        do {
          current = NextHalfedge(halfedge_[current].pairedHalfedge);
          const int thisTri = current / 3;
          const int j = current - 3 * thisTri;
          const int prop = oldTriProp[thisTri][j];
          auto start = oldProperties.begin() + prop * oldNumProp;

          if (group[idx] != lastGroup && group[idx] != 0 && prop == lastProp) {
            lastGroup = group[idx];
            newProp = NumPropVert();
            meshRelation_.properties.resize(meshRelation_.properties.size() +
                                            numProp);
            std::copy(start, start + oldNumProp,
                      meshRelation_.properties.begin() + newProp * numProp);
            for (const int i : {0, 1, 2}) {
              meshRelation_.properties[newProp * numProp + normalIdx + i] =
                  normals[group[idx]][i];
            }
          } else if (prop != lastProp) {
            lastProp = prop;
            newProp = prop;
            std::copy(start, start + oldNumProp,
                      meshRelation_.properties.begin() + prop * numProp);
            for (const int i : {0, 1, 2})
              meshRelation_.properties[prop * numProp + normalIdx + i] =
                  normals[group[idx]][i];
          }

          meshRelation_.triProperties[thisTri][j] = newProp;
          ++idx;
        } while (current != endEdge);
      }
    }
  }
}

/**
 * Calculates halfedgeTangent_, allowing the manifold to be refined and
 * smoothed. The tangents form weighted cubic Beziers along each edge. This
 * function creates circular arcs where possible (minimizing maximum curvature),
 * constrained to the indicated property normals. Across edges that form
 * discontinuities in the normals, the tangent vectors are zero-length, allowing
 * the shape to form a sharp corner with minimal oscillation.
 */
void Manifold::Impl::CreateTangents(int normalIdx) {
  ZoneScoped;
  const int numVert = NumVert();
  const int numHalfedge = halfedge_.size();
  halfedgeTangent_.resize(numHalfedge);

  Vec<glm::vec3> vertNormal(numVert);
  Vec<glm::ivec2> vertSharpHalfedge(numVert, glm::ivec2(-1));
  for (int e = 0; e < numHalfedge; ++e) {
    const int vert = halfedge_[e].startVert;
    auto& sharpHalfedge = vertSharpHalfedge[vert];
    if (sharpHalfedge[0] >= 0 && sharpHalfedge[1] >= 0) continue;

    int idx = 0;
    // Only used when there is only one.
    glm::vec3& lastNormal = vertNormal[vert];

    ForVert<glm::vec3>(
        e,
        [normalIdx, this](int halfedge) {
          return GetNormal(halfedge, normalIdx);
        },
        [&sharpHalfedge, &idx, &lastNormal](int halfedge,
                                            const glm::vec3& normal,
                                            const glm::vec3& nextNormal) {
          const glm::vec3 diff = nextNormal - normal;
          if (glm::dot(diff, diff) > kTolerance * kTolerance) {
            if (idx > 1) {
              sharpHalfedge[0] = -1;
            } else {
              sharpHalfedge[idx++] = halfedge;
            }
          }
          lastNormal = normal;
        });
  }

  for_each_n(autoPolicy(numHalfedge),
             zip(halfedgeTangent_.begin(), halfedge_.cbegin()), numHalfedge,
             SmoothBezier({vertPos_, faceNormal_, vertNormal, halfedge_}));

  for (int vert = 0; vert < numVert; ++vert) {
    const int first = vertSharpHalfedge[vert][0];
    const int second = vertSharpHalfedge[vert][1];
    if (second == -1) continue;
    if (first != -1) {  // Make continuous edge
      const glm::vec3 newTangent = glm::normalize(glm::cross(
          GetNormal(first, normalIdx), GetNormal(second, normalIdx)));
      if (!isfinite(newTangent[0])) continue;

      halfedgeTangent_[first] = CircularTangent(
          newTangent, vertPos_[halfedge_[first].endVert] - vertPos_[vert]);
      halfedgeTangent_[second] = CircularTangent(
          -newTangent, vertPos_[halfedge_[second].endVert] - vertPos_[vert]);

      ForVert(first, [this, first, second](int current) {
        if (current != first && current != second) {
          halfedgeTangent_[current] = glm::vec4(0);
        }
      });
    } else {  // Sharpen vertex uniformly
      int current = first;
      do {
        halfedgeTangent_[current] = glm::vec4(0);
        current = NextHalfedge(halfedge_[current].pairedHalfedge);
      } while (current != first);
    }
  }
}

/**
 * Calculates halfedgeTangent_, allowing the manifold to be refined and
 * smoothed. The tangents form weighted cubic Beziers along each edge. This
 * function creates circular arcs where possible (minimizing maximum curvature),
 * constrained to the vertex normals. Where sharpenedEdges are specified, the
 * tangents are shortened that intersect the sharpened edge, concentrating the
 * curvature there, while the tangents of the sharp edges themselves are aligned
 * for continuity.
 */
void Manifold::Impl::CreateTangents(std::vector<Smoothness> sharpenedEdges) {
  ZoneScoped;
  const int numHalfedge = halfedge_.size();
  halfedgeTangent_.resize(numHalfedge);

  Vec<bool> triIsFlatFace = FlatFaces();
  Vec<int> vertFlatFace = VertFlatFace(triIsFlatFace);
  Vec<glm::vec3> vertNormal = vertNormal_;
  for (int v = 0; v < NumVert(); ++v) {
    if (vertFlatFace[v] >= 0) {
      vertNormal[v] = faceNormal_[vertFlatFace[v]];
    }
  }

  for_each_n(autoPolicy(numHalfedge),
             zip(halfedgeTangent_.begin(), halfedge_.cbegin()), numHalfedge,
             SmoothBezier({vertPos_, faceNormal_, vertNormal, halfedge_}));

  // Add sharpened edges around faces, just on the face side.
  for (int tri = 0; tri < NumTri(); ++tri) {
    if (!triIsFlatFace[tri]) continue;
    for (const int j : {0, 1, 2}) {
      const int tri2 = halfedge_[3 * tri + j].pairedHalfedge / 3;
      if (!triIsFlatFace[tri2] ||
          !meshRelation_.triRef[tri].SameFace(meshRelation_.triRef[tri2])) {
        sharpenedEdges.push_back({3 * tri + j, 0});
      }
    }
  }

  if (sharpenedEdges.empty()) return;

  using Pair = std::pair<Smoothness, Smoothness>;
  // Fill in missing pairs with default smoothness = 1.
  std::map<int, Pair> edges;
  for (Smoothness edge : sharpenedEdges) {
    if (edge.smoothness >= 1) continue;
    const bool forward = halfedge_[edge.halfedge].IsForward();
    const int pair = halfedge_[edge.halfedge].pairedHalfedge;
    const int idx = forward ? edge.halfedge : pair;
    if (edges.find(idx) == edges.end()) {
      edges[idx] = {edge, {pair, 1}};
      if (!forward) std::swap(edges[idx].first, edges[idx].second);
    } else {
      Smoothness& e = forward ? edges[idx].first : edges[idx].second;
      e.smoothness = glm::min(edge.smoothness, e.smoothness);
    }
  }

  std::map<int, std::vector<Pair>> vertTangents;
  for (const auto& value : edges) {
    const Pair edge = value.second;
    vertTangents[halfedge_[edge.first.halfedge].startVert].push_back(edge);
    vertTangents[halfedge_[edge.second.halfedge].startVert].push_back(
        {edge.second, edge.first});
  }

  Vec<glm::vec4>& tangent = halfedgeTangent_;
  for (const auto& value : vertTangents) {
    const std::vector<Pair>& vert = value.second;
    // Sharp edges that end are smooth at their terminal vert.
    if (vert.size() == 1) continue;
    if (vert.size() == 2) {  // Make continuous edge
      const int first = vert[0].first.halfedge;
      const int second = vert[1].first.halfedge;
      const glm::vec3 newTangent = glm::normalize(glm::vec3(tangent[first]) -
                                                  glm::vec3(tangent[second]));

      const glm::vec3 pos = vertPos_[halfedge_[first].startVert];
      tangent[first] =
          CircularTangent(newTangent, vertPos_[halfedge_[first].endVert] - pos);
      tangent[second] = CircularTangent(
          -newTangent, vertPos_[halfedge_[second].endVert] - pos);

      auto SmoothHalf = [&](int first, int last, float smoothness) {
        int current = NextHalfedge(halfedge_[first].pairedHalfedge);
        while (current != last) {
          tangent[current] = smoothness * tangent[current];
          current = NextHalfedge(halfedge_[current].pairedHalfedge);
        }
      };

      SmoothHalf(first, second,
                 (vert[0].second.smoothness + vert[1].first.smoothness) / 2);
      SmoothHalf(second, first,
                 (vert[1].second.smoothness + vert[0].first.smoothness) / 2);
    } else {  // Sharpen vertex uniformly
      float smoothness = 0;
      for (const Pair& pair : vert) {
        smoothness += pair.first.smoothness;
        smoothness += pair.second.smoothness;
      }
      smoothness /= 2 * vert.size();

      const int start = vert[0].first.halfedge;
      int current = start;
      do {
        tangent[current] = smoothness * tangent[current];
        current = NextHalfedge(halfedge_[current].pairedHalfedge);
      } while (current != start);
    }
  }
}

/**
 * Split each edge into n pieces as defined by calling the edgeDivisions
 * function, and sub-triangulate each triangle accordingly. This function
 * doesn't run Finish(), as that is expensive and it'll need to be run after
 * the new vertices have moved, which is a likely scenario after refinement
 * (smoothing).
 */
Vec<Barycentric> Manifold::Impl::Subdivide(
    std::function<int(glm::vec3)> edgeDivisions) {
  Vec<TmpEdge> edges = CreateTmpEdges(halfedge_);
  const int numEdge = edges.size();
  Vec<int> half2Edge(2 * numEdge);
  auto policy = autoPolicy(numEdge);
  for_each_n(policy, zip(countAt(0), edges.begin()), numEdge,
             ReindexHalfedge({half2Edge, halfedge_}));

  Vec<int> edgeAdded(numEdge);
  for_each_n(policy, zip(edgeAdded.begin(), edges.cbegin()), numEdge,
             [edgeDivisions, this](thrust::tuple<int&, TmpEdge> inOut) {
               const TmpEdge edge = thrust::get<1>(inOut);
               const glm::vec3 vec =
                   this->vertPos_[edge.first] - this->vertPos_[edge.second];
               thrust::get<0>(inOut) = edgeDivisions(vec);
             });

  Vec<int> edgeOffset(numEdge);
  const int numVert = NumVert();
  exclusive_scan(policy, edgeAdded.begin(), edgeAdded.end(), edgeOffset.begin(),
                 numVert);

  Vec<Barycentric> vertBary(edgeOffset.back() + edgeAdded.back());
  const int totalEdgeAdded = vertBary.size() - numVert;
  FillRetainedVerts(vertBary, halfedge_);
  for_each_n(policy, zip(edges.begin(), edgeAdded.begin(), edgeOffset.begin()),
             numEdge, [&vertBary](thrust::tuple<TmpEdge, int, int> in) {
               const TmpEdge edge = thrust::get<0>(in);
               const int n = thrust::get<1>(in);
               const int offset = thrust::get<2>(in);
               const float frac = 1.0f / (n + 1);
               const int v0 = edge.halfedgeIdx % 3;
               const int v1 = Next3(v0);
               const int tri = edge.halfedgeIdx / 3;
               for (int i = 0; i < n; ++i) {
                 glm::vec3 uvw(0);
                 uvw[v1] = (i + 1) * frac;
                 uvw[v0] = 1 - uvw[v1];
                 vertBary[offset + i].uvw = uvw;
                 vertBary[offset + i].tri = tri;
               }
             });

  const int numTri = NumTri();
  std::vector<Partition> subTris(numTri);
  for_each_n(policy, countAt(0), numTri,
             [&subTris, &half2Edge, &edgeAdded](int tri) {
               glm::ivec3 divisions;
               for (const int i : {0, 1, 2}) {
                 divisions[i] = edgeAdded[half2Edge[3 * tri + i]] + 1;
               }
               subTris[tri] = Partition::GetPartition(divisions);
             });

  Vec<int> triOffset(numTri);
  auto numSubTris = thrust::make_transform_iterator(
      subTris.begin(),
      [](const Partition& part) { return part.triVert.size(); });
  exclusive_scan(policy, numSubTris, numSubTris + numTri, triOffset.begin(), 0);

  Vec<int> interiorOffset(numTri);
  auto numInterior = thrust::make_transform_iterator(
      subTris.begin(),
      [](const Partition& part) { return part.NumInterior(); });
  exclusive_scan(policy, numInterior, numInterior + numTri,
                 interiorOffset.begin(), vertBary.size());

  Vec<glm::ivec3> triVerts(triOffset.back() + subTris.back().triVert.size());
  vertBary.resize(interiorOffset.back() + subTris.back().NumInterior());
  Vec<TriRef> triRef(triVerts.size());
  for_each_n(
      policy, countAt(0), numTri,
      [this, &triVerts, &triRef, &vertBary, &subTris, &edgeOffset, &half2Edge,
       &triOffset, &interiorOffset](int tri) {
        glm::ivec3 tri3;
        glm::ivec3 edgeOffsets;
        glm::bvec3 edgeFwd;
        for (const int i : {0, 1, 2}) {
          const Halfedge& halfedge = this->halfedge_[3 * tri + i];
          tri3[i] = halfedge.startVert;
          edgeOffsets[i] = edgeOffset[half2Edge[3 * tri + i]];
          edgeFwd[i] = halfedge.IsForward();
        }

        Vec<glm::ivec3> newTris = subTris[tri].Reindex(
            tri3, edgeOffsets, edgeFwd, interiorOffset[tri]);
        copy(ExecutionPolicy::Seq, newTris.begin(), newTris.end(),
             triVerts.begin() + triOffset[tri]);
        auto start = triRef.begin() + triOffset[tri];
        fill(ExecutionPolicy::Seq, start, start + newTris.size(),
             meshRelation_.triRef[tri]);

        const glm::ivec3 idx = subTris[tri].idx;
        const glm::ivec3 vIdx =
            idx[1] == Next3(idx[0]) ? idx : glm::ivec3(idx[2], idx[0], idx[1]);
        glm::ivec3 rIdx;
        for (const int i : {0, 1, 2}) {
          rIdx[vIdx[i]] = i;
        }

        const auto& subBary = subTris[tri].vertBary;
        transform(ExecutionPolicy::Seq,
                  subBary.begin() + subTris[tri].InteriorOffset(),
                  subBary.end(), vertBary.begin() + interiorOffset[tri],
                  [tri, rIdx](glm::vec3 bary) {
                    return Barycentric(
                        {tri, {bary[rIdx[0]], bary[rIdx[1]], bary[rIdx[2]]}});
                  });
      });
  meshRelation_.triRef = triRef;

  Vec<glm::vec3> newVertPos(vertBary.size());
  for_each_n(
      policy, zip(newVertPos.begin(), vertBary.begin()), vertBary.size(),
      [this](thrust::tuple<glm::vec3&, Barycentric> inOut) {
        const Barycentric bary = thrust::get<1>(inOut);
        glm::mat3 triPos;
        for (const int i : {0, 1, 2}) {
          triPos[i] =
              this->vertPos_[this->halfedge_[3 * bary.tri + i].startVert];
        }
        thrust::get<0>(inOut) = triPos * bary.uvw;
      });
  vertPos_ = newVertPos;

  faceNormal_.resize(0);

  if (meshRelation_.numProp > 0) {
    const int numPropVert = NumPropVert();
    const int addedVerts = NumVert() - numVert;
    const int propOffset = numPropVert - numVert;
    Vec<float> prop(meshRelation_.numProp *
                    (numPropVert + addedVerts + totalEdgeAdded));

    copy(policy, meshRelation_.properties.begin(),
         meshRelation_.properties.end(), prop.begin());

    for_each_n(
        policy, zip(countAt(numPropVert), vertBary.begin() + numVert),
        addedVerts, [this, &prop](thrust::tuple<int, Barycentric> in) {
          const int vert = thrust::get<0>(in);
          const Barycentric bary = thrust::get<1>(in);
          auto& rel = this->meshRelation_;

          for (int p = 0; p < rel.numProp; ++p) {
            glm::vec3 triProp;
            for (const int i : {0, 1, 2}) {
              triProp[i] =
                  rel.properties[rel.triProperties[bary.tri][i] * rel.numProp +
                                 p];
            }
            prop[vert * rel.numProp + p] = glm::dot(triProp, bary.uvw);
          }
        });

    for_each_n(
        policy, zip(edges.begin(), edgeAdded.begin(), edgeOffset.begin()),
        numEdge,
        [this, &prop, propOffset,
         addedVerts](thrust::tuple<TmpEdge, int, int> in) {
          const TmpEdge edge = thrust::get<0>(in);
          const int n = thrust::get<1>(in);
          const int offset = thrust::get<2>(in) + propOffset + addedVerts;
          auto& rel = this->meshRelation_;

          const float frac = 1.0f / (n + 1);
          const int halfedgeIdx =
              this->halfedge_[edge.halfedgeIdx].pairedHalfedge;
          const int v0 = halfedgeIdx % 3;
          const int v1 = Next3(v0);
          const int tri = halfedgeIdx / 3;
          for (int i = 0; i < n; ++i) {
            glm::vec3 uvw(0);
            uvw[v1] = (i + 1) * frac;
            uvw[v0] = 1 - uvw[v1];
            for (int p = 0; p < rel.numProp; ++p) {
              glm::vec3 triProp;
              for (const int j : {0, 1, 2}) {
                triProp[j] =
                    rel.properties[rel.triProperties[tri][j] * rel.numProp + p];
              }
              prop[(offset + i) * rel.numProp + p] = glm::dot(triProp, uvw);
            }
          }
        });

    Vec<glm::ivec3> triProp(triVerts.size());

    for_each_n(policy, countAt(0), numTri,
               [this, &triProp, &subTris, &edgeOffset, &half2Edge, &triOffset,
                &interiorOffset, propOffset, addedVerts](int tri) {
                 auto& rel = this->meshRelation_;
                 const glm::ivec3 tri3 = rel.triProperties[tri];
                 glm::ivec3 edgeOffsets;
                 glm::bvec3 edgeFwd(true);
                 for (const int i : {0, 1, 2}) {
                   const Halfedge& halfedge = this->halfedge_[3 * tri + i];
                   edgeOffsets[i] = edgeOffset[half2Edge[3 * tri + i]];
                   if (!halfedge.IsForward()) {
                     const int pairTri = halfedge.pairedHalfedge / 3;
                     const int j = halfedge.pairedHalfedge % 3;
                     if (rel.triProperties[pairTri][j] !=
                             rel.triProperties[tri][Next3(i)] ||
                         rel.triProperties[pairTri][Next3(j)] !=
                             rel.triProperties[tri][i]) {
                       edgeOffsets[i] += addedVerts;
                     } else {
                       edgeFwd[i] = false;
                     }
                   }
                 }

                 Vec<glm::ivec3> newTris = subTris[tri].Reindex(
                     tri3, edgeOffsets + propOffset, edgeFwd,
                     interiorOffset[tri] + propOffset);
                 copy(ExecutionPolicy::Seq, newTris.begin(), newTris.end(),
                      triProp.begin() + triOffset[tri]);
               });

    meshRelation_.properties = prop;
    meshRelation_.triProperties = triProp;
  }

  CreateHalfedges(triVerts);

  return vertBary;
}

void Manifold::Impl::Refine(std::function<int(glm::vec3)> edgeDivisions) {
  if (IsEmpty()) return;
  Manifold::Impl old = *this;
  Vec<Barycentric> vertBary = Subdivide(edgeDivisions);
  if (vertBary.size() == 0) return;

  if (old.halfedgeTangent_.size() == old.halfedge_.size()) {
    for_each_n(autoPolicy(NumTri()), zip(vertPos_.begin(), vertBary.begin()),
               NumVert(),
               InterpTri({old.halfedge_, old.halfedgeTangent_, old.vertPos_}));
    // Make original since the subdivided faces have been warped into
    // being non-coplanar, and hence not being related to the original faces.
    meshRelation_.originalID = ReserveIDs(1);
    InitializeOriginal();
  }

  halfedgeTangent_.resize(0);
  Finish();
}

}  // namespace manifold
