/*
 * Copyright (c) 2026 TaiJu Wu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Memory allocator torture tests. Scenarios 1, 4 and 5 follow stress-ng
 * --malloc --verify on a k_heap, the system heap and the ESP32-S3 PSRAM
 * shared multi heap; scenarios 2 and 3 have no Linux counterpart and are
 * marked as such. Every block carries a pattern that is checked before it
 * is freed, so overlapping or reused blocks show up as corruption. The
 * deviations from stress-ng are listed in README.rst.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/sys_heap.h>
#include <string.h>
#if defined(CONFIG_SHARED_MULTI_HEAP) && defined(CONFIG_ESP_SPIRAM)
#include <zephyr/multi_heap/shared_multi_heap.h>
#define PSRAM_TEST 1
#endif

#define DUR_MS   CONFIG_MEM_TORTURE_DURATION_MS
#define STALL_MS CONFIG_MEM_TORTURE_STALL_MS
#define NTHREADS CONFIG_MEM_TORTURE_MAX_THREADS
#define STACK    1536
#define SLOTS    8

K_THREAD_STACK_ARRAY_DEFINE(stacks, NTHREADS, STACK);
static struct k_thread threads[NTHREADS];
K_THREAD_STACK_DEFINE(mon_stack, 1024);
static struct k_thread mon_thread;

K_HEAP_DEFINE(test_heap, CONFIG_MEM_TORTURE_HEAP_SIZE);
K_HEAP_DEFINE(small_heap, 4096);

static volatile bool stop;
static atomic_t corrupt, misaligned, failed, done_ops;

static uint32_t rnd(uint32_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 17;
	*s ^= *s << 5;
	return *s;
}

/* Run seed: CONFIG_MEM_TORTURE_SEED, or a new one each boot (printed, so a run
 * can be replayed). Every thread mixes it into its own xorshift state.
 */
static uint32_t seed;

static uint32_t seed_mix(uint32_t c)
{
	uint32_t s = c ^ seed;

	return (s != 0U) ? s : 1U;
}

static void *seed_setup(void)
{
	seed = CONFIG_MEM_TORTURE_SEED;
	if (seed == 0U) {
		seed = k_cycle_get_32() * 0x9e3779b9u;
	}
	printk("mem-torture seed 0x%08x (CONFIG_MEM_TORTURE_SEED=0x%08x replays it)\n", seed, seed);
	return NULL;
}

static int nworkers(int per_cpu)
{
	return MIN(per_cpu * (int)arch_num_cpus() + 1, NTHREADS);
}

/* ---- progress monitor (same idea as sched_torture) ---- */
static volatile uint32_t progress[NTHREADS];
static int nmon;
static volatile uint32_t stalls;

static void monitor_fn(void *a, void *b, void *c)
{
	uint32_t last[NTHREADS];
	int64_t since[NTHREADS];
	bool reported[NTHREADS] = {0};

	for (int i = 0; i < nmon; i++) {
		last[i] = progress[i];
		since[i] = k_uptime_get();
	}
	while (!stop) {
		k_msleep(100);

		int64_t now = k_uptime_get();

		for (int i = 0; i < nmon; i++) {
			if (progress[i] != last[i]) {
				last[i] = progress[i];
				since[i] = now;
			} else if (!reported[i] && now - since[i] > STALL_MS) {
				printk("MT stall: thread %d made no progress for %lld ms\n", i,
				       now - since[i]);
				reported[i] = true;
				stalls++;
			}
		}
	}
}

static void start_workers(int n, k_thread_entry_t fn)
{
	nmon = n;
	stalls = 0;
	memset((void *)progress, 0, sizeof(progress));
	k_thread_create(&mon_thread, mon_stack, K_THREAD_STACK_SIZEOF(mon_stack), monitor_fn,
			NULL, NULL, NULL, K_PRIO_COOP(1), 0, K_NO_WAIT);
	for (int i = 0; i < n; i++) {
		k_thread_create(&threads[i], stacks[i], STACK, fn, (void *)(uintptr_t)i, NULL, NULL,
				K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	}
}

static void run_and_join(int n)
{
	k_msleep(DUR_MS);
	stop = true;
	for (int i = 0; i < n; i++) {
		k_thread_join(&threads[i], K_FOREVER);
	}
	k_thread_join(&mon_thread, K_FOREVER);
}

static void before(void *f)
{
	stop = false;
	atomic_clear(&corrupt);
	atomic_clear(&misaligned);
	atomic_clear(&failed);
	atomic_clear(&done_ops);
}

/* ---- block patterns ---- */
struct slot {
	uint8_t *p;
	uint32_t size, tag;
};

static void fill(uint8_t *p, uint32_t size, uint32_t tag)
{
	for (uint32_t i = 0; i < size; i++) {
		p[i] = (uint8_t)(tag + i * 7);
	}
}

static bool intact(const uint8_t *p, uint32_t size, uint32_t tag)
{
	for (uint32_t i = 0; i < size; i++) {
		if (p[i] != (uint8_t)(tag + i * 7)) {
			return false;
		}
	}
	return true;
}

/* Mostly small, some medium, a few large: the mix that fragments a heap. */
static uint32_t pick_size(uint32_t *s, uint32_t large)
{
	uint32_t r = rnd(s) % 100;

	if (r < 70) {
		return 8 + rnd(s) % 57;
	} else if (r < 95) {
		return 64 + rnd(s) % 961;
	}
	return 1024 + rnd(s) % (large - 1024);
}

static bool heap_valid(struct k_heap *h)
{
	bool ok;

	K_SPINLOCK(&h->lock) {
		ok = sys_heap_validate(&h->heap);
	}
	return ok;
}

static size_t heap_allocated(struct k_heap *h)
{
	struct sys_memory_stats st;

	sys_heap_runtime_stats_get(&h->heap, &st);
	return st.allocated_bytes;
}

/* ---------------------------------------------------------------------------
 * 1. stress-ng --malloc --verify on a k_heap: instances randomly allocate,
 * reallocate and free blocks and verify their contents; aligned allocation
 * is added since k_heap offers it (see README).
 */
static void churn_worker(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a;
	uint32_t s = seed_mix(0x9e3779b9u * (id + 1)), tag = id << 24;
	struct slot sl[SLOTS] = {0};

	while (!stop) {
		int k = rnd(&s) % SLOTS;
		struct slot *x = &sl[k];

		if (x->p == NULL) {
			uint32_t size = pick_size(&s, 4096), align = 4U << (rnd(&s) % 5);

			x->p = k_heap_aligned_alloc(&test_heap, align, size, K_NO_WAIT);
			if (x->p == NULL) {
				atomic_inc(&failed);
			} else {
				if ((uintptr_t)x->p & (align - 1)) {
					atomic_inc(&misaligned);
				}
				x->size = size;
				x->tag = ++tag;
				fill(x->p, size, x->tag);
			}
		} else if (rnd(&s) % 4 == 0) {
			/* realloc keeps the common prefix */
			uint32_t size = pick_size(&s, 4096), keep = MIN(size, x->size);
			uint8_t *np;

			if (!intact(x->p, x->size, x->tag)) {
				atomic_inc(&corrupt);
			}
			np = k_heap_realloc(&test_heap, x->p, size, K_NO_WAIT);
			if (np == NULL) {
				atomic_inc(&failed);
			} else {
				if (!intact(np, keep, x->tag)) {
					atomic_inc(&corrupt);
				}
				x->p = np;
				x->size = size;
				x->tag = ++tag;
				fill(np, size, x->tag);
			}
		} else {
			if (!intact(x->p, x->size, x->tag)) {
				atomic_inc(&corrupt);
			}
			k_heap_free(&test_heap, x->p);
			x->p = NULL;
		}
		atomic_inc(&done_ops);
		progress[id]++;
		if ((rnd(&s) & 31) == 0) {
			k_yield();
		}
	}
	for (int k = 0; k < SLOTS; k++) {
		if (sl[k].p) {
			if (!intact(sl[k].p, sl[k].size, sl[k].tag)) {
				atomic_inc(&corrupt);
			}
			k_heap_free(&test_heap, sl[k].p);
		}
	}
}

ZTEST(mem_torture, test_1_heap_churn)
{
	int n = nworkers(3);
	uint32_t checks = 0, invalid = 0;

	start_workers(n, churn_worker);
	for (int64_t end = k_uptime_get() + DUR_MS; k_uptime_get() < end;) {
		k_msleep(50);
		checks++;
		invalid += !heap_valid(&test_heap);
	}
	stop = true;
	for (int i = 0; i < n; i++) {
		k_thread_join(&threads[i], K_FOREVER);
	}
	k_thread_join(&mon_thread, K_FOREVER);

	size_t left = heap_allocated(&test_heap);

	printk("MT heap churn: %d threads, %ld ops, failed allocs %ld, corrupt %ld, misaligned %ld, "
	       "invalid heap %u/%u, left allocated %zu\n",
	       n, (long)atomic_get(&done_ops), (long)atomic_get(&failed),
	       (long)atomic_get(&corrupt), (long)atomic_get(&misaligned), invalid, checks, left);
	zassert_equal(atomic_get(&corrupt), 0, "a block was overwritten while in use");
	zassert_equal(atomic_get(&misaligned), 0);
	zassert_equal(invalid, 0, "sys_heap_validate() failed");
	zassert_true(heap_valid(&test_heap));
	zassert_equal(left, 0, "heap not empty after everything was freed");
	zassert_equal(stalls, 0);
}

/* ---------------------------------------------------------------------------
 * 2. exhaustion (no Linux counterpart): a small heap shared by more
 * threads than it can serve, using blocking allocations; a free must wake
 * a waiter.
 */
static atomic_t ex_timeouts;

static void exhaust_worker(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a;
	uint32_t s = seed_mix(0x1234567u + id);

	while (!stop) {
		uint32_t size = 256 + rnd(&s) % 769;
		uint8_t *p = k_heap_alloc(&small_heap, size, K_MSEC(50));

		if (p == NULL) {
			atomic_inc(&ex_timeouts);
		} else {
			fill(p, size, id);
			k_busy_wait(rnd(&s) % 300);
			if (!intact(p, size, id)) {
				atomic_inc(&corrupt);
			}
			k_heap_free(&small_heap, p);
			atomic_inc(&done_ops);
		}
		progress[id]++;
	}
}

ZTEST(mem_torture, test_2_heap_exhaustion)
{
	int n = nworkers(4);

	atomic_clear(&ex_timeouts);
	start_workers(n, exhaust_worker);
	run_and_join(n);
	printk("MT exhaustion: %d threads on a 4 KiB heap, %ld allocations, %ld timeouts, "
	       "corrupt %ld\n",
	       n, (long)atomic_get(&done_ops), (long)atomic_get(&ex_timeouts),
	       (long)atomic_get(&corrupt));
	zassert_true(atomic_get(&done_ops) > 0);
	zassert_equal(atomic_get(&corrupt), 0);
	zassert_equal(heap_allocated(&small_heap), 0);
	zassert_true(heap_valid(&small_heap));
	zassert_equal(stalls, 0, "a thread waiting for memory was never woken");
}

/* ---------------------------------------------------------------------------
 * 3. mem slab shared by threads and a timer ISR (no Linux counterpart). The slab keeps its free
 * list in the first word of a free block, so the ownership mark sits after it.
 */
#define SLAB_BLOCKS 24
#define SLAB_SIZE   64
#define FREE_MARK   0xf4eef4eeu

K_MEM_SLAB_DEFINE(test_slab, SLAB_SIZE, SLAB_BLOCKS, 8);

struct sblock {
	uintptr_t slab_link;
	uint32_t mark;
	uint8_t data[SLAB_SIZE - sizeof(uintptr_t) - sizeof(uint32_t)];
};

static atomic_t sl_double, sl_isr_ops;
static struct sblock *isr_held[4];

static bool slab_take(struct sblock **out, uint32_t owner)
{
	void *m;

	if (k_mem_slab_alloc(&test_slab, &m, K_NO_WAIT) != 0) {
		return false;
	}
	*out = m;
	if ((*out)->mark != FREE_MARK) {
		atomic_inc(&sl_double);
	}
	(*out)->mark = owner;
	memset((*out)->data, (uint8_t)owner, sizeof((*out)->data));
	return true;
}

static void slab_give(struct sblock *b, uint32_t owner)
{
	bool ok = b->mark == owner;

	for (size_t i = 0; ok && i < sizeof(b->data); i++) {
		ok = b->data[i] == (uint8_t)owner;
	}
	if (!ok) {
		atomic_inc(&corrupt);
	}
	b->mark = FREE_MARK;
	k_mem_slab_free(&test_slab, b);
}

static void slab_isr(struct k_timer *t)
{
	static uint32_t i;
	struct sblock **h = &isr_held[i++ % ARRAY_SIZE(isr_held)];

	if (*h) {
		slab_give(*h, 0x15e00000u);
		*h = NULL;
	} else {
		slab_take(h, 0x15e00000u);
	}
	atomic_inc(&sl_isr_ops);
}
K_TIMER_DEFINE(slab_timer, slab_isr, NULL);

static void slab_worker(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a;
	uint32_t s = seed_mix(0xabcdefu + id), owner = 0x7e000000u | id;
	struct sblock *held[3] = {0};

	while (!stop) {
		int k = rnd(&s) % ARRAY_SIZE(held);

		if (held[k]) {
			slab_give(held[k], owner);
			held[k] = NULL;
		} else if (!slab_take(&held[k], owner)) {
			atomic_inc(&failed);
		}
		atomic_inc(&done_ops);
		progress[id]++;
		if ((rnd(&s) & 7) == 0) {
			k_yield();
		}
	}
	for (int k = 0; k < (int)ARRAY_SIZE(held); k++) {
		if (held[k]) {
			slab_give(held[k], owner);
		}
	}
}

ZTEST(mem_torture, test_3_slab_isr)
{
	int n = nworkers(3);
	void *all[SLAB_BLOCKS];

	/* Mark every block free once, so the first allocation can be checked. */
	for (int i = 0; i < SLAB_BLOCKS; i++) {
		zassert_ok(k_mem_slab_alloc(&test_slab, &all[i], K_NO_WAIT));
		((struct sblock *)all[i])->mark = FREE_MARK;
	}
	for (int i = 0; i < SLAB_BLOCKS; i++) {
		k_mem_slab_free(&test_slab, all[i]);
	}
	atomic_clear(&sl_double);
	atomic_clear(&sl_isr_ops);
	memset(isr_held, 0, sizeof(isr_held));
	k_timer_start(&slab_timer, K_MSEC(1), K_MSEC(1));
	start_workers(n, slab_worker);
	run_and_join(n);
	k_timer_stop(&slab_timer);
	for (int i = 0; i < (int)ARRAY_SIZE(isr_held); i++) {
		if (isr_held[i]) {
			slab_give(isr_held[i], 0x15e00000u);
		}
	}
	printk("MT slab: %d threads + ISR, %ld thread ops, %ld ISR ops, empty %ld, double %ld, "
	       "corrupt %ld, max used %u/%d, used now %u\n",
	       n, (long)atomic_get(&done_ops), (long)atomic_get(&sl_isr_ops),
	       (long)atomic_get(&failed), (long)atomic_get(&sl_double),
	       (long)atomic_get(&corrupt), k_mem_slab_max_used_get(&test_slab), SLAB_BLOCKS,
	       k_mem_slab_num_used_get(&test_slab));
	zassert_equal(atomic_get(&sl_double), 0, "a block was handed out twice");
	zassert_equal(atomic_get(&corrupt), 0);
	zassert_equal(k_mem_slab_num_used_get(&test_slab), 0, "blocks leaked");
	zassert_equal(stalls, 0);
}

/* ---------------------------------------------------------------------------
 * 4. stress-ng --malloc --verify on the system heap: k_malloc, k_calloc
 * (zero fill checked, as with --malloc-zerofree) and k_free.
 */
static atomic_t calloc_dirty;

static void kmalloc_worker(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a;
	uint32_t s = seed_mix(0x55aa55aau + id), tag = id << 24;
	struct slot sl[SLOTS] = {0};

	while (!stop) {
		struct slot *x = &sl[rnd(&s) % SLOTS];

		if (x->p) {
			if (!intact(x->p, x->size, x->tag)) {
				atomic_inc(&corrupt);
			}
			k_free(x->p);
			x->p = NULL;
		} else {
			uint32_t size = pick_size(&s, 2048);
			bool zero = rnd(&s) & 1;

			x->p = zero ? k_calloc(1, size) : k_malloc(size);
			if (x->p == NULL) {
				atomic_inc(&failed);
			} else {
				for (uint32_t i = 0; zero && i < size; i++) {
					if (x->p[i] != 0) {
						atomic_inc(&calloc_dirty);
						break;
					}
				}
				x->size = size;
				x->tag = ++tag;
				fill(x->p, size, x->tag);
			}
		}
		atomic_inc(&done_ops);
		progress[id]++;
	}
	for (int k = 0; k < SLOTS; k++) {
		if (sl[k].p) {
			k_free(sl[k].p);
		}
	}
}

ZTEST(mem_torture, test_4_kmalloc)
{
	int n = nworkers(2);

	atomic_clear(&calloc_dirty);
	start_workers(n, kmalloc_worker);
	run_and_join(n);
	printk("MT k_malloc: %d threads, %ld ops, failed %ld, corrupt %ld, dirty calloc %ld\n", n,
	       (long)atomic_get(&done_ops), (long)atomic_get(&failed), (long)atomic_get(&corrupt),
	       (long)atomic_get(&calloc_dirty));
	zassert_equal(atomic_get(&corrupt), 0);
	zassert_equal(atomic_get(&calloc_dirty), 0, "k_calloc returned non-zeroed memory");
	zassert_equal(stalls, 0);
}

/* ---------------------------------------------------------------------------
 * 5. stress-ng --malloc --verify on the ESP32-S3 PSRAM region of the
 * shared multi heap.
 */
#ifdef PSRAM_TEST
static void psram_worker(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a;
	uint32_t s = seed_mix(0x31415926u + id), tag = id << 24;
	struct slot sl[SLOTS] = {0};

	while (!stop) {
		struct slot *x = &sl[rnd(&s) % SLOTS];

		if (x->p) {
			if (!intact(x->p, x->size, x->tag)) {
				atomic_inc(&corrupt);
			}
			shared_multi_heap_free(x->p);
			x->p = NULL;
		} else {
			uint32_t size = pick_size(&s, 8192), align = 4U << (rnd(&s) % 5);

			x->p = shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL, align, size);
			if (x->p == NULL) {
				atomic_inc(&failed);
			} else {
				if ((uintptr_t)x->p & (align - 1)) {
					atomic_inc(&misaligned);
				}
				x->size = size;
				x->tag = ++tag;
				fill(x->p, size, x->tag);
			}
		}
		atomic_inc(&done_ops);
		progress[id]++;
	}
	for (int k = 0; k < SLOTS; k++) {
		if (sl[k].p) {
			shared_multi_heap_free(sl[k].p);
		}
	}
}
#endif

ZTEST(mem_torture, test_5_psram_heap)
{
#ifdef PSRAM_TEST
	int n = nworkers(3);

	start_workers(n, psram_worker);
	run_and_join(n);
	printk("MT psram heap: %d threads, %ld ops, failed %ld, corrupt %ld, misaligned %ld\n", n,
	       (long)atomic_get(&done_ops), (long)atomic_get(&failed), (long)atomic_get(&corrupt),
	       (long)atomic_get(&misaligned));
	zassert_equal(atomic_get(&corrupt), 0);
	zassert_equal(atomic_get(&misaligned), 0);
	zassert_equal(stalls, 0);
#else
	ztest_test_skip();
#endif
}

ZTEST_SUITE(mem_torture, NULL, seed_setup, before, NULL, NULL);
