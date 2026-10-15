#include "box64_launcher.h"
#include "cpu_topology.h"
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <atomic>

static std::atomic<pid_t> g_child{-1};

int launchBox64(const std::string& box64, const std::string& wine,
                const std::string& exe, const std::string& wd,
                const std::string& log) {
    cpu_set_t mask = bigCoreMask();
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        sched_setaffinity(0, sizeof(mask), &mask);
        if (!wd.empty() && chdir(wd.c_str()) != 0) _exit(126);
        int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        setenv("BOX64_DYNAREC", "1", 1);
        setenv("BOX64_DYNAREC_BIGBLOCK", "2", 1);
        setenv("BOX64_DYNAREC_STRONGMEM", "1", 1);
        setenv("BOX64_DYNAREC_FASTNAN", "1", 1);
        setenv("BOX64_DYNAREC_FASTROUND", "1", 1);
        setenv("BOX64_LOG", "0", 1);
        setenv("DXVK_ASYNC", "1", 1);
        setenv("MESA_VK_WSI_PRESENT_MODE", "mailbox", 1);
        execl(box64.c_str(), box64.c_str(), wine.c_str(), exe.c_str(), (char*)nullptr);
        _exit(127);
    }
    g_child = pid;
    int st = 0;
    waitpid(pid, &st, 0);
    g_child = -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

void stopBox64() {
    pid_t p = g_child.load();
    if (p > 0) kill(p, SIGTERM);
}
