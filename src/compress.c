/*
 * compress.c
 *
 * Generic functions for compression, wrapping around actual compression
 * implementations.
 */

/*
 * Copyright (C) 2013, 2014 Eric Biggers
 *
 * This file is part of wimlib, a library for working with WIM files.
 *
 * wimlib is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 *
 * wimlib is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with wimlib; if not, see http://www.gnu.org/licenses/.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "wimlib.h"
#include "wimlib/assert.h"
#include "wimlib/error.h"
#include "wimlib/compressor_ops.h"
#include "wimlib/util.h"

#include <stdlib.h>
#include <string.h>

struct wimlib_compressor {
	const struct compressor_ops *ops;
	void *private;
	enum wimlib_compression_type ctype;
	size_t max_block_size;
};

static const struct compressor_ops *compressor_ops[] = {
	[WIMLIB_COMPRESSION_TYPE_XPRESS] = &xpress_compressor_ops,
	[WIMLIB_COMPRESSION_TYPE_LZX]    = &lzx_compressor_ops,
	[WIMLIB_COMPRESSION_TYPE_LZMS]   = &lzms_compressor_ops,
};

/* Scale: 10 = low, 50 = medium, 100 = high */

#define DEFAULT_COMPRESSION_LEVEL 50

static unsigned int default_compression_levels[ARRAY_LEN(compressor_ops)];

static bool
compressor_ctype_valid(int ctype)
{
	return (ctype >= 0 &&
		ctype < ARRAY_LEN(compressor_ops) &&
		compressor_ops[ctype] != NULL);
}

WIMLIBAPI int
wimlib_set_default_compression_level(int ctype, unsigned int compression_level)
{
	if (ctype == -1) {
		for (int i = 0; i < ARRAY_LEN(default_compression_levels); i++)
			default_compression_levels[i] = compression_level;
	} else {
		if (!compressor_ctype_valid(ctype))
			return WIMLIB_ERR_INVALID_COMPRESSION_TYPE;

		default_compression_levels[ctype] = compression_level;
	}
	return 0;
}

WIMLIBAPI u64
wimlib_get_compressor_needed_memory(enum wimlib_compression_type ctype,
				    size_t max_block_size,
				    unsigned int compression_level)
{
	const struct compressor_ops *ops;
	u64 size;

	if (!compressor_ctype_valid(ctype))
		return 0;

	ops = compressor_ops[ctype];

	if (compression_level == 0)
		compression_level = default_compression_levels[ctype];
	if (compression_level == 0)
		compression_level = DEFAULT_COMPRESSION_LEVEL;

	size = sizeof(struct wimlib_compressor);
	if (ops->get_needed_memory)
		size += ops->get_needed_memory(max_block_size, compression_level);
	return size;
}

WIMLIBAPI int
wimlib_create_compressor(enum wimlib_compression_type ctype,
			 size_t max_block_size,
			 unsigned int compression_level,
			 struct wimlib_compressor **c_ret)
{
	struct wimlib_compressor *c;

	if (c_ret == NULL)
		return WIMLIB_ERR_INVALID_PARAM;

	if (max_block_size == 0)
		return WIMLIB_ERR_INVALID_PARAM;

	if (!compressor_ctype_valid(ctype))
		return WIMLIB_ERR_INVALID_COMPRESSION_TYPE;

	c = MALLOC(sizeof(*c));
	if (c == NULL)
		return WIMLIB_ERR_NOMEM;
	c->ops = compressor_ops[ctype];
	c->private = NULL;
	c->ctype = ctype;
	c->max_block_size = max_block_size;
	if (c->ops->create_compressor) {
		int ret;

		if (compression_level == 0)
			compression_level = default_compression_levels[ctype];
		if (compression_level == 0)
			compression_level = DEFAULT_COMPRESSION_LEVEL;

		ret = c->ops->create_compressor(max_block_size,
						compression_level,
						&c->private);
		if (ret) {
			FREE(c);
			return ret;
		}
	}
	*c_ret = c;
	return 0;
}

WIMLIBAPI size_t
wimlib_compress(const void *uncompressed_data, size_t uncompressed_size,
		void *compressed_data, size_t compressed_size_avail,
		struct wimlib_compressor *c)
{
	size_t compressed_size;

	wimlib_assert(uncompressed_size <= c->max_block_size);

	compressed_size = c->ops->compress(uncompressed_data,
					   uncompressed_size,
					   compressed_data,
					   compressed_size_avail,
					   c->private);

	/* (Optional) Verify that we really get the same thing back when
	 * decompressing.  Should always be the case, unless there's a bug.  */
#ifdef ENABLE_VERIFY_COMPRESSION
	if (compressed_size != 0) {
		struct wimlib_decompressor *d;
		int res;
		u8 *buf;

		buf = MALLOC(uncompressed_size);
		if (!buf) {
			WARNING("Unable to verify results of %s compression "
				"(can't allocate buffer)",
				wimlib_get_compression_type_string(c->ctype));
			return 0;
		}

		res = wimlib_create_decompressor(c->ctype,
						 c->max_block_size, &d);
		if (res) {
			WARNING("Unable to verify results of %s compression "
				"(can't create decompressor)",
				wimlib_get_compression_type_string(c->ctype));
			FREE(buf);
			return 0;
		}

		res = wimlib_decompress(compressed_data, compressed_size,
					buf, uncompressed_size, d);
		wimlib_free_decompressor(d);
		if (res) {
			ERROR("Failed to decompress our %s-compressed data",
			      wimlib_get_compression_type_string(c->ctype));
			FREE(buf);
			abort();
		}

		res = memcmp(uncompressed_data, buf, uncompressed_size);
		FREE(buf);

		if (res) {
			ERROR("Our %s-compressed data did not decompress "
			      "to original",
			      wimlib_get_compression_type_string(c->ctype));
			abort();
		}
	}
#endif /* ENABLE_VERIFY_COMPRESSION */

	return compressed_size;
}

WIMLIBAPI void
wimlib_free_compressor(struct wimlib_compressor *c)
{
	if (c) {
		if (c->ops->free_compressor)
			c->ops->free_compressor(c->private);
		FREE(c);
	}
}
