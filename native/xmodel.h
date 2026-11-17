/* xmodel.h - minimal parser for the binary .x model inside the paks.
 *
 * The game hands the raw .x image to D3DXLoadMeshHierarchyFromXInMemory; the
 * stub used to answer with a synthetic hierarchy.  This parser reads the real
 * frame tree, meshes, materials, normals and texture coordinates so the
 * native renderer can draw the player model instead of a placeholder.
 *
 * Only the frame tree is parsed: the animation sets that follow it are not
 * needed yet (the effect/animation path is still stubbed).
 */
#ifndef XMODEL_H
#define XMODEL_H

#include <stdint.h>

/* Parse a `xof 03xxbin` image.  Returns 0 on success, -1 on a malformed file.
 * A previous parse is discarded. */
int xmodel_parse(const uint8_t *data, uint32_t size);

int         xmodel_frame_count(void);
const char *xmodel_frame_name(int i);
int         xmodel_frame_parent(int i);       /* -1 for the root frame */
const float *xmodel_frame_matrix(int i);      /* 16 floats, row-vector D3D */
int         xmodel_frame_mesh(int i);         /* mesh index, or -1 */

int         xmodel_mesh_count(void);
const char *xmodel_mesh_name(int m);
int         xmodel_mesh_nverts(int m);
int         xmodel_mesh_nfaces(int m);
int         xmodel_mesh_nmats(int m);
const float    *xmodel_mesh_pos(int m);       /* 3 floats per vertex */
const float    *xmodel_mesh_norm(int m);      /* 3 floats per vertex, may be NULL */
const float    *xmodel_mesh_uv(int m);        /* 2 floats per vertex, may be NULL */
const uint32_t *xmodel_mesh_idx(int m);       /* 3 indices per face */
const uint32_t *xmodel_mesh_facemat(int m);   /* material index per face */
const float    *xmodel_mesh_matcolor(int m, int mat);  /* RGBA */

/* One-line summary per frame and mesh (OTOMEDIUS_XMODEL_DBG=1). */
void xmodel_dump(void);

#endif /* XMODEL_H */
