/* win32.h - internal helpers shared by the native Win32 modules. */
#ifndef WIN32_H
#define WIN32_H

#include "x86rt.h"

typedef struct {
    const char *dll;
    const char *name;
    void (*fn)(void);
    int nargs;          /* stdcall arguments popped by the callee, 0 = cdecl */
} import_def_t;

static inline uint32_t arg32(int i)
{
    return M32(x86_cpu()->esp + 4u + 4u * (uint32_t)i);
}

static inline uint64_t arg64(int i)
{
    uint32_t p = x86_cpu()->esp + 4u + 4u * (uint32_t)i;
    return (uint64_t)M32(p) | ((uint64_t)M32(p + 4) << 32);
}

static inline const char *arg_str(int i)
{
    uint32_t p = arg32(i);
    return p ? (const char *)(MEM + p) : NULL;
}

static inline void *arg_ptr(int i)
{
    uint32_t p = arg32(i);
    return p ? (void *)(MEM + p) : NULL;
}

#define RET32(v) (x86_cpu()->eax = (uint32_t)(v))
#define RET64(v) do { uint64_t r_ = (uint64_t)(v); \
    x86_cpu()->eax = (uint32_t)r_; x86_cpu()->edx = (uint32_t)(r_ >> 32); } while (0)

/* Last Win32 error, mirrors the game's view of SetLastError/GetLastError. */
void     win_set_last_error(uint32_t err);
uint32_t win_last_error(void);

/* Path translation: ".\Data\x.pak" and "E:\nvmem\y.bin" both live in the game
 * directory.  Returns a host malloc'd string. */
char *win_host_path(const char *guest);
const char *win_game_dir(void);

/* Window / message plumbing (user32.c) */
void  win_request_quit(void);
int   win_pump(void);

/* Platform (SDL3): returns 0 on success. */
int   platform_init(void);
void  platform_shutdown(void);
void  platform_frame_begin(void);
void  platform_frame_end(void);

/* Input: arcade PJEI control status buffer (see avs.c). */
void  avs_poll_input(uint8_t *buf);

/* d3d9.c */
void  d3d_init(void);

/* loader / runtime entry points */
int      bind_imports(uint32_t base);
int      x86_mem_init(void);
int      x86_load_image(const char *exe_path);
uint32_t x86_entry_address(void);
uint32_t x86_stack_top(void);
uint32_t x86_teb_address(void);
int      x86_thread_begin(uint32_t start, uint32_t param, uint32_t stack_top, uint32_t teb);
uint32_t pe_image_base(void);
uint32_t pe_image_size(void);

/* native thunks (imports.c) */
uint32_t x86_native_register(void (*fn)(void), int nargs);
void     x86_native_invoke(uint32_t thunk);
int      x86_is_native_thunk(uint32_t addr);
uint32_t x86_native_entry(uint32_t thunk);

/* dshow.c */
int      dshow_create_filter_graph(uint32_t rclsid, uint32_t riid, uint32_t ppv);

/* d3d9_objects.c */
void     texture_upload(uint32_t tex, const void *data, uint32_t size, int fmt);
uint32_t texture_data_of(uint32_t tex);

/* verbose logging switch (OTOMEDIUS_LOG=1) */
void  log_printf(const char *fmt, ...);
extern int g_verbose;

#endif
