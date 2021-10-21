#include <stdlib.h>

#include "cit.h"

#if defined(__clang__) || defined(__GNUC__)
#   if !defined(BIKESHED_CPU_YIELD)
#       include <emmintrin.h>
#   endif
#elif defined (_MSC_VER)
#   if !defined(BIKESHED_ATOMICADD) || !defined(BIKESHED_ATOMICCAS) || !defined(BIKESHED_CPU_YIELD)
#       if !defined(_WINDOWS_)
#           define WIN32_LEAN_AND_MEAN
#           include <Windows.h>
#           undef WIN32_LEAN_AND_MEAN
#       endif
#   endif
#endif


#if !defined(ATOMICCAS)
#   if defined(__clang__) || defined(__GNUC__)
#       define ATOMICCAS_PRIVATE(store, compare, value) __sync_val_compare_and_swap(store, compare, value)
#   elif defined(_MSC_VER)
#       define ATOMICCAS_PRIVATE(store, compare, value) _InterlockedCompareExchange((volatile LONG *)store, value, compare)
#   else
inline int32_t NonAtomicCAS(volatile int32_t* store, int32_t compare, int32_t value) { int32_t old = *store; if (old == compare) { *store = value; } return old; }
#       define ATOMICCAS_PRIVATE(store, compare, value) NonAtomicCAS(store, value, compare)
#   endif
#else
#   define ATOMICCAS_PRIVATE ATOMICCAS
#endif

#if !defined(ATOMICADD)
#   if defined(__clang__) || defined(__GNUC__)
#       define ATOMICADD_PRIVATE(value, amount) (__sync_add_and_fetch (value, amount))
#   elif defined(_MSC_VER)
#       define ATOMICADD_PRIVATE(value, amount) (_InterlockedExchangeAdd((volatile LONG *)value, amount) + amount)
#   else
inline int32_t NonAtomicAdd(volatile int32_t* store, int32_t value) { *store += value; return *store; }
#       define ATOMICADD_PRIVATE(value, amount) (NonAtomicAdd(value, amount))
#   endif
#else
#   define ATOMICADD_PRIVATE ATOMICADD
#endif

#if !defined(CPU_YIELD)
#   if defined(__clang__) || defined(__GNUC__)
#       define CPU_YIELD_PRIVATE  _mm_pause();
#   elif defined(_MSC_VER)
#       define CPU_YIELD_PRIVATE  YieldProcessor();
#   else
#       define CPU_YIELD_PRIVATE  void();
#   endif
#else
#   define CPU_YIELD_PRIVATE      CPU_YIELD
#endif

#define GENERATION_SHIFT_PRIVATE 23u
#define INDEX_MASK_PRIVATE       0x007fffffu
#define GENERATION_MASK_PRIVATE  0xff800000u
#define ID_PRIVATE(index, generation) (((uint32_t)(generation) << GENERATION_SHIFT_PRIVATE) + index)
#define ID_GENERATION_PRIVATE(task_id) ((int32_t)(task_id >> GENERATION_SHIFT_PRIVATE))
#define ID_INDEX_PRIVATE(task_id) ((uint32_t)(task_id & INDEX_MASK_PRIVATE))

static void PoolInitialize(int32_t volatile* generation, int32_t volatile* head, int32_t volatile* items, uint32_t fill_count)
{
    if (generation == 0) {
        return;
    }
    if (head == 0) {
        return;
    }
    if (items == 0) {
        return;
    }
    *generation = 0;
    if (fill_count == 0)
    {
        *head = 0;
        return;
    }
    *head = 1;
    for (uint32_t i = 0; i < fill_count - 1; ++i)
    {
        items[i] = (int32_t)(i + 2);
    }
    items[fill_count - 1] = 0;
}

// Returns 1 if this the push is to an empty pool, 0 otherwise
static int PushRange(int32_t volatile* head, int32_t volatile* generation_index, uint32_t head_index, int32_t* tail_index)
{
    if (head == 0) {
        return 0;
    }
    if (generation_index == 0) {
        return 0;
    }
    if (tail_index == 0) {
        return 0;
    }
    uint32_t gen = (((uint32_t)ATOMICADD_PRIVATE(generation_index, 1)) << GENERATION_SHIFT_PRIVATE)& GENERATION_MASK_PRIVATE;
    uint32_t new_head = gen | head_index;
    uint32_t current_head = (uint32_t)*head;
    *tail_index = (int32_t)(ID_INDEX_PRIVATE(current_head));

    while (ATOMICCAS_PRIVATE(head, (int32_t)current_head, (int32_t)new_head) != (int32_t)current_head)
    {
        CPU_YIELD_PRIVATE
        current_head = (uint32_t)*head;
        *tail_index = (int32_t)(ID_INDEX_PRIVATE(current_head));
    }
    if (ID_INDEX_PRIVATE(current_head) == 0) {
        return 1;
    }
    return 0;
}

// Returns a 1-based index for the item pool index, 0 if there are no items in the pool.
static uint32_t PopOne(int32_t volatile* head, int32_t volatile* items)
{
    if (head == 0) {
        return 0;
    }
    if (items == 0) {
        return 0;
    }
    do
    {
        uint32_t current_head = (uint32_t)*head;
        uint32_t popped_index = ID_INDEX_PRIVATE(current_head);
        if (popped_index == 0)
        {
            return 0;
        }

        uint32_t next = (uint32_t)items[popped_index - 1];
        uint32_t new_head = (current_head & GENERATION_MASK_PRIVATE) | next;

        if (ATOMICCAS_PRIVATE(head, (int32_t)current_head, (int32_t)new_head) == (int32_t)current_head)
        {
            return popped_index;
        }
        CPU_YIELD_PRIVATE
    } while (1);
}

struct CIT_private {
    WaitCallback* waitPop;
    WakeCallback* wakePop;
    WaitCallback* waitPush;
    WakeCallback* wakePush;
    int32_t volatile generation;
    int32_t volatile freeHeadIndex;
    int32_t volatile allocatedHeadIndex;
    int32_t volatile* freeIndexes;
    int32_t volatile* allocatedIndexes;
    size_t item_size;
    void* items;
};

CIT* cit_create(uint32_t channel_size, size_t item_size, WaitCallback* waitPop, WakeCallback* wakePop, WaitCallback* waitPush, WakeCallback* wakePush) {
    if (channel_size == 0) {
        return 0;
    }
    if (item_size == 0) {
        return 0;
    }
    if (waitPop == 0) {
        return 0;
    }
    if (wakePop == 0) {
        return 0;
    }
    if (waitPush == 0) {
        return 0;
    }
    if (wakePush == 0) {
        return 0;
    }
    size_t s = sizeof(CIT) + (sizeof(int32_t) * channel_size * 2) + (channel_size * item_size);
    void* m = malloc(s);
    if (m == 0) {
        return 0;
    }

    CIT* cit = (CIT*)m;

    cit->waitPop = waitPop;
    cit->wakePop = wakePop;
    cit->waitPush = waitPush;
    cit->wakePush = wakePush;
    cit->generation = 0;
    cit->freeHeadIndex = 0;
    cit->allocatedHeadIndex = 0;
    cit->freeIndexes = (int32_t volatile*)&cit[1];
    cit->allocatedIndexes = &cit->freeIndexes[channel_size];
    cit->item_size = item_size;
    cit->items = (void*)&cit->allocatedIndexes[channel_size];
    PoolInitialize(&cit->generation, &cit->freeHeadIndex, cit->freeIndexes, channel_size);
    cit->allocatedHeadIndex = 0;
    return cit;
}

void cit_push_internal(CIT* cit, uint32_t item_index, const void* item) {
    if (cit == 0) {
        return;
    }
    if (item == 0) {
        return;
    }
    void* item_address = &((char*)cit->items)[cit->item_size * item_index];
    memcpy(item_address, item, cit->item_size);
    if (1 == PushRange(
        &cit->allocatedHeadIndex,
        &cit->generation,
        item_index + 1,
        (int32_t*)&cit->allocatedIndexes[item_index])) {
        cit->wakePop->wake(cit, cit->wakePop);
    }
}


void cit_pop_internal(CIT* cit, uint32_t item_index, void* item) {
    if (cit == 0) {
        return;
    }
    if (item == 0) {
        return;
    }
    void* item_address = &((char*)cit->items)[cit->item_size * item_index];
    memcpy(item, item_address, cit->item_size);
    if (1 == PushRange(
        &cit->freeHeadIndex,
        &cit->generation,
        item_index + 1,
        (int32_t*)&cit->freeIndexes[item_index])) {
        cit->wakePush->wake(cit, cit->wakePush);
    }
}

void cit_push(CIT* cit, const void* item) {
    if (cit == 0) {
        return;
    }
    if (item == 0) {
        return;
    }
    uint32_t item_index = PopOne(&cit->freeHeadIndex, cit->freeIndexes);
    while (item_index == 0) {
        cit->waitPush->wait(cit, cit->waitPush);
        item_index = PopOne(&cit->freeHeadIndex, cit->freeIndexes);
        if (item_index != 0) {
            // If we got one after waiting, wake up next in line
            cit->wakePush->wake(cit, cit->wakePush);
        }
    }
    item_index--;
    cit_push_internal(cit, item_index, item);
}

int cit_trypush(CIT* cit, const void* item) {
    if (cit == 0) {
        return 0;
    }
    if (item == 0) {
        return 0;
    }
    uint32_t item_index = PopOne(&cit->freeHeadIndex, cit->freeIndexes);
    if (item_index == 0) {
        return 0;
    }
    item_index--;
    cit_push_internal(cit, item_index, item);
    return 1;
}

void cit_pop(CIT* cit, void* item) {
    if (cit == 0) {
        return;
    }
    if (item == 0) {
        return;
    }
    uint32_t item_index = PopOne(&cit->allocatedHeadIndex, cit->allocatedIndexes);
    while (item_index == 0) {
        cit->waitPop->wait(cit, cit->waitPop);
        item_index = PopOne(&cit->allocatedHeadIndex, cit->allocatedIndexes);
        if (item_index != 0) {
            // If we got one after waiting, wake up next in line
            cit->wakePop->wake(cit, cit->wakePop);
        }
    }
    item_index--;
    cit_pop_internal(cit, item_index, item);
}

int cit_trypop(CIT* cit, void* item) {
    if (cit == 0) {
        return 0;
    }
    if (item == 0) {
        return 0;
    }
    uint32_t item_index = PopOne(&cit->allocatedHeadIndex, cit->allocatedIndexes);
    if (item_index == 0) {
        return 0;
    }
    item_index--;
    cit_pop_internal(cit, item_index, item);
    return 1;
}

void cit_close(CIT* cit) {
    free(cit);
}
