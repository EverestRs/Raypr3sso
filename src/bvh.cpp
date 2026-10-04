#include "bvh.h"
#include "bvh_gpu.h"
#include "geometry.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <cassert>

// ---- BVH File Header ----
struct BVHFileHeader {
    char magic[4] = {'B', 'V', 'H', 1};
    uint32_t numFaces;
    uint32_t numNodes;
    uint64_t faceDataOffset;
    uint64_t nodeDataOffset;
};

// ---- SAH Constants ----
static constexpr int   BINS            = 12;
static constexpr int   LEAF_THRESHOLD  = 4;
static constexpr float TRAV_COST       = 1.0f;
static constexpr float INTERSECT_COST  = 1.0f;

// ---- Per-face build info ----
struct BuildFace {
    Box    bbox;
    float3 centroid;
    int    faceIdx;  // index into the faces vector
};

// ---- Bin for SAH evaluation ----
struct Bin {
    Box   bbox;
    int   count = 0;
};

BVH::~BVH() {
    delete[] nodes;
}

// ---------------------------------------------------------------------------
// SAH Recursive Build
// ---------------------------------------------------------------------------

static int buildRecursive(BuildFace* buildFaces, int start, int end,
                          Face* faces, BVHNode*& nodes, int& nodeCount) {
    const int idx = nodeCount++;

    // Compute bounding box of all faces in this range
    Box centerBox;
    Box fullBox;
    for (int i = start; i < end; ++i) {
        fullBox = fullBox + buildFaces[i].bbox;
        centerBox = centerBox + Box(buildFaces[i].centroid, buildFaces[i].centroid);
    }

    const int numFaces = end - start;

    // Leaf: if few faces or all centroids at same point
    if (numFaces <= LEAF_THRESHOLD || centerBox.surfaceArea() < 1e-8f) {
        BVHNode& node = nodes[idx];
        node.box        = fullBox;
        node.leftChild  = -1;
        node.rightChild = -1;
        node.faceStart  = start;
        node.faceEnd    = end;
        return idx;
    }

    // ---- Find best split using SAH binning ----
    int   bestAxis   = 0;
    int   bestBin    = 0;
    float bestCost   = INFINITY;

    const float3 centerSize = centerBox.v1 - centerBox.v0;

    for (int axis = 0; axis < 3; ++axis) {
        if (centerSize[axis] < 1e-8f) continue;

        const float invBinSize = BINS / centerSize[axis];
        const float axisMin    = centerBox.v0[axis];

        // Initialize bins
        Bin bin[BINS];
        for (int i = start; i < end; ++i) {
            const int b = std::min(BINS - 1,
                int((buildFaces[i].centroid[axis] - axisMin) * invBinSize));
            bin[b].bbox  = bin[b].bbox + buildFaces[i].bbox;
            bin[b].count++;
        }

        // Prefix scan (left to right)
        Box leftBox[BINS - 1];
        int leftCount[BINS - 1];
        Box curBox;
        int curCount = 0;
        for (int b = 0; b < BINS - 1; ++b) {
            curBox   = curBox + bin[b].bbox;
            curCount += bin[b].count;
            leftBox[b]   = curBox;
            leftCount[b] = curCount;
        }

        // Suffix scan (right to left)
        Box rightBox;
        int rightCount = 0;
        for (int b = BINS - 1; b >= 1; --b) {
            rightBox   = rightBox + bin[b].bbox;
            rightCount += bin[b].count;
            const float saLeft  = leftBox[b - 1].surfaceArea();
            const float saRight = rightBox.surfaceArea();
            const float saParent = fullBox.surfaceArea();
            if (saParent <= 0.0f) continue;

            const float cost = TRAV_COST +
                (saLeft  * leftCount[b - 1] +
                 saRight * rightCount) * INTERSECT_COST / saParent;

            if (cost < bestCost) {
                bestCost = cost;
                bestAxis = axis;
                bestBin  = b;
            }
        }
    }

    // Fallback: if no good split found, make leaf
    const float leafCost = numFaces * INTERSECT_COST;
    if (bestCost >= leafCost || bestCost == INFINITY) {
        BVHNode& node = nodes[idx];
        node.box        = fullBox;
        node.leftChild  = -1;
        node.rightChild = -1;
        node.faceStart  = start;
        node.faceEnd    = end;
        return idx;
    }

    // ---- Partition faces along best axis at best bin ----
    const float axisMin   = centerBox.v0[bestAxis];
    const float invBinSize = BINS / centerSize[bestAxis];

    auto mid = std::partition(buildFaces + start, buildFaces + end,
        [&](const BuildFace& bf) {
            const int b = std::min(BINS - 1,
                int((bf.centroid[bestAxis] - axisMin) * invBinSize));
            return b <= bestBin;
        });
    const int splitIdx = int(mid - buildFaces);

    // Handle degenerate split: fallback to median split
    if (splitIdx == start || splitIdx == end) {
        const int midIdx = start + numFaces / 2;
        std::nth_element(buildFaces + start, buildFaces + midIdx, buildFaces + end,
            [bestAxis](const BuildFace& a, const BuildFace& b) {
                return a.centroid[bestAxis] < b.centroid[bestAxis];
            });
        BVHNode& node = nodes[idx];
        node.box        = fullBox;
        node.leftChild  = buildRecursive(buildFaces, start, midIdx,   faces, nodes, nodeCount);
        node.rightChild = buildRecursive(buildFaces, midIdx,  end,    faces, nodes, nodeCount);
        return idx;
    }

    BVHNode& node = nodes[idx];
    node.box        = fullBox;
    node.leftChild  = buildRecursive(buildFaces, start,    splitIdx, faces, nodes, nodeCount);
    node.rightChild = buildRecursive(buildFaces, splitIdx, end,      faces, nodes, nodeCount);
    return idx;
}

void BVH::build(std::vector<Face>& faces) {
    this->faces = faces.data();

    const int totalFaces = static_cast<int>(faces.size());
    if (totalFaces == 0) {
        numNodes = 0;
        nodes = nullptr;
        return;
    }

    // Build per-face metadata
    std::vector<BuildFace> buildFaces(totalFaces);
    for (int i = 0; i < totalFaces; ++i) {
        const Face& f = faces[i];
        Box bbox(f.v[0], f.v[0]);
        bbox = bbox + Box(f.v[1], f.v[1]);
        bbox = bbox + Box(f.v[2], f.v[2]);
        buildFaces[i].bbox     = bbox;
        buildFaces[i].centroid = bbox.centroid();
        buildFaces[i].faceIdx  = i;
    }

    // Allocate worst-case nodes (2 * totalFaces - 1)
    const int maxNodes = 2 * totalFaces - 1;
    nodes = new BVHNode[maxNodes];
    numNodes = 0;

    // Build recursively
    buildRecursive(buildFaces.data(), 0, totalFaces, faces.data(), nodes, numNodes);

    // Reorder faces to match leaf order for better cache performance
    std::vector<Face> reordered(totalFaces);
    for (int i = 0; i < totalFaces; ++i) {
        reordered[i] = faces[buildFaces[i].faceIdx];
    }
    for (int i = 0; i < totalFaces; ++i) {
        faces[i] = reordered[i];
    }

    // Remap leaf faceStart/faceEnd (now correspond to reordered array positions)
    // The buildRecursive already stores start/end indices into the BuildFace array,
    // which after partition corresponds to the reordered faces.
    // No remapping needed since we buildFaces was used as the partition target
    // and faces was reordered to match.

    assert(numNodes <= maxNodes);
}

// ---------------------------------------------------------------------------
// BVH Traversal
// ---------------------------------------------------------------------------

static bool intersectLeaf(const Ray& ray, const Face* faces,
                           int faceStart, int faceEnd, HitRecord& hit) {
    bool anyHit = false;
    for (int i = faceStart; i < faceEnd; ++i) {
        const Face& f = faces[i];
        float u, v;
        const float t = rayTriangleIntersect(ray, f.v[0], f.v[1], f.v[2], u, v);
        if (t < hit.t_max && t >= hit.t_min) {
            hit.t_max = t;
            hit.t     = t;
            hit.u     = u;
            hit.v     = v;
            hit.face  = &f;
            anyHit = true;
        }
    }
    return anyHit;
}

void BVH::rayHit(const Ray& ray, HitRecord& hit) const {
    if (!nodes || numNodes == 0) return;

    // Iterative traversal using explicit stack
    constexpr int STACK_SIZE = 64;
    int stack[STACK_SIZE];
    int sp = 0;

    stack[sp++] = 0;  // push root

    while (sp > 0) {
        const int idx = stack[--sp];
        const BVHNode& node = nodes[idx];

        // Test ray against node bounding box
        float tL = hit.t_min, tR = hit.t_max;
        rayBoxIntersect(ray, node.box, tL, tR);
        if (tL > tR || tR < hit.t_min) continue;

        if (node.leftChild == -1) {
            // Leaf: intersect all faces
            intersectLeaf(ray, faces, node.faceStart, node.faceEnd, hit);
        } else {
            // Interior: push children (near child first based on ray direction)
            const BVHNode& left  = nodes[node.leftChild];
            const BVHNode& right = nodes[node.rightChild];

            // Determine traversal order based on ray sign
            const float3 raySign = ray.direction;
            const int nearChild = (raySign.x + raySign.y + raySign.z >= 0)
                ? node.leftChild : node.rightChild;
            const int farChild  = (nearChild == node.leftChild)
                ? node.rightChild : node.leftChild;

            // Push far first so near is processed first
            stack[sp++] = farChild;
            stack[sp++] = nearChild;

            if (sp > STACK_SIZE - 2) break;  // safety
        }
    }
}

bool BVH::rayOccluded(const Ray& ray, float maxDist) const {
    if (!nodes || numNodes == 0) return false;

    HitRecord hit;
    hit.t_max = maxDist;

    constexpr int STACK_SIZE = 64;
    int stack[STACK_SIZE];
    int sp = 0;
    stack[sp++] = 0;

    while (sp > 0) {
        const int idx = stack[--sp];
        const BVHNode& node = nodes[idx];

        float tL = hit.t_min, tR = hit.t_max;
        rayBoxIntersect(ray, node.box, tL, tR);
        if (tL > tR || tR < hit.t_min) continue;

        if (node.leftChild == -1) {
            // Leaf: check for any intersection
            for (int i = node.faceStart; i < node.faceEnd; ++i) {
                const Face& f = faces[i];
                float u, v;
                const float t = rayTriangleIntersect(ray, f.v[0], f.v[1], f.v[2], u, v);
                if (t < maxDist && t >= hit.t_min) {
                    return true;  // early exit
                }
            }
        } else {
            const float3 raySign = ray.direction;
            const int nearChild = (raySign.x + raySign.y + raySign.z >= 0)
                ? node.leftChild : node.rightChild;
            const int farChild  = (nearChild == node.leftChild)
                ? node.rightChild : node.leftChild;

            stack[sp++] = farChild;
            stack[sp++] = nearChild;
            if (sp > STACK_SIZE - 2) break;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

bool BVH::save(const std::string& path) const {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;

    BVHFileHeader header;
    header.numFaces       = numNodes > 0 ? nodes[0].faceEnd : 0;
    header.numNodes       = numNodes;
    header.faceDataOffset = sizeof(BVHFileHeader);
    header.nodeDataOffset = header.faceDataOffset + header.numFaces * sizeof(Face);

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(faces), header.numFaces * sizeof(Face));
    file.write(reinterpret_cast<const char*>(nodes), numNodes * sizeof(BVHNode));

    return file.good();
}

bool BVH::load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    BVHFileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (header.magic[0] != 'B' || header.magic[1] != 'V' ||
        header.magic[2] != 'H' || header.magic[3] != 1) {
        return false;
    }

    // Read faces
    // Note: faces are owned by Model, so this just establishes a reference.
    // For loading from cache, we'd need to re-upload to the owning Model.
    // For now, skip reading faces (they should be loaded separately).

    delete[] nodes;
    nodes = new BVHNode[header.numNodes];

    file.seekg(header.nodeDataOffset);
    file.read(reinterpret_cast<char*>(nodes), header.numNodes * sizeof(BVHNode));
    numNodes = header.numNodes;
    faces    = nullptr;  // caller must set faces

    return file.good();
}

// ---------------------------------------------------------------------------
// Flatten to Linear BVH for GPU
// ---------------------------------------------------------------------------
LinearBVHNode* BVH::flattenForGPU() const {
    if (!nodes || numNodes == 0) return nullptr;

    auto* linear = new LinearBVHNode[numNodes];

    for (int i = 0; i < numNodes; ++i) {
        const BVHNode& src = nodes[i];
        LinearBVHNode& dst = linear[i];

        dst.bboxMin[0] = src.box.v0;
        dst.bboxMax[0] = src.box.v1;
        dst.bboxMin[1] = src.box.v0;
        dst.bboxMax[1] = src.box.v1;

        if (src.leftChild == -1) {
            // Leaf: use faceStart/faceCount (union with leftChild/rightChild)
            dst.isLeaf = 1;
            dst.faceStart = src.faceStart;
            dst.faceCount = src.faceEnd - src.faceStart;
        } else {
            // Interior: use leftChild/rightChild (union with faceStart/faceCount)
            dst.isLeaf = 0;
            dst.leftChild  = src.leftChild;
            dst.rightChild = src.rightChild;
        }
        dst.pad = 0;
    }

    return linear;
}
