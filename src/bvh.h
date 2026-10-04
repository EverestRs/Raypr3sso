#pragma once

#include "../include/types.h"
#include "component.h"
#include "bvh_gpu.h"
#ifndef __CUDACC__
#include <vector>
#include <string>
#endif

struct BVHNode {
    Box box;
    int leftChild;    // -1 for leaf
    int rightChild;
    int faceStart;    // Leaf: face range start
    int faceEnd;      // Leaf: face range end
};

class BVH {
    BVHNode* nodes = nullptr;
#ifndef __CUDACC__
    Face*    faces = nullptr;  // Reference to Model::faces (CPU only)
#endif
    int      numNodes = 0;

public:
    BVH() = default;
    ~BVH();

    // Non-copyable but movable
    BVH(const BVH&) = delete;
    BVH& operator=(const BVH&) = delete;

    BVH(BVH&& other) noexcept
        : nodes(other.nodes), numNodes(other.numNodes)
#ifndef __CUDACC__
        , faces(other.faces)
#endif
    {
        other.nodes = nullptr;
#ifndef __CUDACC__
        other.faces = nullptr;
#endif
        other.numNodes = 0;
    }
    BVH& operator=(BVH&& other) noexcept {
        if (this != &other) {
            delete[] nodes;
            nodes = other.nodes;
            numNodes = other.numNodes;
#ifndef __CUDACC__
            faces = other.faces;
            other.faces = nullptr;
#endif
            other.nodes = nullptr;
            other.numNodes = 0;
        }
        return *this;
    }

#ifndef __CUDACC__
    // Build using SAH heuristic
    void build(std::vector<Face>& faces);
#endif

    // CPU traversal (recursive)
    void rayHit(const Ray& ray, HitRecord& hit) const;
    bool rayOccluded(const Ray& ray, float maxDist) const;

    // Scene bounding box (root node)
    Box bounds() const {
        if (!nodes || numNodes == 0) return Box();
        return nodes[0].box;
    }

    // Export Linear BVH (GPU)
    LinearBVHNode* flattenForGPU() const;
    int numNodesGPU() const { return numNodes; }

#ifndef __CUDACC__
    // Serialization
    bool save(const std::string& path) const;
    bool load(const std::string& path);
#endif

    int numFaces() const { return numNodes > 0 ? nodes[0].faceEnd : 0; }
    int numNodesTotal() const { return numNodes; }
};
