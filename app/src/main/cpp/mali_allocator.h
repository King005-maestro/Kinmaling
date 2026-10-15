#pragma once
#include <vulkan/vulkan.h>
#include <mutex>
#include <vector>

struct MaliAlloc {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, size = 0;
    void* mapped = nullptr;
    uint32_t block = 0;
};

class MaliAllocator {
public:
    bool init(VkPhysicalDevice pd, VkDevice dev);
    void destroy();
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, MaliAlloc& a);
    void destroyBuffer(VkBuffer buf, const MaliAlloc& a);
    bool unifiedMemory() const { return unified_; }
private:
    struct Range { VkDeviceSize off, size; };
    struct Block {
        VkDeviceMemory mem; void* map; VkDeviceSize size; std::vector<Range> freeList;
    };
    bool newBlock(VkDeviceSize min);
    uint32_t pickType(uint32_t bits) const;
    VkPhysicalDevice pd_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mp_{};
    uint32_t typeIdx_ = 0;
    bool unified_ = false;
    std::vector<Block> blocks_;
    std::mutex mu_;
    static constexpr VkDeviceSize kBlock = 64ull << 20;
};
