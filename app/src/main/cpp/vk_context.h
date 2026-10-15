#pragma once
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <android/native_window.h>
#include <vector>
#include "mali_allocator.h"
#include "frame_bridge.h"

class VkContext {
public:
    bool init(ANativeWindow* win);
    bool present(float r, float g, float b);
    void destroy();
    bool hasFragmentDensityMap() const { return fdm_; }
    MaliAllocator& allocator() { return alloc_; }
    FrameBridge& bridge() { return bridge_; }
private:
    bool createSwapchain();
    VkInstance inst_ = VK_NULL_HANDLE;
    VkPhysicalDevice pd_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue q_ = VK_NULL_HANDLE;
    uint32_t qf_ = 0;
    VkSurfaceKHR surf_ = VK_NULL_HANDLE;
    VkSwapchainKHR sc_ = VK_NULL_HANDLE;
    std::vector<VkImage> imgs_;
    VkFormat fmt_ = VK_FORMAT_R8G8B8A8_UNORM;
    VkExtent2D ext_{};
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cb_ = VK_NULL_HANDLE;
    VkSemaphore semAcq_ = VK_NULL_HANDLE, semDone_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    bool fdm_ = false;
    MaliAllocator alloc_;
    FrameBridge bridge_;
    bool bridgeOn_ = false;
    VkSurfaceTransformFlagBitsKHR preTransform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
};
