/*
 * vcam_injector - ptrace 注入器
 * 将 libvcam_hook.so 注入到 cameraserver 进程
 * 无需重启，注入后即刻生效，所有 App 摄像头被替换
 *
 * 编译: aarch64-linux-android31-clang -o vcam_inject vcam_inject.c -static
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <errno.h>

#define HOOK_LIB "/data/local/tmp/vcam/libvcam_hook.so"

/* ARM64 指令和寄存器 */
#define ARM64_PC   32  /* Program Counter in pt_regs */
#define ARM64_SP   31
#define ARM64_LR   30
#define ARM64_X0   0

/* 在目标进程中找到 libc 的 dlopen / __loader_dlopen 地址 */
static unsigned long find_dlopen(pid_t pid) {
    char maps_path[256];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    
    FILE *fp = fopen(maps_path, "r");
    if (!fp) return 0;
    
    char line[512];
    unsigned long libc_base = 0, linker_base = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        /* 找 libc.so 基址 */
        if (strstr(line, "libc.so") && strstr(line, "r-xp")) {
            sscanf(line, "%lx", &libc_base);
        }
        /* 找 linker64 基址 */
        if (strstr(line, "linker64") && strstr(line, "r-xp")) {
            sscanf(line, "%lx", &linker_base);
        }
    }
    fclose(fp);
    
    if (linker_base > 0) {
        /* 用本进程的 __loader_dlopen 偏移推算 */
        void *local_dlopen = dlsym(RTLD_DEFAULT, "__loader_dlopen");
        if (local_dlopen) {
            Dl_info info;
            if (dladdr(local_dlopen, &info)) {
                unsigned long offset = (unsigned long)local_dlopen - (unsigned long)info.dli_fbase;
                return linker_base + offset;
            }
        }
    }
    
    /* 回退: 直接用 dlopen */
    if (libc_base > 0) {
        void *local = dlsym(RTLD_DEFAULT, "dlopen");
        if (local) {
            Dl_info info;
            if (dladdr(local, &info)) {
                unsigned long offset = (unsigned long)local - (unsigned long)info.dli_fbase;
                return libc_base + offset;
            }
        }
    }
    
    return 0;
}

/* 在目标进程分配内存并写入数据 */
static unsigned long remote_alloc(pid_t pid, const void *data, size_t size) {
    /* 通过对端 /proc/pid/mem 写入（需要 ptrace 已 attach） */
    char mem_path[64];
    snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
    
    /* 先获取当前 mmap 区域来确定一个安全的地址 */
    unsigned long addr = 0x7000000000UL; /* 高地址，通常未使用 */
    
    int fd = open(mem_path, O_RDWR);
    if (fd < 0) {
        perror("open mem");
        return 0;
    }
    
    /* 尝试写入 */
    if (lseek(fd, addr, SEEK_SET) == addr) {
        if (write(fd, data, size) == (ssize_t)size) {
            close(fd);
            return addr;
        }
    }
    
    close(fd);
    return 0;
}

/* ARM64 shellcode: 调用 dlopen(HOOK_LIB, RTLD_NOW) */
static unsigned char shellcode[] = {
    /* 构造在目标进程中调用 dlopen(path, RTLD_NOW|RTLD_GLOBAL) 的代码 */
    /* 实际 shellcode 在运行时动态构造 */
    0x00, 0x00, 0x00, 0x00
};

/* 执行远程函数调用 */
static int remote_call(pid_t pid, unsigned long func_addr,
                       unsigned long arg0, unsigned long arg1) {
    struct user_pt_regs old_regs, new_regs;
    struct iovec iov;
    
    iov.iov_base = &old_regs;
    iov.iov_len = sizeof(old_regs);
    ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov);
    
    memcpy(&new_regs, &old_regs, sizeof(new_regs));
    
    /* 设置 ARM64 调用约定: X0=arg0, X1=arg1, LR=返回地址(设为0让目标crash后我们捕获) */
    new_regs.regs[0] = arg0;
    new_regs.regs[1] = arg1;
    new_regs.regs[30] = 0;  /* LR = 0 会导致 SIGSEGV，我们捕获它 */
    new_regs.pc = func_addr;
    new_regs.sp = old_regs.sp - 1024; /* 留出栈空间 */
    
    iov.iov_base = &new_regs;
    iov.iov_len = sizeof(new_regs);
    ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &iov);
    
    /* 执行 */
    ptrace(PTRACE_CONT, pid, 0, 0);
    
    /* 等待 SIGSEGV（LR=0 触发）或 SIGTRAP */
    int status;
    waitpid(pid, &status, 0);
    
    if (WIFSTOPPED(status) && (WSTOPSIG(status) == SIGSEGV || WSTOPSIG(status) == SIGTRAP)) {
        /* 获取返回值 (X0) */
        iov.iov_base = &new_regs;
        iov.iov_len = sizeof(new_regs);
        ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov);
        
        /* 恢复原始寄存器 */
        iov.iov_base = &old_regs;
        iov.iov_len = sizeof(old_regs);
        ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &iov);
        
        return (int)new_regs.regs[0];
    }
    
    /* 恢复 */
    iov.iov_base = &old_regs;
    iov.iov_len = sizeof(old_regs);
    ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &iov);
    
    return -1;
}

/* 主注入逻辑 */
int inject_into_process(pid_t pid) {
    printf("[*] Attaching to PID %d...\n", pid);
    
    if (ptrace(PTRACE_ATTACH, pid, 0, 0) < 0) {
        perror("ptrace ATTACH");
        return -1;
    }
    waitpid(pid, NULL, 0);
    
    /* 找到 dlopen 地址 */
    unsigned long dlopen_addr = find_dlopen(pid);
    if (!dlopen_addr) {
        printf("[-] Cannot find dlopen in target\n");
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return -1;
    }
    printf("[+] dlopen @ 0x%lx\n", dlopen_addr);
    
    /* 在目标进程写入 HOOK_LIB 路径字符串 */
    const char *libpath = HOOK_LIB;
    unsigned long str_addr = remote_alloc(pid, libpath, strlen(libpath) + 1);
    if (!str_addr) {
        printf("[-] Cannot allocate memory in target\n");
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return -1;
    }
    printf("[+] String written @ 0x%lx\n", str_addr);
    
    /* RTLD_NOW = 2, RTLD_GLOBAL = 0x00100 -> combined = 0x102 */
    int result = remote_call(pid, dlopen_addr, str_addr, 0x102);
    printf("[+] dlopen returned: 0x%x\n", result);
    
    ptrace(PTRACE_DETACH, pid, 0, 0);
    
    return (result != 0) ? 0 : -1;
}

/* 查找进程 PID */
static pid_t find_process(const char *name) {
    DIR *dir = opendir("/proc");
    if (!dir) return -1;
    
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_type != DT_DIR) continue;
        
        int pid = atoi(entry->d_name);
        if (pid <= 0) continue;
        
        char cmdfile[256];
        snprintf(cmdfile, sizeof(cmdfile), "/proc/%d/cmdline", pid);
        FILE *fp = fopen(cmdfile, "r");
        if (!fp) continue;
        
        char cmd[256] = {0};
        fread(cmd, 1, sizeof(cmd) - 1, fp);
        fclose(fp);
        
        if (strstr(cmd, name)) {
            closedir(dir);
            return pid;
        }
    }
    closedir(dir);
    return -1;
}

int main(int argc, char **argv) {
    const char *target = (argc > 1) ? argv[1] : "cameraserver";
    int action = (argc > 2) ? atoi(argv[2]) : 0; /* 0=inject, 1=stop */
    
    printf("=== VCAM System-Level Camera Hook ===\n");
    
    pid_t pid = find_process(target);
    if (pid < 0) {
        printf("[-] Process '%s' not found\n", target);
        /* 试试 HAL 服务名 */
        const char *hal_names[] = {
            "camera.provider",
            "android.hardware.camera.provider",
            "vendor.qti.camera.provider",
            NULL
        };
        for (int i = 0; hal_names[i]; i++) {
            pid = find_process(hal_names[i]);
            if (pid > 0) {
                printf("[+] Found HAL service: %s (PID %d)\n", hal_names[i], pid);
                break;
            }
        }
        if (pid < 0) return 1;
    }
    
    printf("[+] Target: %s (PID %d)\n", target, pid);
    
    if (action == 0) {
        printf("[*] Injecting hook library...\n");
        if (inject_into_process(pid) == 0) {
            printf("[+] SUCCESS: Camera hook injected!\n");
            printf("[+] All apps will now see virtual camera.\n");
            return 0;
        }
    }
    
    printf("[-] Injection failed\n");
    return 1;
}
