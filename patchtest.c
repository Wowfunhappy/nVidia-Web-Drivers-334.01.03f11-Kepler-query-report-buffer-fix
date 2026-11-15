/*
 * patchtest: runs the code bytes of a patched GeForceGLDriverWeb without
 * installing it.  When the driver is loaded, the regions changed by the
 * patch (the code in the load-command padding and the hook in the
 * query-buffer-changed callback) are copied from the patched file's x86_64
 * slice into the loaded driver, after checking that the loaded bytes are
 * the stock ones.  The header edit (removing LC_CODE_SIGNATURE) has no
 * effect once the driver is loaded and is not reproduced.
 *
 * Environment: PATCHTEST_FILE=<path to GeForceGLDriverWeb.patched>
 * Reports via syslog (tag "patchtest").
 *
 * Build: clang -O2 -Wall -arch x86_64 -dynamiclib -o patchtest.dylib patchtest.c
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define DRIVER_IMAGE "/GeForceGLDriverWeb.bundle/Contents/MacOS/GeForceGLDriverWeb"
#define SLICE_OFFSET 23154688
#define TEXT_VMADDR  0x123440000000ULL

static const struct { uint64_t offset, length; } regions[] = {
    { 0xd30, 0x180 },      /* the load-command padding, which holds the code */
    { 0x309094, 6 },       /* jump at the start of the callback */
};

static int writeCode(void *addr, const void *bytes, size_t len)
{
    vm_address_t page = (vm_address_t)addr & ~(vm_address_t)(vm_page_size - 1);
    vm_size_t size = ((vm_address_t)addr + len - page + vm_page_size - 1) & ~(vm_size_t)(vm_page_size - 1);
    if (vm_protect(mach_task_self(), page, size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS)
        return 0;
    memcpy(addr, bytes, len);
    vm_protect(mach_task_self(), page, size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    return 1;
}

static void imageAdded(const struct mach_header *mh, intptr_t slide)
{
    Dl_info info;
    if (!dladdr(mh, &info) || !info.dli_fname || !strstr(info.dli_fname, DRIVER_IMAGE)) return;
    const char *path = getenv("PATCHTEST_FILE");
    int fd = path ? open(path, O_RDONLY) : -1;
    if (fd < 0) { syslog(LOG_NOTICE, "patchtest: cannot open PATCHTEST_FILE"); return; }
    int fdStock = open(info.dli_fname, O_RDONLY);
    for (size_t i = 0; i < sizeof regions / sizeof regions[0]; i++) {
        uint8_t patched[512], stock[512];
        uint64_t len = regions[i].length, off = regions[i].offset;
        uint8_t *mem = (uint8_t *)(TEXT_VMADDR + off + slide);
        if (pread(fd, patched, len, SLICE_OFFSET + off) != (ssize_t)len ||
            pread(fdStock, stock, len, SLICE_OFFSET + off) != (ssize_t)len ||
            memcmp(mem, stock, len)) {
            syslog(LOG_NOTICE, "patchtest: region %#llx does not hold the stock bytes; nothing written", off);
            close(fd); close(fdStock);
            return;
        }
        if (!writeCode(mem, patched, len)) { syslog(LOG_NOTICE, "patchtest: write failed"); return; }
    }
    close(fd); close(fdStock);
    syslog(LOG_NOTICE, "patchtest: loaded the patched driver code in %s", getprogname());
}

__attribute__((constructor))
static void patchtestInit(void)
{
    _dyld_register_func_for_add_image(imageAdded);
}
