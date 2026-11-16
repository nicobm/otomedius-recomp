/* memory.c - the emulated 32-bit address space and the PE image loader. */

#define _GNU_SOURCE
#include "win32.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>

int g_verbose = 1;   /* OTOMEDIUS_LOG=0 silences everything */

void log_printf(const char *fmt, ...)
{
    va_list ap;
    if (!g_verbose)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---------------------------------------------------------------- memory */
#define TEB_ADDR   0x7FFDE000u
#define PEB_ADDR   0x7FFDF000u
#define STACK_TOP  0x00A00000u
#define STACK_SIZE (8u * 1024u * 1024u)

static uint32_t g_image_base;
static uint32_t g_image_size;
static uint32_t g_entry;

uint32_t pe_image_base(void) { return g_image_base; }
uint32_t pe_image_size(void) { return g_image_size; }
uint32_t x86_entry_address(void) { return g_entry; }
uint32_t x86_stack_top(void) { return STACK_TOP; }
uint32_t x86_teb_address(void) { return TEB_ADDR; }

int x86_mem_init(void)
{
    void *p = mmap(NULL, (size_t)X86_MEM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "memory: cannot reserve 4 GiB of address space: %s\n",
                strerror(errno));
        return -1;
    }
    MEM = (uint8_t *)p;
    return 0;
}

/* --------------------------------------------------------------- PE loader */
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint8_t *read_file(const char *path, size_t *size_out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return NULL;
    }
    size_t size = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(size ? size : 1);
    if (!buf) {
        close(fd);
        return NULL;
    }
    size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, buf + got, size - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    *size_out = got;
    return buf;
}

static void setup_teb(void)
{
    cpu_t *c = x86_cpu();
    c->teb = TEB_ADDR;
    memset(MEM + TEB_ADDR, 0, 0x2000);
    memset(MEM + PEB_ADDR, 0, 0x200);
    M32(TEB_ADDR + 0x00) = 0xFFFFFFFFu;        /* SEH chain terminator */
    M32(TEB_ADDR + 0x04) = STACK_TOP;          /* stack base */
    M32(TEB_ADDR + 0x08) = STACK_TOP - STACK_SIZE;
    M32(TEB_ADDR + 0x18) = TEB_ADDR;
    M32(TEB_ADDR + 0x1C) = PEB_ADDR;
    M32(TEB_ADDR + 0x24) = 1;                  /* process id */
    M32(TEB_ADDR + 0x28) = 1;                  /* thread id */
    M32(TEB_ADDR + 0x2C) = TEB_ADDR + 0x1000;  /* TLS slots */
    M32(TEB_ADDR + 0x34) = 0;                  /* last error */
    M32(PEB_ADDR + 0x00) = 0;
    M32(PEB_ADDR + 0x08) = g_image_base;
    M32(PEB_ADDR + 0x18) = PEB_ADDR + 0x20;
    M32(PEB_ADDR + 0x30) = 0x0501;             /* NT 5.1 */
    c->esp = STACK_TOP - 0x1000;
    c->ebp = c->esp;
}

int x86_load_image(const char *exe_path)
{
    size_t size = 0;
    uint8_t *file = read_file(exe_path, &size);
    if (!file) {
        fprintf(stderr, "loader: cannot read %s\n", exe_path);
        return -1;
    }
    if (size < 0x200 || file[0] != 'M' || file[1] != 'Z') {
        fprintf(stderr, "loader: %s is not an MZ/PE image\n", exe_path);
        return -1;
    }
    uint32_t lfanew = rd32(file + 0x3C);
    if (lfanew + 0x200 > size || memcmp(file + lfanew, "PE\0\0", 4) != 0) {
        fprintf(stderr, "loader: %s has no PE signature\n", exe_path);
        return -1;
    }
    const uint8_t *coff = file + lfanew + 4;
    uint16_t nsec = rd16(coff + 2);
    uint16_t opt_size = rd16(coff + 16);
    const uint8_t *opt = coff + 20;
    if (rd16(opt) != 0x10B) {
        fprintf(stderr, "loader: %s is not a PE32 image\n", exe_path);
        return -1;
    }
    g_image_base = rd32(opt + 28);
    g_image_size = rd32(opt + 56);
    uint32_t headers = rd32(opt + 60);
    g_entry = g_image_base + rd32(opt + 16);
    if ((uint64_t)g_image_base + (uint64_t)g_image_size > X86_MEM_SIZE) {
        fprintf(stderr, "loader: image does not fit the 32-bit space\n");
        return -1;
    }
    memcpy(MEM + g_image_base, file, headers);
    const uint8_t *sec = opt + opt_size;
    for (int i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsize = rd32(sec + 8), va = rd32(sec + 12);
        uint32_t rawsize = rd32(sec + 16), raw = rd32(sec + 20);
        if (!vsize && !rawsize)
            continue;
        uint32_t n = rawsize;
        if (raw + n > size)
            n = (uint32_t)(size - raw);
        if (n && va + n <= g_image_size)
            memcpy(MEM + g_image_base + va, file + raw, n);
    }
    free(file);
    setup_teb();
    log_printf("loader: image %08x-%08x entry %08x", g_image_base,
               g_image_base + g_image_size, g_entry);
    if (bind_imports(g_image_base) != 0)
        return -1;
    return 0;
}
