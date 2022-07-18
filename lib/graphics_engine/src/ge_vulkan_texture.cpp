#include "ge_vulkan_texture.hpp"

#include "ge_vulkan_command_loader.hpp"
#include "ge_vulkan_features.hpp"
#include "ge_vulkan_driver.hpp"
#include "ge_gl_utils.hpp"
#include "ge_main.hpp"
#include "ge_texture.hpp"

extern "C"
{
    #include <mipmap/img.h>
    #include <mipmap/imgresize.h>
}

#include <IAttributes.h>
#include <limits>

namespace GE
{
GEVulkanTexture::GEVulkanTexture(const std::string& path,
                         std::function<void(video::IImage*)> image_mani)
               : video::ITexture(path.c_str()), m_image_mani(image_mani),
                 m_locked_data(NULL),
                 m_vulkan_device(getVKDriver()->getDevice()),
                 m_image(VK_NULL_HANDLE), m_vma_allocation(VK_NULL_HANDLE),
                 m_texture_size(0), m_disable_reload(false),
                 m_has_mipmaps(true),
                 m_internal_format(VK_FORMAT_R8G8B8A8_UNORM),
                 m_vk(getVKDriver())
{
    m_max_size = getDriver()->getDriverAttributes()
        .getAttributeAsDimension2d("MAX_TEXTURE_SIZE");
    m_full_path = getDriver()->getFileSystem()->getAbsolutePath(NamedPath);
    if (!getDriver()->getFileSystem()->existFileOnly(m_full_path))
    {
        LoadingFailed = true;
        return;
    }

    m_size_lock.lock();
    m_image_view_lock.lock();
    GEVulkanCommandLoader::addMultiThreadingCommand(
        std::bind(&GEVulkanTexture::reloadInternal, this));
}   // GEVulkanTexture

// ----------------------------------------------------------------------------
GEVulkanTexture::GEVulkanTexture(video::IImage* img, const std::string& name)
               : video::ITexture(name.c_str()), m_image_mani(nullptr),
                 m_locked_data(NULL),
                 m_vulkan_device(getVKDriver()->getDevice()),
                 m_image(VK_NULL_HANDLE), m_vma_allocation(VK_NULL_HANDLE),
                 m_texture_size(0), m_disable_reload(true),
                 m_has_mipmaps(true),
                 m_internal_format(VK_FORMAT_R8G8B8A8_UNORM),
                 m_vk(getVKDriver())
{
    if (!img)
    {
        LoadingFailed = true;
        return;
    }
    m_size = m_orig_size = img->getDimension();
    if (m_size.Width < 4 || m_size.Height < 4)
        m_has_mipmaps = false;
    uint8_t* data = (uint8_t*)img->lock();
    bgraConversion(data);
    upload(data, m_has_mipmaps/*generate_hq_mipmap*/);
    img->unlock();
    img->drop();
}   // GEVulkanTexture

// ----------------------------------------------------------------------------
GEVulkanTexture::GEVulkanTexture(const std::string& name, unsigned int size,
                                 bool single_channel)
           : video::ITexture(name.c_str()), m_image_mani(nullptr),
             m_locked_data(NULL), m_vulkan_device(getVKDriver()->getDevice()),
             m_image(VK_NULL_HANDLE), m_vma_allocation(VK_NULL_HANDLE),
             m_texture_size(0), m_disable_reload(true), m_has_mipmaps(true),
             m_internal_format(single_channel ?
             VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM),
             m_vk(getVKDriver())
{
    if (isSingleChannel() && !GEVulkanFeatures::supportsR8Blit())
        m_has_mipmaps = false;
    else if (!isSingleChannel() && !GEVulkanFeatures::supportsRGBA8Blit())
        m_has_mipmaps = false;

    m_orig_size.Width = size;
    m_orig_size.Height = size;
    m_size = m_orig_size;

    std::vector<uint8_t> data;
    data.resize(size * size * (isSingleChannel() ? 1 : 4), 0);
    upload(data.data());
}   // GEVulkanTexture

// ----------------------------------------------------------------------------
GEVulkanTexture::~GEVulkanTexture()
{
    m_image_view_lock.lock();
    m_image_view_lock.unlock();

    if (m_image_view || m_image != VK_NULL_HANDLE ||
        m_vma_allocation != VK_NULL_HANDLE)
        m_vk->waitIdle();

    clearVulkanData();
}   // ~GEVulkanTexture

// ----------------------------------------------------------------------------
bool GEVulkanTexture::createTextureImage(uint8_t* texture_data,
                                         bool generate_hq_mipmap)
{
    video::IImage* mipmap = NULL;
    std::vector<std::pair<core::dimension2du, unsigned> > mipmap_sizes =
        getMipmapSizes();
    VkDeviceSize mipmap_data_size = 0;

    unsigned channels = (isSingleChannel() ? 1 : 4);
    VkDeviceSize image_size = m_size.Width * m_size.Height * channels;
    if (generate_hq_mipmap)
    {
        mipmap = getDriver()->createImage(video::ECF_A8R8G8B8,
            mipmap_sizes[0].first);
        if (mipmap == NULL)
            throw std::runtime_error("Creating mipmap memory failed");
        generateHQMipmap(texture_data, mipmap_sizes, (uint8_t*)mipmap->lock());
        mipmap_data_size = mipmap_sizes.back().second +
            mipmap_sizes.back().first.getArea() * channels - image_size;
    }

    VkDeviceSize image_total_size = image_size + mipmap_data_size;
    VkBuffer staging_buffer;
    VmaAllocation staging_buffer_allocation;
    VmaAllocationCreateInfo staging_buffer_create_info = {};
    staging_buffer_create_info.usage = VMA_MEMORY_USAGE_AUTO;
    staging_buffer_create_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    staging_buffer_create_info.preferredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    bool success = m_vk->createBuffer(image_total_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging_buffer_create_info,
        staging_buffer, staging_buffer_allocation);

    if (!success)
        return false;

    VkResult ret = VK_SUCCESS;
    uint8_t* data;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    if ((ret = vmaMapMemory(m_vk->getVmaAllocator(), staging_buffer_allocation,
        (void**)&data)) != VK_SUCCESS)
        goto destroy;
    memcpy(data, texture_data, image_size);
    if (mipmap)
    {
        memcpy(data + image_size, mipmap->lock(), mipmap_data_size);
        mipmap->drop();
    }
    vmaUnmapMemory(m_vk->getVmaAllocator(), staging_buffer_allocation);
    vmaFlushAllocation(m_vk->getVmaAllocator(),
        staging_buffer_allocation, 0, image_total_size);

    success = createImage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!success)
    {
        ret = VK_NOT_READY;
        goto destroy;
    }

    command_buffer = GEVulkanCommandLoader::beginSingleTimeCommands();

    transitionImageLayout(command_buffer, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    if (generate_hq_mipmap)
    {
        unsigned level = 0;
        for (auto& mip : mipmap_sizes)
        {
            copyBufferToImage(command_buffer, staging_buffer, mip.first.Width,
                mip.first.Height, 0, 0, mip.second, level++);
        }
    }
    else
    {
        copyBufferToImage(command_buffer, staging_buffer, m_size.Width,
            m_size.Height, 0, 0, 0, 0);
    }

    transitionImageLayout(command_buffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    GEVulkanCommandLoader::endSingleTimeCommands(command_buffer);

destroy:
    vmaDestroyBuffer(m_vk->getVmaAllocator(), staging_buffer,
        staging_buffer_allocation);
    return ret == VK_SUCCESS;
}   // createTextureImage

// ----------------------------------------------------------------------------
bool GEVulkanTexture::createImage(VkImageUsageFlags usage)
{
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = m_size.Width;
    image_info.extent.height = m_size.Height;
    image_info.extent.depth = 1;
    image_info.mipLevels = getMipmapLevels();
    image_info.arrayLayers = 1;
    image_info.format = m_internal_format;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = usage;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    VkResult result = vmaCreateImage(m_vk->getVmaAllocator(), &image_info,
        &alloc_info, &m_image, &m_vma_allocation, NULL);

    if (result != VK_SUCCESS)
        return false;

    return true;
}   // createImage

// ----------------------------------------------------------------------------
void GEVulkanTexture::transitionImageLayout(VkCommandBuffer command_buffer,
                                            VkImageLayout old_layout,
                                            VkImageLayout new_layout)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_image;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = getMipmapLevels();
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    if (new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    else
    {
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }

    VkPipelineStageFlags source_stage;
    VkPipelineStageFlags destination_stage;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
        new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        source_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
        new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        source_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destination_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    }
    else
    {
        return;
    }

    vkCmdPipelineBarrier(command_buffer, source_stage, destination_stage, 0, 0,
        NULL, 0, NULL, 1, &barrier);
}   // transitionImageLayout

// ----------------------------------------------------------------------------
void GEVulkanTexture::copyBufferToImage(VkCommandBuffer command_buffer,
                                        VkBuffer buffer, u32 w, u32 h, s32 x,
                                        s32 y, u32 offset, u32 mipmap_level)
{
    VkBufferImageCopy region = {};
    region.bufferOffset = offset;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = mipmap_level;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {x, y, 0};
    region.imageExtent = {w, h, 1};

    vkCmdCopyBufferToImage(command_buffer, buffer, m_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}   // copyBufferToImage

// ----------------------------------------------------------------------------
bool GEVulkanTexture::createImageView(VkImageAspectFlags aspect_flags)
{
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = m_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = m_internal_format;
    view_info.subresourceRange.aspectMask = aspect_flags;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = getMipmapLevels();
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    if (isSingleChannel())
    {
        view_info.components.r = VK_COMPONENT_SWIZZLE_ONE;
        view_info.components.g = VK_COMPONENT_SWIZZLE_ONE;
        view_info.components.b = VK_COMPONENT_SWIZZLE_ONE;
        view_info.components.a = VK_COMPONENT_SWIZZLE_R;
    }

    std::shared_ptr<VkImageView> image_view = std::make_shared<VkImageView>();
    VkResult result = vkCreateImageView(m_vulkan_device, &view_info, NULL,
        image_view.get());
    if (result == VK_SUCCESS)
    {
        m_image_view = image_view;
        return true;
    }
    return false;
}   // createImageView

// ----------------------------------------------------------------------------
void GEVulkanTexture::clearVulkanData()
{
    if (m_image_view)
    {
        vkDestroyImageView(m_vulkan_device, *m_image_view.get(), NULL);
        *(m_image_view.get()) = VK_NULL_HANDLE;
        m_image_view.reset();
    }
    if (m_image != VK_NULL_HANDLE)
        vmaDestroyImage(m_vk->getVmaAllocator(), m_image, m_vma_allocation);
}   // clearVulkanData

// ----------------------------------------------------------------------------
void GEVulkanTexture::reloadInternal()
{
    if (m_disable_reload)
        return;

    clearVulkanData();

    io::IReadFile* file = io::createReadFile(m_full_path);
    if (file == NULL)
    {
        // We checked for file existence so we should always get a file
        throw std::runtime_error("File missing in getResizedImage");
    }
    video::IImage* texture_image = getResizedImage(file, m_max_size,
        &m_orig_size);
    if (texture_image == NULL)
        throw std::runtime_error("Missing texture_image in getResizedImage");
    file->drop();

    m_size = texture_image->getDimension();
    if (m_size.Width < 4 || m_size.Height < 4)
        m_has_mipmaps = false;
    m_size_lock.unlock();

    if (m_image_mani)
        m_image_mani(texture_image);

    uint8_t* data = (uint8_t*)texture_image->lock();
    bgraConversion(data);
    upload(data, m_has_mipmaps/*generate_hq_mipmap*/);
    m_image_view_lock.unlock();

    texture_image->unlock();
    texture_image->drop();
}   // reloadInternal

// ----------------------------------------------------------------------------
void GEVulkanTexture::upload(uint8_t* data, bool generate_hq_mipmap)
{
    if (!createTextureImage(data, generate_hq_mipmap))
        return;
    if (!createImageView(VK_IMAGE_ASPECT_COLOR_BIT))
        return;
    m_texture_size = m_size.Width * m_size.Height * (isSingleChannel() ? 1 : 4);
}   // upload

// ----------------------------------------------------------------------------
void* GEVulkanTexture::lock(video::E_TEXTURE_LOCK_MODE mode, u32 mipmap_level)
{
    uint8_t* texture_data = getTextureData();
    if (isSingleChannel())
    {
        m_locked_data = new uint8_t[m_size.Width * m_size.Height * 4]();
        for (unsigned int i = 0; i < m_size.Width * m_size.Height; i++)
        {
            m_locked_data[i * 4 + 2] = texture_data[i];
            m_locked_data[i * 4 + 3] = 255;
        }
        delete [] texture_data;
        return m_locked_data;
    }
    else
    {
        m_locked_data = texture_data;
        bgraConversion(m_locked_data);
        return m_locked_data;
    }
}   // lock

// ----------------------------------------------------------------------------
uint8_t* GEVulkanTexture::getTextureData()
{
    m_image_view_lock.lock();
    m_image_view_lock.unlock();

    VkBuffer buffer;
    VmaAllocation buffer_allocation;
    VkDeviceSize image_size =
        m_size.Width * m_size.Height * (isSingleChannel() ? 1 : 4);
    VmaAllocationCreateInfo buffer_create_info = {};
    buffer_create_info.usage = VMA_MEMORY_USAGE_AUTO;
    buffer_create_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    buffer_create_info.preferredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    if (!m_vk->createBuffer(image_size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, buffer_create_info, buffer,
        buffer_allocation))
        return NULL;

    VkCommandBuffer command_buffer = GEVulkanCommandLoader::beginSingleTimeCommands();

    transitionImageLayout(command_buffer,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {m_size.Width, m_size.Height, 1};

    vkCmdCopyImageToBuffer(command_buffer, m_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

    transitionImageLayout(command_buffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    GEVulkanCommandLoader::endSingleTimeCommands(command_buffer);

    uint8_t* texture_data = new uint8_t[image_size];
    void* mapped_data;
    if (vmaMapMemory(m_vk->getVmaAllocator(), buffer_allocation,
        &mapped_data) != VK_SUCCESS)
    {
        delete [] texture_data;
        texture_data = NULL;
        goto cleanup;
    }

    vmaInvalidateAllocation(m_vk->getVmaAllocator(), buffer_allocation,
        0, image_size);
    memcpy(texture_data, mapped_data, image_size);
    vmaUnmapMemory(m_vk->getVmaAllocator(), buffer_allocation);

cleanup:
    vmaDestroyBuffer(m_vk->getVmaAllocator(), buffer, buffer_allocation);
    return texture_data;
}   // getTextureData

//-----------------------------------------------------------------------------
void GEVulkanTexture::updateTexture(void* data, video::ECOLOR_FORMAT format,
                                    u32 w, u32 h, u32 x, u32 y)
{
    m_image_view_lock.lock();
    m_image_view_lock.unlock();

    VkBuffer staging_buffer;
    VmaAllocation staging_buffer_allocation;
    VmaAllocationCreateInfo staging_buffer_create_info = {};
    staging_buffer_create_info.usage = VMA_MEMORY_USAGE_AUTO;
    staging_buffer_create_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    staging_buffer_create_info.preferredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (isSingleChannel())
    {
        if (format == video::ECF_R8)
        {
            unsigned image_size = w * h;
            if (!m_vk->createBuffer(image_size,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging_buffer_create_info,
                staging_buffer, staging_buffer_allocation))
                return;

            void* mapped_data;
            vmaMapMemory(m_vk->getVmaAllocator(), staging_buffer_allocation,
                &mapped_data);
            memcpy(mapped_data, data, (size_t)(image_size));
            vmaUnmapMemory(m_vk->getVmaAllocator(), staging_buffer_allocation);
            vmaFlushAllocation(m_vk->getVmaAllocator(),
                staging_buffer_allocation, 0, image_size);
        }
    }
    else
    {
        if (format == video::ECF_R8)
        {
            unsigned image_size = w * h * 4;
            if (!m_vk->createBuffer(w * h * 4,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging_buffer_create_info,
                staging_buffer, staging_buffer_allocation))
                return;

            const unsigned int size = w * h;
            std::vector<uint8_t> image_data(size * 4, 255);
            uint8_t* orig_data = (uint8_t*)data;
            for (unsigned int i = 0; i < size; i++)
                image_data[4 * i + 3] = orig_data[i];
            void* mapped_data;
            vmaMapMemory(m_vk->getVmaAllocator(), staging_buffer_allocation,
                &mapped_data);
            memcpy(mapped_data, image_data.data(), (size_t)(image_size));
            vmaUnmapMemory(m_vk->getVmaAllocator(), staging_buffer_allocation);
            vmaFlushAllocation(m_vk->getVmaAllocator(),
                staging_buffer_allocation, 0, image_size);
        }
        else if (format == video::ECF_A8R8G8B8)
        {
            unsigned image_size = w * h * 4;
            if (!m_vk->createBuffer(w * h * 4,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging_buffer_create_info,
                staging_buffer, staging_buffer_allocation))
                return;

            uint8_t* u8_data = (uint8_t*)data;
            for (unsigned int i = 0; i < w * h; i++)
            {
                uint8_t tmp_val = u8_data[i * 4];
                u8_data[i * 4] = u8_data[i * 4 + 2];
                u8_data[i * 4 + 2] = tmp_val;
            }
            void* mapped_data;
            vmaMapMemory(m_vk->getVmaAllocator(), staging_buffer_allocation,
                &mapped_data);
            memcpy(mapped_data, u8_data, (size_t)(image_size));
            vmaUnmapMemory(m_vk->getVmaAllocator(), staging_buffer_allocation);
            vmaFlushAllocation(m_vk->getVmaAllocator(),
                staging_buffer_allocation, 0, image_size);
        }
    }

    VkCommandBuffer command_buffer = GEVulkanCommandLoader::beginSingleTimeCommands();
    transitionImageLayout(command_buffer,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyBufferToImage(command_buffer, staging_buffer, w, h, x, y, 0, 0);

    bool blit_mipmap = true;
    if (isSingleChannel() && !GEVulkanFeatures::supportsR8Blit())
        blit_mipmap = false;
    else if (!isSingleChannel() && !GEVulkanFeatures::supportsRGBA8Blit())
        blit_mipmap = false;
    if (blit_mipmap)
    {
        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.image = m_image;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.subresourceRange.levelCount = 1;

        int mip_width = m_size.Width;
        int mip_height = m_size.Height;
        unsigned mip_levels = getMipmapLevels();

        for (unsigned i = 1; i < mip_levels; i++)
        {
            barrier.subresourceRange.baseMipLevel = i - 1;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

            vkCmdPipelineBarrier(command_buffer,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, NULL, 0, NULL, 1, &barrier);

            VkImageBlit blit{};
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {mip_width, mip_height, 1};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = i - 1;
            blit.srcSubresource.baseArrayLayer = 0;
            blit.srcSubresource.layerCount = 1;
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] =
            {
                mip_width > 1 ? mip_width / 2 : 1,
                mip_height > 1 ? mip_height / 2 : 1,
                1
            };
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = i;
            blit.dstSubresource.baseArrayLayer = 0;
            blit.dstSubresource.layerCount = 1;

            vkCmdBlitImage(command_buffer,
                m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                VK_FILTER_LINEAR);

            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

            vkCmdPipelineBarrier(command_buffer,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                &barrier);

            if (mip_width > 1) mip_width /= 2;
            if (mip_height > 1) mip_height /= 2;
        }
    }

    transitionImageLayout(command_buffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    GEVulkanCommandLoader::endSingleTimeCommands(command_buffer);

    vmaDestroyBuffer(m_vk->getVmaAllocator(), staging_buffer,
        staging_buffer_allocation);
}   // updateTexture

//-----------------------------------------------------------------------------
void GEVulkanTexture::bgraConversion(uint8_t* img_data)
{
    for (unsigned int i = 0; i < m_size.Width * m_size.Height; i++)
    {
        uint8_t tmp_val = img_data[i * 4];
        img_data[i * 4] = img_data[i * 4 + 2];
        img_data[i * 4 + 2] = tmp_val;
    }
}   // bgraConversion

//-----------------------------------------------------------------------------
void GEVulkanTexture::reload()
{
    m_image_view_lock.lock();
    m_image_view_lock.unlock();

    if (m_image_view || m_image != VK_NULL_HANDLE ||
        m_vma_allocation != VK_NULL_HANDLE)
        m_vk->waitIdle();

    if (!m_disable_reload)
    {
        m_size_lock.lock();
        m_image_view_lock.lock();
        GEVulkanCommandLoader::addMultiThreadingCommand(
            std::bind(&GEVulkanTexture::reloadInternal, this));
    }
}   // reload

// ----------------------------------------------------------------------------
void GEVulkanTexture::generateHQMipmap(void* in,
                                       std::vector<std::pair
                                       <core::dimension2du, unsigned> >& mms,
                                       uint8_t* out)
{
    imMipmapCascade cascade;
    imReduceOptions options;
    imReduceSetOptions(&options,
        std::string(NamedPath.getPtr()).find("_Normal.") != std::string::npos ?
        IM_REDUCE_FILTER_NORMALMAP: IM_REDUCE_FILTER_LINEAR/*filter*/,
        2/*hopcount*/, 2.0f/*alpha*/, 1.0f/*amplifynormal*/,
        0.0f/*normalsustainfactor*/);

    unsigned channels = (isSingleChannel() ? 1 : 4);
#ifdef DEBUG
    int ret = imBuildMipmapCascade(&cascade, in, mms[0].first.Width,
        mms[0].first.Height, 1/*layercount*/, channels,
        mms[0].first.Width * channels, &options, 0);
    if (ret != 1)
        throw std::runtime_error("imBuildMipmapCascade failed");
#else
    imBuildMipmapCascade(&cascade, in, mms[0].first.Width,
        mms[0].first.Height, 1/*layercount*/, channels,
        mms[0].first.Width * channels, &options, 0);
#endif
    for (unsigned int i = 1; i < mms.size(); i++)
    {
        const unsigned copy_size = mms[i].first.getArea() * channels;
        memcpy(out, cascade.mipmap[i], copy_size);
        out += copy_size;
        mms[i].second = mms[i - 1].first.getArea() * channels +
            mms[i - 1].second;
    }
    imFreeMipmapCascade(&cascade);
}   // generateHQMipmap

}
