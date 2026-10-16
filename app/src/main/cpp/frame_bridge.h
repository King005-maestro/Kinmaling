#pragma once
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <android/hardware_buffer.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fb {
constexpr uint32_t kMagic = 0x3142464D;
enum MsgType : uint32_t { MSG_HELLO = 1, MSG_FREE = 2, MSG_PRESENT = 3 };
struct Msg { uint32_t magic, type, a, b, c, d; };
}

class FrameBridge {
public:
    static bool selectExtensions(VkPhysicalDevice pd, std::vector<const char*>& out, bool& syncFd);
    bool init(VkDevice dev, uint32_t queueFamily, uint32_t w, uint32_t h,
              uint32_t slots, bool syncFdEnabled);
    bool start(const std::string& socketPath);
    void stop();
    bool beginWrite(uint32_t& slot, void*& ptr, uint32_t& strideBytes);
    bool commitWrite(uint32_t slot);
    bool recordBlit(VkCommandBuffer cb, VkImage dst, VkExtent2D dstExt, VkSemaphore& waitSem,
                    VkSurfaceTransformFlagBitsKHR xform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR);
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }

private:
    enum class State { Free, Client, Ready, Presenting };
    struct Slot {
        AHardwareBuffer* ahb = nullptr;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkSemaphore sem = VK_NULL_HANDLE;
        State state = State::Free;
        int syncFd = -1;
        uint64_t seq = 0;
        uint32_t strideBytes = 0;
    };
    bool importSlot(Slot& s);
    void serverLoop();
    void acceptClient();
    void handleClient();
    void dropClientLocked();
    void releaseLocked(uint32_t i);
    bool markReadyLocked(uint32_t i, int fd);
    bool sendMsg(int fd, uint32_t type, uint32_t a, uint32_t b, uint32_t c, uint32_t d, int flags);
    int recvMsg(int fd, fb::Msg& m, int& outFd);

    VkDevice dev_ = VK_NULL_HANDLE;
    uint32_t qf_ = 0, w_ = 0, h_ = 0;
    bool syncFd_ = false, linear_ = false;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getProps_ = nullptr;
    PFN_vkImportSemaphoreFdKHR importSem_ = nullptr;
    std::vector<Slot> slots_;
    int held_ = -1;
    uint64_t seq_ = 0;
    std::mutex mu_;
    std::string path_;
    int listen_ = -1, client_ = -1, wake_ = -1;
    std::thread thr_;
    bool running_ = false;
};
