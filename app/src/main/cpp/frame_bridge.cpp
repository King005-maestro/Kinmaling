#include "frame_bridge.h"
#include <android/log.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <algorithm>

#define FLOG(...) __android_log_print(ANDROID_LOG_INFO, "framebridge", __VA_ARGS__)

bool FrameBridge::selectExtensions(VkPhysicalDevice pd, std::vector<const char*>& out, bool& syncFd) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> e(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, e.data());
    auto has = [&](const char* name) {
        for (auto& x : e) if (!strcmp(x.extensionName, name)) return true;
        return false;
    };
    syncFd = false;
    if (!has("VK_ANDROID_external_memory_android_hardware_buffer") ||
        !has("VK_EXT_queue_family_foreign")) return false;
    out.push_back("VK_ANDROID_external_memory_android_hardware_buffer");
    out.push_back("VK_EXT_queue_family_foreign");
    if (has("VK_KHR_external_semaphore_fd")) {
                out.push_back("VK_KHR_external_semaphore_fd");
        syncFd = true;
    }
    return true;
}

bool FrameBridge::importSlot(Slot& s) {
    VkAndroidHardwareBufferFormatPropertiesANDROID fp{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID pr{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    pr.pNext = &fp;
    if (getProps_(dev_, s.ahb, &pr) != VK_SUCCESS) return false;
    if (fp.format == VK_FORMAT_UNDEFINED) { FLOG("external format not supported"); return false; }
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    if ((fp.formatFeatures & need) != need) { FLOG("AHB format lacks blit src"); return false; }
    linear_ = (fp.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;

    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.pNext = &ext;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fp.format;
    ii.extent = {w_, h_, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev_, &ii, nullptr, &s.image) != VK_SUCCESS) return false;

    VkImportAndroidHardwareBufferInfoANDROID imp{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    imp.buffer = s.ahb;
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.pNext = &imp; ded.image = s.image;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &ded;
    mai.allocationSize = pr.allocationSize;
    mai.memoryTypeIndex = pr.memoryTypeBits ? (uint32_t)__builtin_ctz(pr.memoryTypeBits) : 0;
    if (vkAllocateMemory(dev_, &mai, nullptr, &s.mem) != VK_SUCCESS) return false;
    if (vkBindImageMemory(dev_, s.image, s.mem, 0) != VK_SUCCESS) return false;

    if (syncFd_) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(dev_, &sci, nullptr, &s.sem) != VK_SUCCESS) return false;
    }
    return true;
}

bool FrameBridge::init(VkDevice dev, uint32_t qf, uint32_t w, uint32_t h,
                       uint32_t slots, bool syncFdEnabled) {
    dev_ = dev; qf_ = qf; w_ = w; h_ = h;
    syncFd_ = syncFdEnabled;
    getProps_ = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)
        vkGetDeviceProcAddr(dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
    importSem_ = (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(dev, "vkImportSemaphoreFdKHR");
    if (!getProps_) return false;
    if (!importSem_) syncFd_ = false;
    if (slots < 3) slots = 3;
    if (slots > 8) slots = 8;

    slots_.assign(slots, Slot{});
    for (auto& s : slots_) {
        AHardwareBuffer_Desc d{};
        d.width = w; d.height = h; d.layers = 1;
        d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                  AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
        if (AHardwareBuffer_allocate(&d, &s.ahb) != 0) { FLOG("AHB alloc failed"); return false; }
        AHardwareBuffer_describe(s.ahb, &d);
        s.strideBytes = d.stride * 4;
        if (!importSlot(s)) return false;
    }
    return true;
}

bool FrameBridge::sendMsg(int fd, uint32_t type, uint32_t a, uint32_t b, uint32_t c, uint32_t d, int flags) {
    fb::Msg m{fb::kMagic, type, a, b, c, d};
    return send(fd, &m, sizeof m, MSG_NOSIGNAL | flags) == (ssize_t)sizeof m;
}

int FrameBridge::recvMsg(int fd, fb::Msg& m, int& outFd) {
    outFd = -1;
    iovec iov{&m, sizeof m};
    alignas(cmsghdr) char ctl[CMSG_SPACE(sizeof(int))];
    memset(ctl, 0, sizeof ctl);
    msghdr mh{};
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    mh.msg_control = ctl; mh.msg_controllen = sizeof ctl;
    ssize_t n = recvmsg(fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n == 0) return 0;
    if (n < 0) return (errno == EAGAIN || errno == EINTR) ? -1 : 0;
    for (cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int got = -1;
            memcpy(&got, CMSG_DATA(c), sizeof(int));
            if (outFd < 0) outFd = got; else if (got >= 0) close(got);
        }
    }
    if (n != (ssize_t)sizeof m || m.magic != fb::kMagic || (mh.msg_flags & MSG_CTRUNC)) {
        if (outFd >= 0) { close(outFd); outFd = -1; }
        return 0;
    }
    return 1;
}

void FrameBridge::releaseLocked(uint32_t i) {
    Slot& s = slots_[i];
    if (s.syncFd >= 0) { close(s.syncFd); s.syncFd = -1; }
    if (client_ >= 0) {
        s.state = State::Client;
        sendMsg(client_, fb::MSG_FREE, i, 0, 0, 0, MSG_DONTWAIT);
    } else {
        s.state = State::Free;
    }
}

bool FrameBridge::markReadyLocked(uint32_t i, int fd) {
    if (i >= slots_.size() || slots_[i].state != State::Client) {
        if (fd >= 0) close(fd);
        return false;
    }
    for (uint32_t k = 0; k < slots_.size(); k++)
        if (slots_[k].state == State::Ready) releaseLocked(k);
    Slot& s = slots_[i];
    s.state = State::Ready;
    s.seq = ++seq_;
    if (syncFd_) {
        s.syncFd = fd;
    } else {
        s.syncFd = -1;
        if (fd >= 0) close(fd);
    }
    return true;
}

bool FrameBridge::beginWrite(uint32_t& slot, void*& ptr, uint32_t& strideBytes) {
    std::lock_guard<std::mutex> lk(mu_);
    for (uint32_t i = 0; i < slots_.size(); i++) {
        Slot& s = slots_[i];
        if (s.state != State::Free) continue;
        void* p = nullptr;
        if (AHardwareBuffer_lock(s.ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &p) != 0)
            return false;
        s.state = State::Client;
        slot = i; ptr = p; strideBytes = s.strideBytes;
        return true;
    }
    return false;
}

bool FrameBridge::commitWrite(uint32_t slot) {
    std::lock_guard<std::mutex> lk(mu_);
    if (slot >= slots_.size() || slots_[slot].state != State::Client) return false;
    AHardwareBuffer_unlock(slots_[slot].ahb, nullptr);
    return markReadyLocked(slot, -1);
}

bool FrameBridge::start(const std::string& path) {
    path_ = path;
    listen_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listen_ < 0) return false;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (path.size() >= sizeof a.sun_path) return false;
    strcpy(a.sun_path, path.c_str());
    unlink(path.c_str());
    if (bind(listen_, (sockaddr*)&a, sizeof a) != 0) return false;
    chmod(path.c_str(), 0600);
    if (listen(listen_, 1) != 0) return false;
    wake_ = eventfd(0, EFD_CLOEXEC);
    if (wake_ < 0) return false;
    running_ = true;
    thr_ = std::thread([this] { serverLoop(); });
    return true;
}

void FrameBridge::serverLoop() {
    while (running_) {
        pollfd p[3];
        p[0] = {wake_, POLLIN, 0};
        p[1] = {listen_, POLLIN, 0};
        nfds_t n = 2;
        int cfd;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cfd = client_;
        }
        if (cfd >= 0) { p[2] = {cfd, POLLIN, 0}; n = 3; }
        int r = poll(p, n, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (p[0].revents) break;
        if (p[1].revents & POLLIN) acceptClient();
        if (n == 3 && (p[2].revents & (POLLIN | POLLHUP | POLLERR))) handleClient();
    }
}

void FrameBridge::acceptClient() {
    int fd = accept4(listen_, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        dropClientLocked();
    }
    bool ok = sendMsg(fd, fb::MSG_HELLO, w_, h_, (uint32_t)slots_.size(),
                      AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 0);
    for (size_t i = 0; ok && i < slots_.size(); i++)
        ok = AHardwareBuffer_sendHandleToUnixSocket(slots_[i].ahb, fd) == 0;
    if (!ok) { close(fd); return; }
    std::lock_guard<std::mutex> lk(mu_);
    client_ = fd;
    for (uint32_t i = 0; i < slots_.size(); i++)
        if (slots_[i].state == State::Free) releaseLocked(i);
}

void FrameBridge::handleClient() {
    for (;;) {
        int cfd;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cfd = client_;
        }
        if (cfd < 0) return;
        fb::Msg m;
        int fd = -1;
        int r = recvMsg(cfd, m, fd);
        std::lock_guard<std::mutex> lk(mu_);
        if (r == 0) { dropClientLocked(); return; }
        if (r < 0) return;
        if (m.type == fb::MSG_PRESENT) {
            markReadyLocked(m.a, (m.b && fd >= 0) ? fd : -1);
            if (!(m.b && fd >= 0) && fd >= 0) close(fd);
        } else if (fd >= 0) {
            close(fd);
        }
    }
}

void FrameBridge::dropClientLocked() {
    if (client_ >= 0) { close(client_); client_ = -1; }
    for (auto& s : slots_)
        if (s.state == State::Client) s.state = State::Free;
}

bool FrameBridge::recordBlit(VkCommandBuffer cb, VkImage dst, VkExtent2D de, VkSemaphore& waitSem,
                             VkSurfaceTransformFlagBitsKHR xform) {
    waitSem = VK_NULL_HANDLE;
    std::lock_guard<std::mutex> lk(mu_);
    int pick = -1;
    for (uint32_t i = 0; i < slots_.size(); i++)
        if (slots_[i].state == State::Ready && (pick < 0 || slots_[i].seq > slots_[pick].seq)) pick = (int)i;
    if (pick >= 0) {
        if (held_ >= 0) releaseLocked((uint32_t)held_);
        held_ = pick;
        Slot& s = slots_[pick];
        s.state = State::Presenting;
        if (s.syncFd >= 0) {
            VkImportSemaphoreFdInfoKHR ii{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
            ii.semaphore = s.sem;
            ii.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
            ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            ii.fd = s.syncFd;
            if (importSem_(dev_, &ii) == VK_SUCCESS) waitSem = s.sem;
            else close(s.syncFd);
            s.syncFd = -1;
        }
    }
    if (held_ < 0) return false;
    Slot& s = slots_[held_];

    VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier acq{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    acq.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    acq.dstQueueFamilyIndex = qf_;
    acq.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    acq.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    acq.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    acq.image = s.image; acq.subresourceRange = rg;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &acq);

    VkClearColorValue black = {{0.f, 0.f, 0.f, 1.f}};
    vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rg);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);

    float sc = std::min((float)de.width / (float)w_, (float)de.height / (float)h_);
    int32_t rw = (int32_t)((float)w_ * sc), rh = (int32_t)((float)h_ * sc);
    int32_t x0 = ((int32_t)de.width - rw) / 2, y0 = ((int32_t)de.height - rh) / 2;
    int32_t x1 = x0 + rw, y1 = y0 + rh;

    bool flipX = false, flipY = false;
    switch (xform) {
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
            flipX = true; flipY = true; break;
        case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_BIT_KHR:
            flipX = true; break;
        case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_180_BIT_KHR:
            flipY = true; break;
        default: break;
    }
    if (flipX) std::swap(x0, x1);
    if (flipY) std::swap(y0, y1);

    VkImageBlit bl{};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[0] = {0, 0, 0};
    bl.srcOffsets[1] = {(int32_t)w_, (int32_t)h_, 1};
    bl.dstOffsets[0] = {x0, y0, 0};
    bl.dstOffsets[1] = {x1, y1, 1};
    vkCmdBlitImage(cb, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                   linear_ ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

    VkImageMemoryBarrier rel = acq;
    rel.srcQueueFamilyIndex = qf_;
    rel.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    rel.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    rel.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    rel.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    rel.dstAccessMask = 0;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &rel);
    return true;
}

void FrameBridge::stop() {
    if (running_) {
        running_ = false;
        uint64_t one = 1;
        if (write(wake_, &one, sizeof one) < 0) {}
        if (thr_.joinable()) thr_.join();
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (client_ >= 0) { close(client_); client_ = -1; }
    }
    if (listen_ >= 0) { close(listen_); listen_ = -1; unlink(path_.c_str()); }
    if (wake_ >= 0) { close(wake_); wake_ = -1; }
    for (auto& s : slots_) {
        if (s.syncFd >= 0) close(s.syncFd);
        if (dev_) {
            if (s.sem) vkDestroySemaphore(dev_, s.sem, nullptr);
            if (s.image) vkDestroyImage(dev_, s.image, nullptr);
            if (s.mem) vkFreeMemory(dev_, s.mem, nullptr);
        }
        if (s.ahb) AHardwareBuffer_release(s.ahb);
    }
    slots_.clear();
    held_ = -1;
}
