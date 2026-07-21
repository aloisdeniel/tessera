/*
 * core.h — foundation services: logging, arena + pool allocators, slot map.
 * All internal (not part of the public ABI).
 */
#ifndef TESSERA_CORE_H
#define TESSERA_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tessera.h"

/* ---------------------------------------------------------------- logging */
typedef struct {
    TesseraLogFn fn;
    void*        userdata;
    int          min_level;
} TsLog;

void ts_log_init(TsLog* log, TesseraLogFn fn, void* userdata);
void ts_log_msg(const TsLog* log, int level, const char* fmt, ...);

#define TS_LOGT(log, ...) ts_log_msg((log), TESSERA_LOG_TRACE, __VA_ARGS__)
#define TS_LOGD(log, ...) ts_log_msg((log), TESSERA_LOG_DEBUG, __VA_ARGS__)
#define TS_LOGI(log, ...) ts_log_msg((log), TESSERA_LOG_INFO,  __VA_ARGS__)
#define TS_LOGW(log, ...) ts_log_msg((log), TESSERA_LOG_WARN,  __VA_ARGS__)
#define TS_LOGE(log, ...) ts_log_msg((log), TESSERA_LOG_ERROR, __VA_ARGS__)

/* ------------------------------------------------------------ arena alloc */
/* A growable bump allocator made of linked blocks. Freed all-at-once. */
typedef struct TsArenaBlock TsArenaBlock;
typedef struct {
    TsArenaBlock* head;
    size_t        block_size;   /* default block granularity */
    size_t        total_used;
    size_t        high_water;
} TsArena;

void  ts_arena_init(TsArena* a, size_t block_size);
void* ts_arena_alloc(TsArena* a, size_t size, size_t align);
void  ts_arena_reset(TsArena* a);   /* keep blocks, rewind */
void  ts_arena_destroy(TsArena* a); /* free all blocks     */
#define TS_ARENA_NEW(a, T)      ((T*)ts_arena_alloc((a), sizeof(T), _Alignof(T)))
#define TS_ARENA_ARR(a, T, n)   ((T*)ts_arena_alloc((a), sizeof(T) * (size_t)(n), _Alignof(T)))

/* ------------------------------------------------------------- pool alloc */
/* Fixed-size block pool with a free list. */
typedef struct {
    uint8_t* storage;
    void*    free_list;
    size_t   block_size;
    size_t   capacity;   /* number of blocks */
    size_t   in_use;
} TsPool;

bool  ts_pool_init(TsPool* p, size_t block_size, size_t capacity);
void* ts_pool_alloc(TsPool* p);      /* NULL when exhausted */
void  ts_pool_free(TsPool* p, void* ptr);
void  ts_pool_destroy(TsPool* p);

/* -------------------------------------------------------------- slot map  */
/* Generation-tagged slot map. Handle packs (index | generation<<24).
 * A stale handle (freed/reallocated slot) is rejected by ts_slotmap_get. */
typedef uint32_t TsHandle;   /* 0 == invalid */
#define TS_HANDLE_INVALID   ((TsHandle)0)
#define TS_HANDLE_INDEX(h)  ((uint32_t)((h) & 0x00FFFFFFu))
#define TS_HANDLE_GEN(h)    ((uint32_t)(((h) >> 24) & 0xFFu))

typedef struct {
    uint8_t* items;        /* dense array of `item_size` slots */
    uint8_t* generations;  /* per-slot generation (1..255)     */
    uint8_t* occupied;     /* per-slot occupancy flag          */
    uint32_t* free_stack;
    size_t   free_count;
    size_t   item_size;
    size_t   capacity;
    size_t   count;        /* live items */
} TsSlotMap;

bool     ts_slotmap_init(TsSlotMap* m, size_t item_size, size_t capacity);
void     ts_slotmap_destroy(TsSlotMap* m);
/* Allocates a slot, returns its handle; *out_ptr points at zeroed storage. */
TsHandle ts_slotmap_alloc(TsSlotMap* m, void** out_ptr);
/* Returns pointer to the item, or NULL if the handle is stale/invalid. */
void*    ts_slotmap_get(const TsSlotMap* m, TsHandle h);
void     ts_slotmap_free(TsSlotMap* m, TsHandle h);

#endif /* TESSERA_CORE_H */
