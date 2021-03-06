/*
 * compress_serial.c
 *
 * Compress chunks of data (serial version).
 */

/*
 * Copyright (C) 2013 Eric Biggers
 *
 * This file is part of wimlib, a library for working with WIM files.
 *
 * wimlib is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version.
 *
 * wimlib is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * wimlib; if not, see http://www.gnu.org/licenses/.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "wimlib.h"
#include "wimlib/assert.h"
#include "wimlib/chunk_compressor.h"
#include "wimlib/util.h"

#include <string.h>

struct serial_chunk_compressor {
	struct chunk_compressor base;
	struct wimlib_compressor *compressor;
	u8 *udata;
	u8 *cdata;
	u32 ulen;
};

static void
serial_chunk_compressor_destroy(struct chunk_compressor *_ctx)
{
	struct serial_chunk_compressor *ctx = (struct serial_chunk_compressor*)_ctx;

	if (ctx == NULL)
		return;

	wimlib_free_compressor(ctx->compressor);
	FREE(ctx->udata);
	FREE(ctx->cdata);
	FREE(ctx);
}

static bool
serial_chunk_compressor_submit_chunk(struct chunk_compressor *_ctx,
				     const void *chunk, u32 size)
{
	struct serial_chunk_compressor *ctx = (struct serial_chunk_compressor*)_ctx;

	if (ctx->ulen != 0)
		return false;

	wimlib_assert(size > 0);
	wimlib_assert(size <= ctx->base.out_chunk_size);

	memcpy(ctx->udata, chunk, size);
	ctx->ulen = size;
	return true;
}

static bool
serial_chunk_compressor_get_chunk(struct chunk_compressor *_ctx,
				  const void **cdata_ret, u32 *csize_ret,
				  u32 *usize_ret)
{
	struct serial_chunk_compressor *ctx = (struct serial_chunk_compressor*)_ctx;
	u32 clen;

	if (ctx->ulen == 0)
		return false;

	clen = wimlib_compress(ctx->udata, ctx->ulen,
			       ctx->cdata, ctx->ulen - 1,
			       ctx->compressor);

	if (clen) {
		*cdata_ret = ctx->cdata;
		*csize_ret = clen;
	} else {
		*cdata_ret = ctx->udata;
		*csize_ret = ctx->ulen;
	}
	*usize_ret = ctx->ulen;

	ctx->ulen = 0;
	return true;
}

int
new_serial_chunk_compressor(int out_ctype, u32 out_chunk_size,
			    struct chunk_compressor **compressor_ret)
{
	struct serial_chunk_compressor *ctx;
	int ret;

	wimlib_assert(out_chunk_size > 0);

	ctx = CALLOC(1, sizeof(*ctx));
	if (ctx == NULL)
		return WIMLIB_ERR_NOMEM;

	ctx->base.out_ctype = out_ctype;
	ctx->base.out_chunk_size = out_chunk_size;
	ctx->base.num_threads = 1;
	ctx->base.destroy = serial_chunk_compressor_destroy;
	ctx->base.submit_chunk = serial_chunk_compressor_submit_chunk;
	ctx->base.get_chunk = serial_chunk_compressor_get_chunk;

	ret = wimlib_create_compressor(out_ctype, out_chunk_size,
				       0, &ctx->compressor);
	if (ret)
		goto err;

	ctx->udata = MALLOC(out_chunk_size);
	ctx->cdata = MALLOC(out_chunk_size - 1);
	ctx->ulen = 0;
	if (ctx->udata == NULL || ctx->cdata == NULL) {
		ret = WIMLIB_ERR_NOMEM;
		goto err;
	}

	*compressor_ret = &ctx->base;
	return 0;

err:
	serial_chunk_compressor_destroy(&ctx->base);
	return ret;
}
