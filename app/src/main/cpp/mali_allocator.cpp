#include "mali_allocator.h"
#include <algorithm>

uint32_t MaliAllocator::pickType(uint32_t bits) const {
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp_.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp_.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < mp_.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp_.memoryTypes[i].propertyFlags &
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return i;
    return UINT32_MAX;
}

bool MaliAllocator::init(VkPhysicalDevice pd, VkDevice dev) {
    pd_ = pd; dev_ = dev;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp_);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp_.memoryTypeCount; i++)
        if ((mp_.memoryTypes[i].propertyFlags & want) == want) { unified_ = true; typeIdx_ = i; break; }
    return true;
}

bool MaliAllocator::newBlock(VkDeviceSize minSize) {
    VkDeviceSize sz = std::max(kBlock, minSize);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = sz;
    ai.memoryTypeIndex = typeIdx_;
    Block b{};
    if (vkAllocateMemory(dev_, &ai, nullptr, &b.mem) != VK_SUCCESS) return false;
    if (vkMapMemory(dev_, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map) != VK_SUCCESS) {
        vkFreeMemory(dev_, b.mem, nullptr); return false;
    }
    b.size = sz;
    b.freeList.push_back({0, sz});
    blocks_.push_back(std::move(b));
    return true;
}

bool MaliAllocator::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                 VkBuffer& buf, MaliAlloc& a) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev_, &bi, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, buf, &req);
    if (pickType(req.memoryTypeBits) == UINT32_MAX) { vkDestroyBuffer(dev_, buf, nullptr); return false; }
    if (!unified_) typeIdx_ = pickType(req.memoryTypeBits);

    std::lock_guard<std::mutex> lk(mu_);
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t bi2 = 0; bi2 < blocks_.size(); bi2++) {
            Block& b = blocks_[bi2];
            for (size_t i = 0; i < b.freeList.size(); i++) {
                Range r = b.freeList[i];
                VkDeviceSize off = (r.off + req.alignment - 1) & ~(req.alignment - 1);
                if (off + req.size > r.off + r.size) continue;
                b.freeList.erase(b.freeList.begin() + i);
                if (off > r.off) b.freeList.push_back({r.off, off - r.off});
                VkDeviceSize end = off + req.size, rend = r.off + r.size;
                if (end < rend) b.freeList.push_back({end, rend - end});
                if (vkBindBufferMemory(dev_, buf, b.mem, off) != VK_SUCCESS) {
                    vkDestroyBuffer(dev_, buf, nullptr); return false;
                }
                a.memory = b.mem; a.offset = off; a.size = req.size;
                a.mapped = static_cast<char*>(b.map) + off; a.block = bi2;
                return true;
            }
        }
        if (pass == 0 && !newBlock(req.size + req.alignment)) break;
    }
    vkDestroyBuffer(dev_, buf, nullptr);
    return false;
}

void MaliAllocator::destroyBuffer(VkBuffer buf, const MaliAlloc& a) {
    vkDestroyBuffer(dev_, buf, nullptr);
    std::lock_guard<std::mutex> lk(mu_);
    if (a.block >= blocks_.size()) return;
    auto& fl = blocks_[a.block].freeList;
    fl.push_back({a.offset, a.size});
    std::sort(fl.begin(), fl.end(), [](const Range& x, const Range& y){ return x.off < y.off; });
    std::vector<Range> m;
    for (auto& r : fl) {
        if (!m.empty() && m.back().off + m.back().size == r.off) m.back().size += r.size;
        else m.push_back(r);
    }
    fl.swap(m);
}

void MaliAllocator::destroy() {
    for (auto& b : blocks_) { vkUnmapMemory(dev_, b.mem); vkFreeMemory(dev_, b.mem, nullptr); }
    blocks_.clear();
}
