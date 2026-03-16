/*
 * engine/main.c - 64-bit consumer process.
 *
 * Pops tick data from shared memory (produced by 32-bit Kiwoom Trader)
 * and feeds it into trcache for raw tick persistence via trade_flush_ops.
 *
 * The engine does not know the tick entry layout beyond the first
 * KOB_CODE_SIZE bytes (stock code). It strips the code prefix and
 * feeds the remaining bytes into trcache, which flushes them to
 * per-symbol binary files.
 *
 * Usage: kob_engine.exe <output_dir>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "shared/kiwoom_ocx_bridge.h"
#include "trcache/trcache.h"

#include <direct.h>
#define kob_mkdir(path) _mkdir(path)
#define kob_sleep_ms(ms) Sleep(ms)

#define KOB_MAX_SYMBOLS 8196

/* --------------------------------------------------------------- */
/*  Shutdown signal                                                */
/* --------------------------------------------------------------- */

static volatile int g_running = 1;

static BOOL WINAPI ctrl_handler(DWORD type)
{
	(void)type;
	g_running = 0;
	return TRUE;
}

/* --------------------------------------------------------------- */
/*  Dummy candle config (required by trcache, not actually used)   */
/* --------------------------------------------------------------- */

struct dummy_candle {
	struct trcache_candle_base base;
	uint64_t _dummy;
};

static uint64_t g_dummy_key;

static void dummy_candle_init(struct trcache_candle_base *c,
	void *trade, const void *book_state)
{
	(void)trade;
	(void)book_state;
	c->key.value = g_dummy_key++;
	c->is_closed = false;
}

static bool dummy_candle_update(struct trcache_candle_base *c,
	void *trade, const void *book_state)
{
	(void)c;
	(void)trade;
	(void)book_state;
	return true;
}

/* --------------------------------------------------------------- */
/*  Trade flush: persist raw tick data to binary files             */
/* --------------------------------------------------------------- */

struct flush_ctx {
	char     output_dir[260];
	uint32_t trade_data_size;  /* entry_size - KOB_CODE_SIZE */
	FILE    *fps[KOB_MAX_SYMBOLS];
};

static FILE *get_fp(struct flush_ctx *fctx, struct trcache *cache,
	int symbol_id)
{
	FILE *fp;
	const char *symbol;
	char path[520];

	if (fctx->fps[symbol_id] != NULL)
		return fctx->fps[symbol_id];

	symbol = trcache_lookup_symbol_str(cache, symbol_id);
	if (symbol == NULL)
		return NULL;

	snprintf(path, sizeof(path), "%s/%s.bin",
		fctx->output_dir, symbol);

	fp = fopen(path, "ab");
	if (fp == NULL) {
		fprintf(stderr, "Failed to open %s for writing\n", path);
		return NULL;
	}

	fctx->fps[symbol_id] = fp;
	return fp;
}

static void trade_flush(struct trcache *cache, int symbol_id,
	const void *io_block, int num_trades, void *ctx)
{
	struct flush_ctx *fctx = (struct flush_ctx *)ctx;
	FILE *fp = get_fp(fctx, cache, symbol_id);

	if (fp == NULL)
		return;

	fwrite(io_block, fctx->trade_data_size, num_trades, fp);
	fflush(fp);
}

static bool trade_flush_is_done(struct trcache *cache,
	const void *io_block, void *ctx)
{
	(void)cache;
	(void)io_block;
	(void)ctx;
	return true;
}

static void close_all_fps(struct flush_ctx *fctx)
{
	for (int i = 0; i < KOB_MAX_SYMBOLS; i++) {
		if (fctx->fps[i] != NULL) {
			fclose(fctx->fps[i]);
			fctx->fps[i] = NULL;
		}
	}
}

/* --------------------------------------------------------------- */
/*  Helpers                                                        */
/* --------------------------------------------------------------- */

static void make_today_dir(const char *base_dir, char *out_dir,
	size_t out_dir_size)
{
	time_t now = time(NULL);
	struct tm today;
	char date_str[16];

	localtime_s(&today, &now);
	strftime(date_str, sizeof(date_str), "%Y%m%d", &today);
	snprintf(out_dir, out_dir_size, "%s/%s", base_dir, date_str);
	kob_mkdir(out_dir);
}

/* --------------------------------------------------------------- */
/*  Main                                                           */
/* --------------------------------------------------------------- */

int main(int argc, char *argv[])
{
	struct kob_handle shm = {0};
	struct trcache *cache = NULL;
	struct flush_ctx fctx = {0};
	uint8_t *entry_buf = NULL;
	uint32_t entry_size;
	uint32_t trade_data_size;
	int symbol_id;
	uint64_t pop_count = 0;
	const char *base_dir;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <output_dir>\n", argv[0]);
		return 1;
	}
	base_dir = argv[1];

	SetConsoleCtrlHandler(ctrl_handler, TRUE);

	/* Create today's output directory */
	kob_mkdir(base_dir);
	make_today_dir(base_dir, fctx.output_dir, sizeof(fctx.output_dir));

	/* Open shared memory (producer must have called kob_create first) */
	if (kob_open(&shm) != 0) {
		fprintf(stderr, "Failed to open shared memory '%s'.\n"
			"Make sure the producer is running.\n",
			KOB_SHM_NAME);
		return 1;
	}

	entry_size = shm.queue->entry_size;
	trade_data_size = entry_size - KOB_CODE_SIZE;
	fctx.trade_data_size = trade_data_size;

	fprintf(stderr, "Connected to shared memory "
		"(capacity=%u, entry_size=%u, trade_data_size=%u)\n",
		shm.queue->capacity, entry_size, trade_data_size);

	if (entry_size <= KOB_CODE_SIZE) {
		fprintf(stderr,
			"Entry size %u is too small "
			"(must be > %d for stock code + data)\n",
			entry_size, KOB_CODE_SIZE);
		kob_close(&shm);
		return 1;
	}

	entry_buf = (uint8_t *)malloc(entry_size);
	if (entry_buf == NULL) {
		fprintf(stderr, "Failed to allocate entry buffer\n");
		kob_close(&shm);
		return 1;
	}

	/* Set up trcache */
	struct trcache_field_def dummy_fields[] = {
		{
			offsetof(struct dummy_candle, _dummy),
			sizeof(uint64_t),
			FIELD_TYPE_UINT64
		},
	};

	struct trcache_candle_config candle_configs[] = {{
		.user_candle_size = sizeof(struct dummy_candle),
		.field_definitions = dummy_fields,
		.num_fields = 1,
		.update_ops = {
			.init = dummy_candle_init,
			.update = dummy_candle_update,
		},
		.batch_flush_ops = {0},
	}};

	struct trcache_init_ctx init_ctx = {
		.candle_configs      = candle_configs,
		.num_candle_configs  = 1,
		.batch_candle_count_pow2  = 10,
		.cached_batch_count_pow2  = 0,
		.total_memory_limit  = (size_t)1 << 30,
		.num_worker_threads  = 3,
		.max_symbols         = KOB_MAX_SYMBOLS,
		.trade_data_size     = trade_data_size,
		.trade_flush_ops     = {
			.flush   = trade_flush,
			.is_done = trade_flush_is_done,
			.ctx     = &fctx,
		},
	};

	cache = trcache_init(&init_ctx);
	if (cache == NULL) {
		fprintf(stderr, "trcache_init failed\n");
		free(entry_buf);
		kob_close(&shm);
		return 1;
	}

	fprintf(stderr, "Engine started. Output: %s\n", fctx.output_dir);

	/* Consumer loop */
	while (g_running) {
		if (kob_pop(&shm, entry_buf) != 0) {
			if (kob_has_error(&shm)) {
				fprintf(stderr,
					"FATAL: queue was full, tick data "
					"lost. ticks_processed=%llu, "
					"capacity=%u, entry_size=%u\n",
					(unsigned long long)pop_count,
					shm.queue->capacity,
					shm.queue->entry_size);
				abort();
			}
			kob_sleep_ms(1);
			continue;
		}

		/*
		 * First KOB_CODE_SIZE bytes = stock code (protocol).
		 * The rest is opaque trade data fed into trcache.
		 */
		symbol_id = trcache_lookup_symbol_id(
			cache, (const char *)entry_buf);
		if (symbol_id < 0) {
			symbol_id = trcache_register_symbol(
				cache, (const char *)entry_buf);
			if (symbol_id < 0) {
				fprintf(stderr,
					"FATAL: failed to register "
					"symbol '%.*s', "
					"ticks_processed=%llu, "
					"max_symbols=%d\n",
					KOB_CODE_SIZE, entry_buf,
					(unsigned long long)pop_count,
					KOB_MAX_SYMBOLS);
				abort();
			}
		}

		if (trcache_feed_trade_data(cache,
			entry_buf + KOB_CODE_SIZE, symbol_id) != 0) {
			fprintf(stderr,
				"FATAL: trcache_feed_trade_data failed, "
				"symbol='%.*s', symbol_id=%d, "
				"ticks_processed=%llu, "
				"memory_limit=%zu\n",
				KOB_CODE_SIZE, entry_buf,
				symbol_id,
				(unsigned long long)pop_count,
				(size_t)1 << 30);
			abort();
		}

		pop_count++;
	}

	fprintf(stderr, "Shutting down. Total ticks processed: %llu\n",
		(unsigned long long)pop_count);

	trcache_destroy(cache);
	close_all_fps(&fctx);
	free(entry_buf);
	kob_close(&shm);
	return 0;
}
