#include "vk_context.h"
#include <cstring>
#include <algorithm>

bool VkContext::init(ANativeWindow* win) {
    const char* iext[] = {"VK_KHR_surface", "VK_KHR_android_surface"};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app; ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = iext;
    if (vkCreateInstance(&ici, nullptr, &inst_) != VK_SUCCESS) return false;

    VkAndroidSurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    si.window = win;
    if (vkCreateAndroidSurfaceKHR(inst_, &si, nullptr, &surf_) != VK_SUCCESS) return false;

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst_, &n, nullptr);
    if (!n) return false;
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(inst_, &n, pds.data());
    pd_ = pds[0];

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qp(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd_, &qn, qp.data());
    bool found = false;
    for (uint32_t i = 0; i < qn; i++) {
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd_, i, surf_, &ok);
        if (ok && (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { qf_ = i; found = true; break; }
    }
    if (!found) return false;

    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(pd_, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> exts(en);
    vkEnumerateDeviceExtensionProperties(pd_, nullptr, &en, exts.data());
    for (auto& e : exts) if (!strcmp(e.extensionName, "VK_EXT_fragment_density_map")) fdm_ = true;

    std::vector<const char*> dext = {"VK_KHR_swapchain"};
    if (fdm_) dext.push_back("VK_EXT_fragment_density_map");
    
    bool syncFd = false;
    bool bridgeExt = FrameBridge::selectExtensions(pd_, dext, syncFd);

    float prio = 1.f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = qf_; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT fdf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
    fdf.fragmentDensityMap = VK_TRUE;
    
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = fdm_ ? &fdf : nullptr;
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = (uint32_t)dext.size(); di.ppEnabledExtensionNames = dext.data();
    if (vkCreateDevice(pd_, &di, nullptr, &dev_) != VK_SUCCESS) return false;
    vkGetDeviceQueue(dev_, qf_, 0, &q_);

    alloc_.init(pd_, dev_);
    
    bridgeOn_ = false;
    if (bridgeExt) {
        if (bridge_.init(dev_, qf_, 1280, 720, 4, syncFd)) {
            if (bridge_.start("/data/data/com.emu.mali/files/framebridge.sock")) {
                bridgeOn_ = true;
            }
        }
    }

    if (!createSwapchain()) return false;

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = qf_;
    vkCreateCommandPool(dev_, &pi, nullptr, &pool_);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev_, &ai, &cb_);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(dev_, &sci, nullptr, &semAcq_);
    vkCreateSemaphore(dev_, &sci, nullptr, &semDone_);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(dev_, &fi, nullptr, &fence_);
    return true;
}

bool VkContext::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd_, surf_, &caps);
    ext_ = caps.currentExtent;
    preTransform_ = caps.currentTransform;
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd_, surf_, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> fs(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd_, surf_, &fn, fs.data());
    fmt_ = fs[0].format;
    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = surf_;
    sc.minImageCount = std::max(3u, caps.minImageCount);
    if (caps.maxImageCount && sc.minImageCount > caps.maxImageCount) sc.minImageCount = caps.maxImageCount;
    sc.imageFormat = fmt_; sc.imageColorSpace = fs[0].colorSpace;
    sc.imageExtent = ext_; sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    sc.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    uint32_t pn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd_, surf_, &pn, nullptr);
    std::vector<VkPresentModeKHR> pm(pn);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd_, surf_, &pn, pm.data());
    if (std::find(pm.begin(), pm.end(), VK_PRESENT_MODE_MAILBOX_KHR) == pm.end())
        sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc.clipped = VK_TRUE;
    
    if (vkCreateSwapchainKHR(dev_, &sc, nullptr, &sc_) != VK_SUCCESS) return false;
    uint32_t imgCount = 0;
    vkGetSwapchainImagesKHR(dev_, sc_, &imgCount, nullptr);
    imgs_.resize(imgCount);
    vkGetSwapchainImagesKHR(dev_, sc_, &imgCount, imgs_.data());
    return true;
}

bool VkContext::present(float r, float g, float b) {
    vkWaitForFences(dev_, 1, &fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(dev_, 1, &fence_);

    uint32_t idx = 0;
    VkResult res = vkAcquireNextImageKHR(dev_, sc_, UINT64_MAX, semAcq_, VK_NULL_HANDLE, &idx);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) { createSwapchain(); return true; }

    vkResetCommandBuffer(cb_, 0);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb_, &beginInfo);

    VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    
    VkImageMemoryBarrier br{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    br.srcAccessMask = 0; br.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    br.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; br.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    br.image = imgs_[idx]; br.subresourceRange = rg;
    
    vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &br);
    
    VkSemaphore extWait = VK_NULL_HANDLE;
    bool drew = bridgeOn_ && bridge_.recordBlit(cb_, imgs_[idx], ext_, extWait, preTransform_);
    if (!drew) {
        VkClearColorValue c = {{r, g, b, 1.f}};
        vkCmdClearColorImage(cb_, imgs_[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rg);
    }
    
    br.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; br.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    br.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; br.dstAccessMask = 0;
    vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &br);
    
    vkEndCommandBuffer(cb_);
    
    VkSemaphore waits[2] = {semAcq_, extWait};
    VkPipelineStageFlags wst[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    s.waitSemaphoreCount = extWait ? 2 : 1; s.pWaitSemaphores = waits; s.pWaitDstStageMask = wst;
    s.commandBufferCount = 1; s.pCommandBuffers = &cb_;
    s.signalSemaphoreCount = 1; s.pSignalSemaphores = &semDone_;
    vkQueueSubmit(q_, 1, &s, fence_);

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &semDone_;
    pi.swapchainCount = 1; pi.pSwapchains = &sc_; pi.pImageIndices = &idx;
    res = vkQueuePresentKHR(q_, &pi);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) createSwapchain();

    return true;
}

void VkContext::destroy() {
    if (bridgeOn_) { 
        bridge_.stop(); 
        bridgeOn_ = false; 
    }
    vkDeviceWaitIdle(dev_);
    alloc_.destroy();
    vkDestroyFence(dev_, fence_, nullptr);
    vkDestroySemaphore(dev_, semDone_, nullptr);
    vkDestroySemaphore(dev_, semAcq_, nullptr);
    vkDestroyCommandPool(dev_, pool_, nullptr);
    vkDestroySwapchainKHR(dev_, sc_, nullptr);
    vkDestroySurfaceKHR(inst_, surf_, nullptr);
    vkDestroyDevice(dev_, nullptr);
    vkDestroyInstance(inst_, nullptr);
}
