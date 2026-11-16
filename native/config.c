/* config.c - native loader for the game's ini file.
 *
 * The cabinet read its tuning values from `oto.ini` (`set <name> <value>`
 * lines) and stored them in globals laid out by the table at 0x5BE328:
 * { const char *name; void *target; int type; } with type 3 = uint16,
 * 6 = uint32, 7 = float.  None of those targets are backed by file data, so
 * the image loads them as zero - and a zero `wSortDrawCmn2DMax` makes the
 * first CSortData registration overflow, which logs error 1115 and suspends
 * every manager (0x5DE220 = 0xFFFFFFFF).  The client behaves the same way if
 * the ini is missing, so parsing it here is what the cabinet OS did.
 *
 * The table is walked from the emulated image itself so the keys, targets and
 * widths always match the binary being recompiled.
 */

#include "win32.h"

#include <ctype.h>

#define INI_TABLE   0x5BE328u      /* {name, target, type} * n, NULL-terminated */
#define INI_ENTRY   12u

/* Defaults from the shipped otomedius.ini, used when the file cannot be read
 * (the manifest ships the ini, so this is only a safety net). */
static const struct { const char *name; long v; } ini_defaults[] = {
    {"dwLocalHeapSizeGameMan",   32768},
    {"dwLocalHeapSizeStageMan",   9216},
    {"dwLocalHeapSizeEnemyMan", 131072},
    {"dwLocalHeapSizeEShotMan", 131072},
    {"dwLocalHeapSizePlayerMan",  4096},
    {"dwLocalHeapSizePShotMan",  65536},
    {"dwLocalHeapSizeEffectMan", 262144},
    {"dwLocalHeapSizeHitUtl",   393216},
    {"wSortDrawCmn2DMax",          256},
    {"wSortDrawCmn3DMax",          512},
    {"wSortDrawEShotMax",          512},
    {"wSortDrawEffMuzzleMax",      256},
    {"wSortDrawEffEnemyMax",      2048},
    {"wSortDrawEffPlayerMax",      512},
    {"wSortDrawEffPShotMax",       128},
    {"wSortDrawPowCapMax",         256},
    {"wSortDrawElementMax",        512},
    {"wSortDrawPShotMax",          512},
    {"wSortDrawPOptionMax",         32},
};

static void ini_store(uint32_t target, int type, const char *value)
{
    if (!target)
        return;
    switch (type) {
    case 3:                          /* uint16 */
        M16(target) = (uint16_t)strtoul(value, NULL, 0);
        break;
    case 6:                          /* uint32 */
        M32(target) = (uint32_t)strtoul(value, NULL, 0);
        break;
    case 7:                          /* float */
        M32(target) = (uint32_t)0;
        *(float *)(MEM + target) = strtof(value, NULL);
        break;
    default:
        M32(target) = (uint32_t)strtoul(value, NULL, 0);
        break;
    }
}

/* Returns 1 when `name` was found in the game's table and stored. */
static int ini_apply(const char *name, const char *value)
{
    for (uint32_t e = INI_TABLE;; e += INI_ENTRY) {
        uint32_t key = M32(e);
        uint32_t target = M32(e + 4);
        int type = (int)M32(e + 8);
        if (!key)
            return 0;
        if (strcmp((const char *)(MEM + key), name) != 0)
            continue;
        ini_store(target, type, value);
        return 1;
    }
}

static void ini_apply_text(char *text, int *applied, int *unknown)
{
    char *p = text;

    while (*p) {
        char *line = p, *end;
        char *name, *value;
        int quoted = 0;

        while (*p && *p != '\n' && *p != '\r')
            p++;
        end = p;
        if (*p)
            p++;
        *end = 0;

        while (*line && isspace((unsigned char)*line))
            line++;
        if (!*line || *line == ':' || *line == ';' || *line == '#')
            continue;                     /* comment / blank */
        if (strncmp(line, "set", 3) != 0 || !isspace((unsigned char)line[3]))
            continue;                     /* only `set name value` lines */
        name = line + 3;
        while (*name && isspace((unsigned char)*name))
            name++;
        value = name;
        while (*value && !isspace((unsigned char)*value))
            value++;
        if (!*value)
            continue;
        *value++ = 0;
        while (*value && isspace((unsigned char)*value))
            value++;

        /* the ini sometimes quotes strings; our table has no string keys but
         * strip the quotes so a numeric value still parses */
        if (*value == '"' || *value == '\'') {
            quoted = *value++;
            char *q = value;
            while (*q && *q != quoted)
                q++;
            if (*q)
                *q = 0;
        }

        if (ini_apply(name, value))
            (*applied)++;
        else
            (*unknown)++;
    }
}

static char *ini_read(const char *path, size_t *len)
{
    char *host = win_host_path(path);
    FILE *f;
    char *buf;
    long n;

    if (!host)
        return NULL;
    f = fopen(host, "rb");
    free(host);
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n <= 0 || n > (1 << 20)) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

/* Called once the image is mapped and before the entry point runs. */
void game_config_load(void)
{
    static const char *names[] = {"oto.ini", "otomedius.ini", "./oto.ini",
                                  "./otomedius.ini"};
    int applied = 0, unknown = 0, found = 0;

    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]) && !found; i++) {
        size_t len = 0;
        char *text = ini_read(names[i], &len);
        if (!text)
            continue;
        found = 1;
        ini_apply_text(text, &applied, &unknown);
        free(text);
        log_printf("config: %s: %d values applied, %d unknown", names[i],
                   applied, unknown);
    }

    if (!found) {
        /* no ini: fall back to the values the cabinet shipped with */
        for (unsigned i = 0; i < sizeof(ini_defaults) / sizeof(ini_defaults[0]);
             i++) {
            char v[24];
            snprintf(v, sizeof(v), "%ld", ini_defaults[i].v);
            if (ini_apply(ini_defaults[i].name, v))
                applied++;
        }
        log_printf("config: no ini found, %d built-in defaults applied",
                   applied);
    }
}

/* The async asset loader keeps a 16-slot ring at 0x5DCF98 (0x10C bytes per
 * slot) and marks a free slot with status == -1:
 *
 *     0x419D48  cmp dword ptr [edx + 0x5DCF9C], -1
 *     0x419D4F  jne   0x419DB1            ; != -1  => this slot has work
 *
 * That ring is in uninitialised .data, so it starts as zeroes - and zero is not
 * -1, so the loader happily "processes" empty slots, the job executor fails and
 * the loader stops in state 4, which the scene manager reads as "still
 * loading".  Initialising the ring is what the cabinet's C++ constructor did.
 */
#define ASYNC_RING       0x5DCF98u
#define ASYNC_SLOT_SIZE  0x10Cu
#define ASYNC_SLOTS      16

void game_state_init(void)
{
    for (int i = 0; i < ASYNC_SLOTS; i++)
        M32(ASYNC_RING + (uint32_t)i * ASYNC_SLOT_SIZE + 4u) = 0xFFFFFFFFu;
    log_printf("config: async loader ring initialised (%d free slots)",
               ASYNC_SLOTS);
}

/* The game clears the whole configuration block itself right after startup:
 *
 *     00401ec6  push 0x148
 *     00401ecd  push 0
 *     00401ece  push 0x5db130
 *     00401ed3  call memset
 *
 * so anything written before that is lost.  Re-apply the ini right after that
 * memset (0x401ED8 is its continuation) instead of fighting the ordering: the
 * cabinet applied the file when it booted, and this is the same moment from
 * the game's point of view.  Once the game's own parser is alive this hook can
 * be dropped. */
void game_config_after_reset(uint32_t addr)
{
    static int done;
    if (done || addr != 0x401ED8u)
        return;
    done = 1;
    game_config_load();
    log_printf("config: re-applied after the game's defaults reset");
}
