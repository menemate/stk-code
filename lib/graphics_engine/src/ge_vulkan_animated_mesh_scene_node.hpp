#ifndef HEADER_GE_VULKAN_ANIMATED_MESH_SCENE_NODE_HPP
#define HEADER_GE_VULKAN_ANIMATED_MESH_SCENE_NODE_HPP

#include "../source/Irrlicht/CAnimatedMeshSceneNode.h"

namespace GE
{
class GESPM;

class GEVulkanAnimatedMeshSceneNode : public irr::scene::CAnimatedMeshSceneNode
{
public:
    // ------------------------------------------------------------------------
    GEVulkanAnimatedMeshSceneNode(irr::scene::IAnimatedMesh* mesh,
        irr::scene::ISceneNode* parent, irr::scene::ISceneManager* mgr, irr::s32 id,
        const irr::core::vector3df& position = irr::core::vector3df(0, 0, 0),
        const irr::core::vector3df& rotation = irr::core::vector3df(0, 0, 0),
        const irr::core::vector3df& scale = irr::core::vector3df(1.0f, 1.0f, 1.0f));
    // ------------------------------------------------------------------------
    GESPM* getSPM() const;
};   // GEVulkanAnimatedMeshSceneNode

}

#endif
