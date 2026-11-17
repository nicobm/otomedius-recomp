/* xmodel.c - minimal parser for the binary .x models shipped inside the paks.
 *
 * Format notes (xof 03xxbin), all little-endian:
 *   - 16-byte magic, then the template list, then the data objects.
 *   - A template is  TOKEN_TEMPLATE NAME OBRACE GUID <members> CBRACE.
 *     A member is  TYPE NAME [OBRACKET dims CBRACKET] SEMI, where TYPE is a
 *     scalar token, TOKEN_ARRAY + scalar token, or TOKEN_NAME <template>.
 *     The member list may end with `[...]` (more objects follow the declared
 *     members) or with `[<template> <guid>]` (an inline list of objects).
 *   - A data object is  NAME <template> [NAME <instance>] OBRACE <values> CBRACE.
 *     Values of scalar members may be packed into TOKEN_INTEGER_LIST /
 *     TOKEN_FLOAT_LIST runs that span several members, so they are consumed
 *     through a small cursor instead of per-member reads.
 *   - "Inline" templates (Vector, Coords2d, Matrix4x4, ColorRGBA, MeshFace,
 *     ...) are flattened into the parent object's value stream; complex ones
 *     (Frame, Mesh, ...) appear as nested objects in the `[...]` part.
 *
 * Only the frame tree is parsed: the animation sets that follow it are not
 * needed while the animation controller is still a stub.
 */
#include "xmodel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XM_MAX_TPL     32
#define XM_MAX_MEMBERS 8
#define XM_MAX_DIMS    2
#define XM_MAX_FRAMES  96
#define XM_MAX_MESHES  8
#define XM_MAX_VERTS   8192
#define XM_MAX_FACES   8192
#define XM_MAX_MATS    8
#define XM_NAME        28

enum {
    T_NAME = 1, T_STRING = 2, T_INTEGER = 3, T_GUID = 5, T_INTLIST = 6,
    T_FLOATLIST = 7, T_OBRACE = 10, T_CBRACE = 11, T_OPAREN = 12,
    T_CPAREN = 13, T_OBRACKET = 14, T_CBRACKET = 15, T_OANGLE = 16,
    T_CANGLE = 17, T_DOT = 18, T_COMMA = 19, T_SEMI = 20, T_TEMPLATE = 31,
    T_WORD = 40, T_DWORD = 41, T_FLOAT = 42, T_DOUBLE = 43, T_CHAR = 44,
    T_UCHAR = 45, T_SWORD = 46, T_SDWORD = 47, T_VOID = 48, T_LPSTR = 49,
    T_UNICODE = 50, T_CSTRING = 51, T_ARRAY = 52
};

typedef struct {
    char name[XM_NAME];
    int  type;                  /* scalar token, or -1 for a template */
    char type_name[XM_NAME];    /* template name when type == -1 */
    int  ndims;
    int  dim_kind[XM_MAX_DIMS]; /* 0 = fixed, 1 = reference to a member */
    int  dim_num[XM_MAX_DIMS];
    char dim_ref[XM_MAX_DIMS][XM_NAME];
} xm_member;

typedef struct {
    char      name[XM_NAME];
    int       nmembers;
    xm_member m[XM_MAX_MEMBERS];
    int       has_extra;
} xm_tpl;

typedef struct {
    char  name[XM_NAME];
    int   parent;
    float matrix[16];
    int   mesh;
} xm_frame;

typedef struct {
    char  name[XM_NAME];
    int   first_vert, nverts;
    int   first_face, nfaces;
    int   nmats;
    float mat[XM_MAX_MATS * 4];
} xm_mesh;

static const uint8_t *g_data;
static uint32_t g_size, g_pos;
static int g_err;
static int g_verbose;

static xm_tpl  g_tpl[XM_MAX_TPL];
static int     g_ntpl;

/* Pending runs of scalars shared by consecutive members. */
static const uint8_t *g_ilist;
static int g_irem;
static const uint8_t *g_flist;
static int g_frem;

static xm_frame g_frames[XM_MAX_FRAMES];
static int      g_nframes;
static xm_mesh  g_meshes[XM_MAX_MESHES];
static int      g_nmeshes;

static float    g_verts[XM_MAX_VERTS * 3];
static float    g_norms[XM_MAX_VERTS * 3];
static float    g_uvs[XM_MAX_VERTS * 2];
static uint32_t g_idx[XM_MAX_FACES * 3];
static uint32_t g_facemat[XM_MAX_FACES];
static int      g_nverts, g_nfaces;

/* ---- primitive readers -------------------------------------------------- */

static void fail(const char *why)
{
    if (!g_err)
        fprintf(stderr, "xmodel: %s at 0x%x\n", why, g_pos);
    g_err = 1;
}

static int need(uint32_t n)
{
    if (g_pos + n > g_size || g_pos + n < g_pos) {
        fail("truncated");
        return 0;
    }
    return 1;
}

static uint16_t rd_u16(void)
{
    uint16_t v;
    if (!need(2))
        return 0;
    v = (uint16_t)(g_data[g_pos] | (g_data[g_pos + 1] << 8));
    g_pos += 2;
    return v;
}

static uint32_t rd_u32(void)
{
    uint32_t v;
    if (!need(4))
        return 0;
    v = (uint32_t)g_data[g_pos] | ((uint32_t)g_data[g_pos + 1] << 8) |
        ((uint32_t)g_data[g_pos + 2] << 16) | ((uint32_t)g_data[g_pos + 3] << 24);
    g_pos += 4;
    return v;
}

static float rd_f32(void)
{
    uint32_t v = rd_u32();
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static uint16_t peek_tok(void)
{
    if (!need(2))
        return 0;
    return (uint16_t)(g_data[g_pos] | (g_data[g_pos + 1] << 8));
}

static void rd_name(char *out, int cap)
{
    uint32_t n = rd_u32();
    int i = 0;
    if (n > 0x1000u) {
        fail("bad string length");
        out[0] = 0;
        return;
    }
    if (!need(n))
        return;
    for (; i < (int)n && i < cap - 1; i++)
        out[i] = (char)g_data[g_pos + i];
    out[i] = 0;
    g_pos += n;
}

static void expect(uint16_t tok)
{
    uint16_t t = rd_u16();
    if (t != tok)
        fail("unexpected token");
}

/* ---- scalar value streams ----------------------------------------------- */

static int vs_int(void)
{
    if (g_irem > 0) {
        int v = (int)((uint32_t)g_ilist[0] | ((uint32_t)g_ilist[1] << 8) |
                      ((uint32_t)g_ilist[2] << 16) | ((uint32_t)g_ilist[3] << 24));
        g_ilist += 4;
        g_irem--;
        return v;
    }
    switch (peek_tok()) {
    case T_INTLIST: {
        uint32_t n;
        rd_u16();
        n = rd_u32();
        if (!need(n * 4u)) {
            fail("bad integer list");
            return 0;
        }
        g_ilist = g_data + g_pos;
        g_pos += n * 4u;
        g_irem = (int)n;
        return vs_int();
    }
    case T_INTEGER:
    case T_DWORD:
    case T_SDWORD:
        rd_u16();
        return (int)rd_u32();
    case T_WORD:
    case T_SWORD:
        rd_u16();
        return (int)rd_u16();
    case T_CHAR:
    case T_UCHAR:
        rd_u16();
        return need(1) ? g_data[g_pos++] : 0;
    default:
        fail("expected an integer value");
        return 0;
    }
}

static float vs_float(void)
{
    if (g_frem > 0) {
        float f;
        uint32_t v = (uint32_t)g_flist[0] | ((uint32_t)g_flist[1] << 8) |
                     ((uint32_t)g_flist[2] << 16) | ((uint32_t)g_flist[3] << 24);
        memcpy(&f, &v, 4);
        g_flist += 4;
        g_frem--;
        return f;
    }
    switch (peek_tok()) {
    case T_FLOATLIST: {
        uint32_t n;
        rd_u16();
        n = rd_u32();
        if (!need(n * 4u)) {
            fail("bad float list");
            return 0;
        }
        g_flist = g_data + g_pos;
        g_pos += n * 4u;
        g_frem = (int)n;
        return vs_float();
    }
    case T_FLOAT:
        rd_u16();
        return rd_f32();
    case T_DOUBLE: {
        uint64_t bits = 0;
        double d;
        int i;
        rd_u16();
        if (!need(8))
            return 0;
        for (i = 7; i >= 0; i--)
            bits = (bits << 8) | g_data[g_pos + i];
        g_pos += 8;
        memcpy(&d, &bits, 8);
        return (float)d;
    }
    default:
        fail("expected a float value");
        return 0;
    }
}

static void vs_string(void)
{
    uint16_t t = peek_tok();
    char tmp[XM_NAME];
    if (t != T_STRING && t != T_LPSTR && t != T_CSTRING) {
        fail("expected a string value");
        return;
    }
    rd_u16();
    rd_name(tmp, sizeof tmp);
}

/* Integer payload for a token that has already been consumed. */
static int rd_scalar_int(int tok)
{
    switch (tok) {
    case T_INTEGER:
    case T_DWORD:
    case T_SDWORD:
        return (int)rd_u32();
    case T_WORD:
    case T_SWORD:
        return (int)rd_u16();
    case T_CHAR:
    case T_UCHAR:
        return need(1) ? g_data[g_pos++] : 0;
    default:
        fail("expected an integer dimension");
        return 0;
    }
}

/* ---- templates ---------------------------------------------------------- */

static int find_tpl(const char *name)
{
    int i;
    for (i = 0; i < g_ntpl; i++)
        if (!strcmp(g_tpl[i].name, name))
            return i;
    return -1;
}

/* Templates whose members are flattened into the parent value stream. */
static int is_inline(int tpl)
{
    static const char *names[] = {
        "Vector", "Vector4", "Quaternion", "Matrix4x4", "Coords2d",
        "ColorRGBA", "ColorRGB", "MeshFace", "FloatKeys", "TimedFloatKeys",
        NULL
    };
    int i;
    if (tpl < 0)
        return 0;
    for (i = 0; names[i]; i++)
        if (!strcmp(g_tpl[tpl].name, names[i]))
            return 1;
    return 0;
}

static void parse_templates(void)
{
    while (peek_tok() == T_TEMPLATE) {
        xm_tpl *tp;
        char tname[XM_NAME];
        rd_u16();
        expect(T_NAME);
        rd_name(tname, sizeof tname);
        expect(T_OBRACE);
        expect(T_GUID);
        if (!need(16))
            return;
        g_pos += 16;
        if (g_ntpl >= XM_MAX_TPL) {
            fail("too many templates");
            return;
        }
        tp = &g_tpl[g_ntpl];
        memset(tp, 0, sizeof *tp);
        strncpy(tp->name, tname, XM_NAME - 1);
        while (!g_err && peek_tok() != T_CBRACE) {
            xm_member *mb;
            int t;
            if (peek_tok() == T_OBRACKET) {
                rd_u16();
                if (peek_tok() == T_DOT) {
                    while (!g_err && peek_tok() != T_CBRACKET)
                        rd_u16();
                    expect(T_CBRACKET);
                    tp->has_extra = 1;
                    break;
                }
                /* Inline template list: [<template> <guid>].  The objects are
                 * written as nested objects right before the closing brace. */
                expect(T_NAME);
                {
                    char tmp[XM_NAME];
                    rd_name(tmp, sizeof tmp);
                }
                expect(T_GUID);
                if (!need(16))
                    return;
                g_pos += 16;
                expect(T_CBRACKET);
                tp->has_extra = 1;
                break;
            }
            if (tp->nmembers >= XM_MAX_MEMBERS) {
                fail("too many template members");
                return;
            }
            mb = &tp->m[tp->nmembers];
            memset(mb, 0, sizeof *mb);
            t = rd_u16();
            if (t == T_ARRAY) {
                int mt = rd_u16();
                if (mt == T_NAME) {
                    mb->type = -1;
                    rd_name(mb->type_name, sizeof mb->type_name);
                } else {
                    mb->type = mt;
                }
                expect(T_NAME);
                rd_name(mb->name, sizeof mb->name);
            } else if (t == T_NAME) {
                mb->type = -1;
                rd_name(mb->type_name, sizeof mb->type_name);
                expect(T_NAME);
                rd_name(mb->name, sizeof mb->name);
            } else {
                mb->type = t;
                expect(T_NAME);
                rd_name(mb->name, sizeof mb->name);
            }
            if (peek_tok() == T_OBRACKET) {
                rd_u16();
                while (!g_err && peek_tok() != T_CBRACKET) {
                    int dt = rd_u16();
                    if (mb->ndims >= XM_MAX_DIMS) {
                        fail("too many dimensions");
                        return;
                    }
                    if (dt == T_NAME) {
                        mb->dim_kind[mb->ndims] = 1;
                        rd_name(mb->dim_ref[mb->ndims], XM_NAME);
                    } else {
                        mb->dim_kind[mb->ndims] = 0;
                        mb->dim_num[mb->ndims] = rd_scalar_int(dt);
                    }
                    mb->ndims++;
                }
                expect(T_CBRACKET);
            }
            if (peek_tok() == T_SEMI)
                rd_u16();
            tp->nmembers++;
        }
        expect(T_CBRACE);
        g_ntpl++;
    }
}

/* ---- member dictionaries (dimension references) ------------------------- */

typedef struct {
    char name[XM_NAME];
    int  value;
} xm_val;

typedef struct {
    xm_val v[XM_MAX_MEMBERS];
    int    n;
} xm_dict;

static int dict_get(const xm_dict *d, const char *name)
{
    int i;
    for (i = 0; i < d->n; i++)
        if (!strcmp(d->v[i].name, name))
            return d->v[i].value;
    return 0;
}

static void dict_put(xm_dict *d, const char *name, int value)
{
    if (d->n < XM_MAX_MEMBERS) {
        strncpy(d->v[d->n].name, name, XM_NAME - 1);
        d->v[d->n].name[XM_NAME - 1] = 0;
        d->v[d->n].value = value;
        d->n++;
    }
}

static int member_count(const xm_member *mb, const xm_dict *d)
{
    int n = 1, i;
    for (i = 0; i < mb->ndims; i++)
        n *= mb->dim_kind[i] ? dict_get(d, mb->dim_ref[i]) : mb->dim_num[i];
    return n < 0 ? 0 : n;
}

/* ---- data objects ------------------------------------------------------- */

static void read_object(void);

/* Consume the values of an inline template (flattened into the parent). */
static void skip_inline(int tpl, xm_dict *parent)
{
    const xm_tpl *tp = &g_tpl[tpl];
    xm_dict d;
    int i;
    (void)parent;
    d.n = 0;
    for (i = 0; i < tp->nmembers && !g_err; i++) {
        const xm_member *mb = &tp->m[i];
        int n = member_count(mb, &d);
        if (mb->type == -1) {
            int sub = find_tpl(mb->type_name);
            int k;
            for (k = 0; k < n && !g_err; k++)
                skip_inline(sub, &d);
        } else if (mb->type == T_FLOAT || mb->type == T_DOUBLE) {
            int k;
            for (k = 0; k < n && !g_err; k++)
                vs_float();
        } else if (mb->type == T_LPSTR || mb->type == T_STRING ||
                   mb->type == T_CSTRING) {
            int k;
            for (k = 0; k < n && !g_err; k++)
                vs_string();
        } else {
            int first = 0, k;
            for (k = 0; k < n && !g_err; k++) {
                int v = vs_int();
                if (k == 0)
                    first = v;
            }
            if (n == 1)
                dict_put(&d, mb->name, first);
        }
    }
}

/* Consume the body of an object we do not keep. */
static void skip_object_body(const char *tname)
{
    int tpl = find_tpl(tname);
    xm_dict d;
    int i;
    d.n = 0;
    if (tpl < 0) {
        fail("unknown template");
        return;
    }
    for (i = 0; i < g_tpl[tpl].nmembers && !g_err; i++) {
        const xm_member *mb = &g_tpl[tpl].m[i];
        int n = member_count(mb, &d);
        if (mb->type == -1) {
            int sub = find_tpl(mb->type_name);
            int k;
            for (k = 0; k < n && !g_err; k++) {
                if (is_inline(sub))
                    skip_inline(sub, &d);
                else
                    read_object();
            }
        } else if (mb->type == T_FLOAT || mb->type == T_DOUBLE) {
            int k;
            for (k = 0; k < n && !g_err; k++)
                vs_float();
        } else if (mb->type == T_LPSTR || mb->type == T_STRING ||
                   mb->type == T_CSTRING) {
            int k;
            for (k = 0; k < n && !g_err; k++)
                vs_string();
        } else {
            int first = 0, k;
            for (k = 0; k < n && !g_err; k++) {
                int v = vs_int();
                if (k == 0)
                    first = v;
            }
            if (n == 1)
                dict_put(&d, mb->name, first);
        }
        if (peek_tok() == T_SEMI)
            rd_u16();
    }
    if (g_tpl[tpl].has_extra) {
        while (!g_err && peek_tok() != T_CBRACE)
            read_object();
    }
    expect(T_CBRACE);
}

static int new_frame(int parent, const char *name)
{
    xm_frame *f;
    if (g_nframes >= XM_MAX_FRAMES) {
        fail("too many frames");
        return 0;
    }
    f = &g_frames[g_nframes];
    memset(f, 0, sizeof *f);
    f->parent = parent;
    f->mesh = -1;
    f->matrix[0] = f->matrix[5] = f->matrix[10] = f->matrix[15] = 1.0f;
    if (name)
        strncpy(f->name, name, XM_NAME - 1);
    return g_nframes++;
}

static int new_mesh(const char *name)
{
    xm_mesh *m;
    if (g_nmeshes >= XM_MAX_MESHES) {
        fail("too many meshes");
        return -1;
    }
    m = &g_meshes[g_nmeshes];
    memset(m, 0, sizeof *m);
    m->first_vert = g_nverts;
    m->first_face = g_nfaces;
    if (name)
        strncpy(m->name, name, XM_NAME - 1);
    return g_nmeshes++;
}

static void read_ftm(int frame)
{
    int i;
    for (i = 0; i < 16; i++)
        g_frames[frame].matrix[i] = vs_float();
    if (g_tpl[find_tpl("FrameTransformMatrix")].has_extra)
        while (!g_err && peek_tok() != T_CBRACE)
            read_object();
    expect(T_CBRACE);
}

static void read_normals(int mesh)
{
    xm_mesh *m = &g_meshes[mesh];
    int n = vs_int(), i, k, nf;
    if (n != m->nverts && g_verbose)
        fprintf(stderr, "xmodel: normals %d != verts %d\n", n, m->nverts);
    for (i = 0; i < n * 3 && !g_err; i++)
        g_norms[m->first_vert * 3 + i] = vs_float();
    /* Face normals: MeshFace list, consumed but unused. */
    nf = vs_int();
    for (i = 0; i < nf && !g_err; i++) {
        int k3 = vs_int();
        for (k = 0; k < k3 && !g_err; k++)
            (void)vs_int();
    }
    while (!g_err && peek_tok() != T_CBRACE)
        read_object();
    expect(T_CBRACE);
}

static void read_uvs(int mesh)
{
    xm_mesh *m = &g_meshes[mesh];
    int n = vs_int(), i;
    if (n != m->nverts && g_verbose)
        fprintf(stderr, "xmodel: uvs %d != verts %d\n", n, m->nverts);
    for (i = 0; i < n * 2 && !g_err; i++)
        g_uvs[m->first_vert * 2 + i] = vs_float();
    while (!g_err && peek_tok() != T_CBRACE)
        read_object();
    expect(T_CBRACE);
}

static void read_material(int mesh, int slot)
{
    xm_mesh *m = &g_meshes[mesh];
    int i;
    if (slot >= XM_MAX_MATS) {
        skip_object_body("Material");
        return;
    }
    for (i = 0; i < 4; i++)
        m->mat[slot * 4 + i] = vs_float();
    (void)vs_float();                    /* power */
    for (i = 0; i < 3; i++)
        (void)vs_float();                /* specular */
    for (i = 0; i < 3; i++)
        (void)vs_float();                /* emissive */
    while (!g_err && peek_tok() != T_CBRACE)
        read_object();                   /* TextureFilename and friends */
    expect(T_CBRACE);
}

static void read_matlist(int mesh)
{
    xm_mesh *m = &g_meshes[mesh];
    int nm = vs_int(), nf = vs_int(), i, slot = 0;
    m->nmats = nm;
    for (i = 0; i < nf && !g_err; i++) {
        int fi = vs_int();
        if (m->first_face + i < g_nfaces && fi >= 0)
            g_facemat[m->first_face + i] = (uint32_t)fi;
    }
    while (!g_err && peek_tok() != T_CBRACE) {
        char tname[XM_NAME], iname[XM_NAME];
        expect(T_NAME);
        rd_name(tname, sizeof tname);
        iname[0] = 0;
        if (peek_tok() == T_NAME) {
            rd_u16();
            rd_name(iname, sizeof iname);
        }
        expect(T_OBRACE);
        if (!strcmp(tname, "Material"))
            read_material(mesh, slot++);
        else
            skip_object_body(tname);
    }
    expect(T_CBRACE);
}

static void read_mesh(int frame, const char *name)
{
    int mesh = new_mesh(name && name[0] ? name : xmodel_frame_name(frame));
    xm_mesh *m;
    int nv, nf, i, k;
    if (mesh < 0)
        return;
    m = &g_meshes[mesh];
    g_frames[frame].mesh = mesh;
    nv = vs_int();
    if (nv < 0 || g_nverts + nv > XM_MAX_VERTS) {
        fail("too many vertices");
        return;
    }
    m->nverts = nv;
    g_nverts += nv;
    for (i = 0; i < nv * 3 && !g_err; i++)
        g_verts[m->first_vert * 3 + i] = vs_float();

    nf = vs_int();
    if (nf < 0 || g_nfaces + nf > XM_MAX_FACES) {
        fail("too many faces");
        return;
    }
    m->nfaces = nf;
    g_nfaces += nf;
    for (i = 0; i < nf && !g_err; i++) {
        int k3 = vs_int();
        for (k = 0; k < k3 && !g_err; k++) {
            int idx = vs_int();
            if (k < 3)
                g_idx[(m->first_face + i) * 3 + k] = (uint32_t)idx;
        }
        if (k3 != 3)
            fail("non-triangle face");
    }
    /* Nested objects: normals, texture coordinates, materials, skinning. */
    while (!g_err && peek_tok() != T_CBRACE) {
        char tname[XM_NAME], iname[XM_NAME];
        expect(T_NAME);
        rd_name(tname, sizeof tname);
        iname[0] = 0;
        if (peek_tok() == T_NAME) {
            rd_u16();
            rd_name(iname, sizeof iname);
        }
        expect(T_OBRACE);
        if (!strcmp(tname, "MeshNormals"))
            read_normals(mesh);
        else if (!strcmp(tname, "MeshTextureCoords"))
            read_uvs(mesh);
        else if (!strcmp(tname, "MeshMaterialList"))
            read_matlist(mesh);
        else
            skip_object_body(tname);
    }
    expect(T_CBRACE);
}

static void read_frame(int parent, const char *name)
{
    int frame = new_frame(parent, name);
    if (g_err)
        return;
    /* The Frame template declares no members: everything is nested objects. */
    while (!g_err && peek_tok() != T_CBRACE) {
        char tname[XM_NAME], iname[XM_NAME];
        expect(T_NAME);
        rd_name(tname, sizeof tname);
        iname[0] = 0;
        if (peek_tok() == T_NAME) {
            rd_u16();
            rd_name(iname, sizeof iname);
        }
        expect(T_OBRACE);
        if (!strcmp(tname, "Frame")) {
            read_frame(frame, iname);
        } else if (!strcmp(tname, "FrameTransformMatrix")) {
            read_ftm(frame);
        } else if (!strcmp(tname, "Mesh")) {
            read_mesh(frame, iname);
        } else {
            skip_object_body(tname);
        }
    }
    expect(T_CBRACE);
}

/* Read NAME <template> [NAME <instance>] OBRACE and dispatch. */
static void read_object(void)
{
    char tname[XM_NAME], iname[XM_NAME];
    expect(T_NAME);
    rd_name(tname, sizeof tname);
    iname[0] = 0;
    if (peek_tok() == T_NAME) {
        rd_u16();
        rd_name(iname, sizeof iname);
    }
    expect(T_OBRACE);
    if (g_err)
        return;
    if (!strcmp(tname, "Frame"))
        read_frame(-1, iname);
    else
        skip_object_body(tname);
}

/* ---- public API --------------------------------------------------------- */

int xmodel_parse(const uint8_t *data, uint32_t size)
{
    g_data = data;
    g_size = size;
    g_pos = 0;
    g_err = 0;
    g_ntpl = g_nframes = g_nmeshes = 0;
    g_nverts = g_nfaces = 0;
    g_ilist = g_flist = NULL;
    g_irem = g_frem = 0;
    g_verbose = getenv("OTOMEDIUS_XMODEL_DBG") != NULL;
    memset(g_frames, 0, sizeof g_frames);
    memset(g_meshes, 0, sizeof g_meshes);

    if (size < 16 || memcmp(data, "xof ", 4)) {
        fprintf(stderr, "xmodel: not a binary .x image\n");
        return -1;
    }
    if (data[8] != 'b' || data[9] != 'i' || data[10] != 'n') {
        fprintf(stderr, "xmodel: only binary .x is supported (\"%.16s\")\n",
                (const char *)data);
        return -1;
    }
    g_pos = 16;
    parse_templates();
    if (g_err)
        return -1;
    if (g_verbose)
        fprintf(stderr, "xmodel: %d templates\n", g_ntpl);

    /* Root objects: the frame tree comes first; stop after it. */
    while (!g_err && g_pos < g_size) {
        int before = g_nframes;
        read_object();
        if (g_err)
            break;
        if (g_nframes > before)
            break;
    }
    if (g_err)
        return -1;
    if (g_verbose)
        fprintf(stderr, "xmodel: %d frames, %d meshes, %d verts, %d faces\n",
                g_nframes, g_nmeshes, g_nverts, g_nfaces);
    return g_nframes ? 0 : -1;
}

int xmodel_frame_count(void) { return g_nframes; }
const char *xmodel_frame_name(int i)
{
    return (i >= 0 && i < g_nframes) ? g_frames[i].name : "";
}
int xmodel_frame_parent(int i)
{
    return (i >= 0 && i < g_nframes) ? g_frames[i].parent : -1;
}
const float *xmodel_frame_matrix(int i)
{
    return (i >= 0 && i < g_nframes) ? g_frames[i].matrix : NULL;
}
int xmodel_frame_mesh(int i)
{
    return (i >= 0 && i < g_nframes) ? g_frames[i].mesh : -1;
}

int xmodel_mesh_count(void) { return g_nmeshes; }
const char *xmodel_mesh_name(int m)
{
    return (m >= 0 && m < g_nmeshes) ? g_meshes[m].name : "";
}
int xmodel_mesh_nverts(int m)
{
    return (m >= 0 && m < g_nmeshes) ? g_meshes[m].nverts : 0;
}
int xmodel_mesh_nfaces(int m)
{
    return (m >= 0 && m < g_nmeshes) ? g_meshes[m].nfaces : 0;
}
int xmodel_mesh_nmats(int m)
{
    return (m >= 0 && m < g_nmeshes) ? g_meshes[m].nmats : 0;
}
const float *xmodel_mesh_pos(int m)
{
    return (m >= 0 && m < g_nmeshes) ? &g_verts[g_meshes[m].first_vert * 3] : NULL;
}
const float *xmodel_mesh_norm(int m)
{
    return (m >= 0 && m < g_nmeshes) ? &g_norms[g_meshes[m].first_vert * 3] : NULL;
}
const float *xmodel_mesh_uv(int m)
{
    return (m >= 0 && m < g_nmeshes) ? &g_uvs[g_meshes[m].first_vert * 2] : NULL;
}
const uint32_t *xmodel_mesh_idx(int m)
{
    return (m >= 0 && m < g_nmeshes) ? &g_idx[g_meshes[m].first_face * 3] : NULL;
}
const uint32_t *xmodel_mesh_facemat(int m)
{
    return (m >= 0 && m < g_nmeshes) ? &g_facemat[g_meshes[m].first_face] : NULL;
}
const float *xmodel_mesh_matcolor(int m, int mat)
{
    if (m < 0 || m >= g_nmeshes || mat < 0 || mat >= XM_MAX_MATS)
        return NULL;
    return &g_meshes[m].mat[mat * 4];
}

void xmodel_dump(void)
{
    int i;
    for (i = 0; i < g_nframes; i++)
        fprintf(stderr, "xmodel: frame %2d %-24s parent=%2d mesh=%d\n", i,
                g_frames[i].name, g_frames[i].parent, g_frames[i].mesh);
    for (i = 0; i < g_nmeshes; i++)
        fprintf(stderr, "xmodel: mesh %d %-24s verts=%d faces=%d mats=%d\n", i,
                g_meshes[i].name, g_meshes[i].nverts, g_meshes[i].nfaces,
                g_meshes[i].nmats);
}
