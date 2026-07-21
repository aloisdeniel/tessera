/* core.c — logging, arena, pool, slot map. */
#include "core/core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- logging */
void ts_log_init(TsLog* log, TesseraLogFn fn, void* userdata) {
    log->fn = fn;
    log->userdata = userdata;
    log->min_level = TESSERA_LOG_TRACE;
}

void ts_log_msg(const TsLog* log, int level, const char* fmt, ...) {
    if (log && level < log->min_level) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (log && log->fn) {
        log->fn(log->userdata, level, buf);
    } else {
        static const char* names[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
        const char* n = (level >= 0 && level <= 4) ? names[level] : "?";
        fprintf(stderr, "[tessera %s] %s\n", n, buf);
    }
}

/* --------------------------------------------------------------- arena */
struct TsArenaBlock {
    TsArenaBlock* next;
    size_t        used;
    size_t        cap;
    /* storage follows the header */
};

static uint8_t* block_data(TsArenaBlock* b) { return (uint8_t*)(b + 1); }

void ts_arena_init(TsArena* a, size_t block_size) {
    a->head = NULL;
    a->block_size = block_size ? block_size : (64 * 1024);
    a->total_used = 0;
    a->high_water = 0;
}

static TsArenaBlock* arena_new_block(TsArena* a, size_t need) {
    size_t cap = a->block_size;
    if (need > cap) cap = need;
    TsArenaBlock* b = (TsArenaBlock*)malloc(sizeof(TsArenaBlock) + cap);
    if (!b) return NULL;
    b->next = a->head;
    b->used = 0;
    b->cap = cap;
    a->head = b;
    return b;
}

/* Absolute-address alignment: the block's data pointer is not guaranteed to be
 * more than 8-aligned (it sits past the block header), so aligning the offset
 * alone is not enough for 16-byte types (cglm vec4/mat4). Align the actual
 * pointer. */
static size_t align_offset(const TsArenaBlock* b, size_t align) {
    uintptr_t base = (uintptr_t)block_data((TsArenaBlock*)b);
    uintptr_t cur = base + b->used;
    uintptr_t aligned = (cur + (align - 1)) & ~(uintptr_t)(align - 1);
    return (size_t)(aligned - base);
}

void* ts_arena_alloc(TsArena* a, size_t size, size_t align) {
    if (align == 0) align = 1;
    TsArenaBlock* b = a->head;
    size_t off = 0;
    if (b) {
        off = align_offset(b, align);
        if (off + size > b->cap) b = NULL;
    }
    if (!b) {
        b = arena_new_block(a, size + align);
        if (!b) return NULL;
        off = align_offset(b, align);
    }
    void* p = block_data(b) + off;
    b->used = off + size;
    a->total_used += size;
    if (a->total_used > a->high_water) a->high_water = a->total_used;
    return p;
}

void ts_arena_reset(TsArena* a) {
    for (TsArenaBlock* b = a->head; b; b = b->next) b->used = 0;
    a->total_used = 0;
}

void ts_arena_destroy(TsArena* a) {
    TsArenaBlock* b = a->head;
    while (b) {
        TsArenaBlock* n = b->next;
        free(b);
        b = n;
    }
    a->head = NULL;
    a->total_used = 0;
}

/* ---------------------------------------------------------------- pool */
bool ts_pool_init(TsPool* p, size_t block_size, size_t capacity) {
    if (block_size < sizeof(void*)) block_size = sizeof(void*);
    p->storage = (uint8_t*)calloc(capacity, block_size);
    if (!p->storage) return false;
    p->block_size = block_size;
    p->capacity = capacity;
    p->in_use = 0;
    p->free_list = NULL;
    /* thread every block onto the free list */
    for (size_t i = 0; i < capacity; ++i) {
        void* slot = p->storage + i * block_size;
        *(void**)slot = p->free_list;
        p->free_list = slot;
    }
    return true;
}

void* ts_pool_alloc(TsPool* p) {
    if (!p->free_list) return NULL;
    void* slot = p->free_list;
    p->free_list = *(void**)slot;
    p->in_use++;
    memset(slot, 0, p->block_size);
    return slot;
}

void ts_pool_free(TsPool* p, void* ptr) {
    if (!ptr) return;
    *(void**)ptr = p->free_list;
    p->free_list = ptr;
    p->in_use--;
}

void ts_pool_destroy(TsPool* p) {
    free(p->storage);
    memset(p, 0, sizeof *p);
}

/* ------------------------------------------------------------ slot map */
bool ts_slotmap_init(TsSlotMap* m, size_t item_size, size_t capacity) {
    memset(m, 0, sizeof *m);
    if (capacity == 0) capacity = 16;
    m->items = (uint8_t*)calloc(capacity, item_size);
    m->generations = (uint8_t*)calloc(capacity, 1);
    m->occupied = (uint8_t*)calloc(capacity, 1);
    m->free_stack = (uint32_t*)malloc(capacity * sizeof(uint32_t));
    if (!m->items || !m->generations || !m->occupied || !m->free_stack) {
        ts_slotmap_destroy(m);
        return false;
    }
    m->item_size = item_size;
    m->capacity = capacity;
    /* generation starts at 1 so a zeroed handle (0) is always invalid */
    for (size_t i = 0; i < capacity; ++i) {
        m->generations[i] = 1;
        m->free_stack[m->free_count++] = (uint32_t)(capacity - 1 - i);
    }
    return true;
}

void ts_slotmap_destroy(TsSlotMap* m) {
    free(m->items);
    free(m->generations);
    free(m->occupied);
    free(m->free_stack);
    memset(m, 0, sizeof *m);
}

static bool slotmap_grow(TsSlotMap* m) {
    size_t nc = m->capacity * 2;
    uint8_t* it = (uint8_t*)realloc(m->items, nc * m->item_size);
    uint8_t* gn = (uint8_t*)realloc(m->generations, nc);
    uint8_t* oc = (uint8_t*)realloc(m->occupied, nc);
    uint32_t* fs = (uint32_t*)realloc(m->free_stack, nc * sizeof(uint32_t));
    if (!it || !gn || !oc || !fs) {
        /* Best-effort: keep whatever succeeded; report failure. */
        if (it) m->items = it;
        if (gn) m->generations = gn;
        if (oc) m->occupied = oc;
        if (fs) m->free_stack = fs;
        return false;
    }
    m->items = it; m->generations = gn; m->occupied = oc; m->free_stack = fs;
    memset(m->items + m->capacity * m->item_size, 0, (nc - m->capacity) * m->item_size);
    for (size_t i = m->capacity; i < nc; ++i) {
        m->generations[i] = 1;
        m->occupied[i] = 0;
        m->free_stack[m->free_count++] = (uint32_t)(nc - 1 - (i - m->capacity));
    }
    m->capacity = nc;
    return true;
}

TsHandle ts_slotmap_alloc(TsSlotMap* m, void** out_ptr) {
    if (m->free_count == 0 && !slotmap_grow(m)) {
        if (out_ptr) *out_ptr = NULL;
        return TS_HANDLE_INVALID;
    }
    uint32_t idx = m->free_stack[--m->free_count];
    m->occupied[idx] = 1;
    m->count++;
    void* p = m->items + (size_t)idx * m->item_size;
    memset(p, 0, m->item_size);
    if (out_ptr) *out_ptr = p;
    return (TsHandle)((idx & 0x00FFFFFFu) | ((uint32_t)m->generations[idx] << 24));
}

void* ts_slotmap_get(const TsSlotMap* m, TsHandle h) {
    if (h == TS_HANDLE_INVALID) return NULL;
    uint32_t idx = TS_HANDLE_INDEX(h);
    if (idx >= m->capacity) return NULL;
    if (!m->occupied[idx]) return NULL;
    if (m->generations[idx] != TS_HANDLE_GEN(h)) return NULL;
    return m->items + (size_t)idx * m->item_size;
}

void ts_slotmap_free(TsSlotMap* m, TsHandle h) {
    uint32_t idx = TS_HANDLE_INDEX(h);
    if (idx >= m->capacity || !m->occupied[idx]) return;
    if (m->generations[idx] != TS_HANDLE_GEN(h)) return;
    m->occupied[idx] = 0;
    m->generations[idx]++;
    if (m->generations[idx] == 0) m->generations[idx] = 1;  /* skip 0 */
    m->free_stack[m->free_count++] = idx;
    m->count--;
}
