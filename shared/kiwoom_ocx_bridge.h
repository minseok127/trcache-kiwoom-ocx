/*
 * kiwoom_ocx_bridge.h - Header-only SPSC queue over Windows shared memory.
 *
 * Shared between 32-bit Kiwoom OCX producer and 64-bit trcache consumer.
 * The queue is entry-type-agnostic: entry size is set at creation time
 * and both sides must agree on the same layout.
 *
 * All types use fixed-width integers for 32/64-bit compatibility.
 */

#ifndef KIWOOM_OCX_BRIDGE_H
#define KIWOOM_OCX_BRIDGE_H

#ifndef _WIN32
#error "kiwoom_ocx_bridge.h requires Windows"
#endif

#include <stdint.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/*
 * Compiler barrier: prevents the compiler from reordering memory accesses
 * across this point. On x86, this is sufficient for SPSC lock-free queues
 * because the hardware already guarantees store-store and load-load ordering.
 */
#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(_ReadWriteBarrier)
#define KOB_COMPILER_BARRIER() _ReadWriteBarrier()
#else
#define KOB_COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define KOB_CACHE_LINE_SIZE  64
#define KOB_DEFAULT_CAPACITY (1 << 16)  /* 65536, must be power of 2 */
#define KOB_SHM_NAME         "kiwoom_ocx_bridge_shm"

/*
 * Entry layout protocol:
 * The first KOB_CODE_SIZE bytes of every entry must be the
 * null-terminated stock code (e.g. "005930\0\0"). The consumer
 * uses this to route entries to per-symbol streams; it does not
 * interpret the rest of the entry.
 */
#define KOB_CODE_SIZE 8

/*
 * SPSC ring-buffer queue laid out in shared memory.
 *
 * head:       written only by producer, read by consumer.
 * tail:       written only by consumer, read by producer.
 * error:      set by producer on queue-full; once set, push is a no-op.
 * entry_size: byte size of each entry, set once at creation.
 * capacity:   number of entries, must be a power of 2.
 *
 * The data region starts at the 'data' flexible array member,
 * containing capacity entries of entry_size bytes each.
 */
struct kob_queue {
	volatile uint32_t head;
	uint8_t _pad_head[KOB_CACHE_LINE_SIZE - sizeof(uint32_t)];

	volatile uint32_t tail;
	uint8_t _pad_tail[KOB_CACHE_LINE_SIZE - sizeof(uint32_t)];

	uint32_t          capacity;
	uint32_t          entry_size;
	volatile uint32_t error;
	uint8_t _pad_meta[KOB_CACHE_LINE_SIZE - 3 * sizeof(uint32_t)];

	uint8_t data[];  /* capacity * entry_size bytes follow */
};

/* Handle returned by kob_create / kob_open. */
struct kob_handle {
	struct kob_queue *queue;
	HANDLE h_map_file;
};

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static inline size_t kob_shm_size(uint32_t capacity,
	uint32_t entry_size)
{
	return sizeof(struct kob_queue)
		+ (size_t)entry_size * capacity;
}

/* ------------------------------------------------------------------ */
/*  Shared-memory lifecycle                                           */
/* ------------------------------------------------------------------ */

/*
 * Create a new shared memory region and initialise the queue.
 * Typically called by the producer before any push.
 * capacity must be a power of 2.
 */
static inline int kob_create(struct kob_handle *h, uint32_t capacity,
	uint32_t entry_size)
{
	size_t size = kob_shm_size(capacity, entry_size);

	h->h_map_file = CreateFileMappingA(
		INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
		(DWORD)(size >> 32), (DWORD)(size & 0xFFFFFFFF),
		KOB_SHM_NAME);
	if (h->h_map_file == NULL)
		return -1;

	h->queue = (struct kob_queue *)MapViewOfFile(
		h->h_map_file, FILE_MAP_ALL_ACCESS, 0, 0, size);
	if (h->queue == NULL) {
		CloseHandle(h->h_map_file);
		return -1;
	}

	memset(h->queue, 0, sizeof(struct kob_queue));
	h->queue->capacity   = capacity;
	h->queue->entry_size = entry_size;
	return 0;
}

/*
 * Open an existing shared memory region.
 * Typically called by the consumer after the producer has created it.
 */
static inline int kob_open(struct kob_handle *h)
{
	h->h_map_file = OpenFileMappingA(
		FILE_MAP_ALL_ACCESS, FALSE, KOB_SHM_NAME);
	if (h->h_map_file == NULL)
		return -1;

	h->queue = (struct kob_queue *)MapViewOfFile(
		h->h_map_file, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if (h->queue == NULL) {
		CloseHandle(h->h_map_file);
		return -1;
	}

	return 0;
}

/* Unmap and close the shared memory handle. */
static inline void kob_close(struct kob_handle *h)
{
	if (h->queue) {
		UnmapViewOfFile(h->queue);
		h->queue = NULL;
	}
	if (h->h_map_file) {
		CloseHandle(h->h_map_file);
		h->h_map_file = NULL;
	}
}

/* ------------------------------------------------------------------ */
/*  Queue operations                                                  */
/* ------------------------------------------------------------------ */

/*
 * Push one entry (producer side).
 *
 * entry must point to at least entry_size bytes.
 * Returns 0 on success, -1 if the error flag is already set or the
 * queue just became full. Once the error flag is set, all subsequent
 * pushes fail immediately.
 */
static inline int kob_push(struct kob_handle *h, const void *entry)
{
	struct kob_queue *q = h->queue;
	uint32_t head, tail, mask, esz;

	if (q->error)
		return -1;

	head = q->head;
	KOB_COMPILER_BARRIER();
	tail = q->tail;
	mask = q->capacity - 1;

	if (head - tail >= q->capacity) {
		q->error = 1;
		return -1;
	}

	esz = q->entry_size;
	memcpy(q->data + (size_t)(head & mask) * esz, entry, esz);
	KOB_COMPILER_BARRIER();
	q->head = head + 1;

	return 0;
}

/*
 * Peek at the front entry without consuming it (consumer side).
 *
 * Returns a pointer to the entry in the queue, or NULL if empty.
 * The pointer remains valid until kob_pop() is called.
 */
static inline void *kob_front(struct kob_handle *h)
{
	struct kob_queue *q = h->queue;
	uint32_t tail, head, mask;

	tail = q->tail;
	KOB_COMPILER_BARRIER();
	head = q->head;

	if (tail == head)
		return NULL;

	mask = q->capacity - 1;
	return q->data + (size_t)(tail & mask) * q->entry_size;
}

/*
 * Consume the front entry (consumer side).
 *
 * Must be called after kob_front() returned non-NULL.
 * Advances the tail so the producer can reuse the slot.
 */
static inline void kob_pop(struct kob_handle *h)
{
	struct kob_queue *q = h->queue;
	KOB_COMPILER_BARRIER();
	q->tail = q->tail + 1;
}

/* Returns non-zero if data loss occurred (queue was full). */
static inline int kob_has_error(struct kob_handle *h)
{
	return h->queue->error != 0;
}

#ifdef __cplusplus
}
#endif

#endif /* KIWOOM_OCX_BRIDGE_H */
