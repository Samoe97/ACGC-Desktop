#ifndef SYS_DYNAMIC_H
#define SYS_DYNAMIC_H

#include "types.h"
#include "PR/mbi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYSDYNAMIC_START_MAGIC 0x1234
#define SYSDYNAMIC_END_MAGIC 0x5678

/*
#define SHADOW_SIZE 512
#define LIGHT_SIZE 256
#define LINE_XLU_SIZE 9952
#define OVERLAY_SIZE 2048
#define LINE_OPA_SIZE 1024
#define WORK_SIZE 128
#define UNK_BUF0_SIZE 32
#define POLY_OPA_SIZE 1792
#define POLY_XLU_SIZE 512
#define FONT_SIZE 256
*/

#ifdef TARGET_PC
/* Much more headroom on PC: the wallpaper idle camera draws a full 3x3 of
 * trees/items and a screen-wide weather pool, and GRAPH_ALLOC (matrices,
 * vertices) carves its space out of the tail of poly_opa. Overflowing these
 * silently corrupts memory. */
#define POLY_OPA_SIZE 65536
#define POLY_XLU_SIZE 16384
#else
#define POLY_OPA_SIZE 9952
#define POLY_XLU_SIZE 2048
#endif
#define OVERLAY_SIZE 1024
#define WORK_SIZE 128
#define UNK_BUF0_SIZE 32
#ifdef TARGET_PC
#define FONT_SIZE 4096
#else
#define FONT_SIZE 1792
#endif
#define SHADOW_SIZE 512
#define LIGHT_SIZE 256
#define NEW0_SIZE 512
#define NEW1_SIZE 256

typedef struct dynamic_s {
    u16 start_magic;
    
    Gfx poly_opa[POLY_OPA_SIZE];
    Gfx poly_xlu[POLY_XLU_SIZE];
    Gfx overlay[OVERLAY_SIZE];
    Gfx work[WORK_SIZE];
    Gfx unused[UNK_BUF0_SIZE];
    Gfx font[FONT_SIZE];
    Gfx shadow[SHADOW_SIZE];
    Gfx light[LIGHT_SIZE];
    Gfx new0[NEW0_SIZE];
    Gfx new1[NEW1_SIZE];

    u16 end_magic;
} dynamic_t;

extern dynamic_t sys_dynamic;

#define SYSDYNAMIC_OPEN() \
do { \
  dynamic_t* __dyn = &sys_dynamic;\
while (0)

#define SYSDYNAMIC_CLOSE() \
  (void)__dyn; \
} while (0)

#define SYSDYNAMIC_CHECK_START() (__dyn->start_magic == SYSDYNAMIC_START_MAGIC)
#define SYSDYNAMIC_CHECK_END() (__dyn->end_magic == SYSDYNAMIC_END_MAGIC)

#ifdef __cplusplus
};
#endif

#endif
