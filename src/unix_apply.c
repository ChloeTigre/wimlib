/*
 * unix_apply.c - Code to apply files from a WIM image on UNIX.
 */

/*
 * Copyright (C) 2012, 2013, 2014 Eric Biggers
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

#include "wimlib/apply.h"
#include "wimlib/assert.h"
#include "wimlib/dentry.h"
#include "wimlib/error.h"
#include "wimlib/file_io.h"
#include "wimlib/reparse.h"
#include "wimlib/timestamp.h"
#include "wimlib/unix_data.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/* We don't require O_NOFOLLOW, but the advantage of having it is that if we
 * need to extract a file to a location at which there exists a symbolic link,
 * open(..., O_NOFOLLOW | ...) recognizes the symbolic link rather than
 * following it and creating the file somewhere else.  (Equivalent to
 * FILE_OPEN_REPARSE_POINT on Windows.)  */
#ifndef O_NOFOLLOW
#  define O_NOFOLLOW 0
#endif

static int
unix_get_supported_features(const char *target,
			    struct wim_features *supported_features)
{
	supported_features->hard_links = 1;
	supported_features->symlink_reparse_points = 1;
	supported_features->unix_data = 1;
	supported_features->timestamps = 1;
	supported_features->case_sensitive_filenames = 1;
	return 0;
}

#define NUM_PATHBUFS 2  /* We need 2 when creating hard links  */

struct unix_apply_ctx {
	/* Extract flags, the pointer to the WIMStruct, etc.  */
	struct apply_ctx common;

	/* Buffers for building extraction paths (allocated).  */
	char *pathbufs[NUM_PATHBUFS];

	/* Index of next pathbuf to use  */
	unsigned which_pathbuf;

	/* Currently open file descriptors for extraction  */
	struct filedes open_fds[MAX_OPEN_STREAMS];

	/* Number of currently open file descriptors in open_fds, starting from
	 * the beginning of the array.  */
	unsigned num_open_fds;

	/* Buffer for reading reparse data streams into memory  */
	u8 reparse_data[REPARSE_DATA_MAX_SIZE];

	/* Pointer to the next byte in @reparse_data to fill  */
	u8 *reparse_ptr;

	/* Absolute path to the target directory (allocated buffer).  Only set
	 * if needed for absolute symbolic link fixups.  */
	char *target_abspath;

	/* Number of characters in target_abspath.  */
	size_t target_abspath_nchars;

	/* Number of special files we couldn't create due to EPERM  */
	unsigned long num_special_files_ignored;
};

/* Returns the number of characters needed to represent the path to the
 * specified @dentry when extracted, not including the null terminator or the
 * path to the target directory itself.  */
static size_t
unix_dentry_path_length(const struct wim_dentry *dentry)
{
	size_t len = 0;
	const struct wim_dentry *d;

	d = dentry;
	do {
		len += d->d_extraction_name_nchars + 1;
		d = d->d_parent;
	} while (!dentry_is_root(d) && will_extract_dentry(d));

	return len;
}

/* Returns the maximum number of characters needed to represent the path to any
 * dentry in @dentry_list when extracted, including the null terminator and the
 * path to the target directory itself.  */
static size_t
unix_compute_path_max(const struct list_head *dentry_list,
		      const struct unix_apply_ctx *ctx)
{
	size_t max = 0;
	size_t len;
	const struct wim_dentry *dentry;

	list_for_each_entry(dentry, dentry_list, d_extraction_list_node) {
		len = unix_dentry_path_length(dentry);
		if (len > max)
			max = len;
	}

	/* Account for target and null terminator.  */
	return ctx->common.target_nchars + max + 1;
}

/* Builds and returns the filesystem path to which to extract @dentry.
 * This cycles through NUM_PATHBUFS different buffers.  */
static const char *
unix_build_extraction_path(const struct wim_dentry *dentry,
			   struct unix_apply_ctx *ctx)
{
	char *pathbuf;
	char *p;
	const struct wim_dentry *d;

	pathbuf = ctx->pathbufs[ctx->which_pathbuf];
	ctx->which_pathbuf = (ctx->which_pathbuf + 1) % NUM_PATHBUFS;

	p = &pathbuf[ctx->common.target_nchars +
		     unix_dentry_path_length(dentry)];
	*p = '\0';
	d = dentry;
	do {
		p -= d->d_extraction_name_nchars;
		memcpy(p, d->d_extraction_name, d->d_extraction_name_nchars);
		*--p = '/';
		d = d->d_parent;
	} while (!dentry_is_root(d) && will_extract_dentry(d));

	return pathbuf;
}

/* This causes the next call to unix_build_extraction_path() to use the same
 * path buffer as the previous call.  */
static void
unix_reuse_pathbuf(struct unix_apply_ctx *ctx)
{
	ctx->which_pathbuf = (ctx->which_pathbuf - 1) % NUM_PATHBUFS;
}

/* Builds and returns the filesystem path to which to extract an unspecified
 * alias of the @inode.  This cycles through NUM_PATHBUFS different buffers.  */
static const char *
unix_build_inode_extraction_path(const struct wim_inode *inode,
				 struct unix_apply_ctx *ctx)
{
	return unix_build_extraction_path(inode_first_extraction_dentry(inode), ctx);
}

/* Sets the timestamps on a file being extracted.
 *
 * Either @fd or @path must be specified (not -1 and not NULL, respectively).
 */
static int
unix_set_timestamps(int fd, const char *path, u64 atime, u64 mtime)
{
	{
		struct timespec times[2];

		times[0] = wim_timestamp_to_timespec(atime);
		times[1] = wim_timestamp_to_timespec(mtime);

		errno = ENOSYS;
#ifdef HAVE_FUTIMENS
		if (fd >= 0 && !futimens(fd, times))
			return 0;
#endif
#ifdef HAVE_UTIMENSAT
		if (fd < 0 && !utimensat(AT_FDCWD, path, times, AT_SYMLINK_NOFOLLOW))
			return 0;
#endif
		if (errno != ENOSYS)
			return WIMLIB_ERR_SET_TIMESTAMPS;
	}
	{
		struct timeval times[2];

		times[0] = wim_timestamp_to_timeval(atime);
		times[1] = wim_timestamp_to_timeval(mtime);

		if (fd >= 0 && !futimes(fd, times))
			return 0;
		if (fd < 0 && !lutimes(path, times))
			return 0;
		return WIMLIB_ERR_SET_TIMESTAMPS;
	}
}

static int
unix_set_owner_and_group(int fd, const char *path, uid_t uid, gid_t gid)
{
	if (fd >= 0 && !fchown(fd, uid, gid))
		return 0;
	if (fd < 0 && !lchown(path, uid, gid))
		return 0;
	return WIMLIB_ERR_SET_SECURITY;
}

static int
unix_set_mode(int fd, const char *path, mode_t mode)
{
	if (fd >= 0 && !fchmod(fd, mode))
		return 0;
	if (fd < 0 && !chmod(path, mode))
		return 0;
	return WIMLIB_ERR_SET_SECURITY;
}

/*
 * Set metadata on an extracted file.
 *
 * @fd is an open file descriptor to the extracted file, or -1.  @path is the
 * path to the extracted file, or NULL.  If valid, this function uses @fd.
 * Otherwise, if valid, it uses @path.  Otherwise, it calculates the path to one
 * alias of the extracted file and uses it.
 */
static int
unix_set_metadata(int fd, const struct wim_inode *inode,
		  const char *path, struct unix_apply_ctx *ctx)
{
	int ret;
	struct wimlib_unix_data unix_data;

	if (fd < 0 && !path)
		path = unix_build_inode_extraction_path(inode, ctx);

	if ((ctx->common.extract_flags & WIMLIB_EXTRACT_FLAG_UNIX_DATA)
	    && inode_get_unix_data(inode, &unix_data))
	{
		u32 uid = unix_data.uid;
		u32 gid = unix_data.gid;
		u32 mode = unix_data.mode;

		ret = unix_set_owner_and_group(fd, path, uid, gid);
		if (ret) {
			if (!path)
				path = unix_build_inode_extraction_path(inode, ctx);
			if (ctx->common.extract_flags &
			    WIMLIB_EXTRACT_FLAG_STRICT_ACLS)
			{
				ERROR_WITH_ERRNO("Can't set uid=%"PRIu32" and "
						 "gid=%"PRIu32" on \"%s\"",
						 uid, gid, path);
				return ret;
			} else {
				WARNING_WITH_ERRNO("Can't set uid=%"PRIu32" and "
						   "gid=%"PRIu32" on \"%s\"",
						   uid, gid, path);
			}
		}

		ret = 0;
		if (!inode_is_symlink(inode))
			ret = unix_set_mode(fd, path, mode);
		if (ret) {
			if (!path)
				path = unix_build_inode_extraction_path(inode, ctx);
			if (ctx->common.extract_flags &
			    WIMLIB_EXTRACT_FLAG_STRICT_ACLS)
			{
				ERROR_WITH_ERRNO("Can't set mode=0%"PRIo32" "
						 "on \"%s\"", mode, path);
				return ret;
			} else {
				WARNING_WITH_ERRNO("Can't set mode=0%"PRIo32" "
						   "on \"%s\"", mode, path);
			}
		}
	}

	ret = unix_set_timestamps(fd, path,
				  inode->i_last_access_time,
				  inode->i_last_write_time);
	if (ret) {
		if (!path)
			path = unix_build_inode_extraction_path(inode, ctx);
		if (ctx->common.extract_flags &
		    WIMLIB_EXTRACT_FLAG_STRICT_TIMESTAMPS)
		{
			ERROR_WITH_ERRNO("Can't set timestamps on \"%s\"", path);
			return ret;
		} else {
			WARNING_WITH_ERRNO("Can't set timestamps on \"%s\"", path);
		}
	}
	return 0;
}

/* Extract all needed aliases of the @inode, where one alias, corresponding to
 * @first_dentry, has already been extracted to @first_path.  */
static int
unix_create_hardlinks(const struct wim_inode *inode,
		      const struct wim_dentry *first_dentry,
		      const char *first_path, struct unix_apply_ctx *ctx)
{
	const struct wim_dentry *dentry;
	const char *newpath;

	list_for_each_entry(dentry, &inode->i_extraction_aliases,
			    d_extraction_alias_node)
	{
		if (dentry == first_dentry)
			continue;

		newpath = unix_build_extraction_path(dentry, ctx);
	retry_link:
		if (link(first_path, newpath)) {
			if (errno == EEXIST && !unlink(newpath))
				goto retry_link;
			ERROR_WITH_ERRNO("Can't create hard link "
					 "\"%s\" => \"%s\"", newpath, first_path);
			return WIMLIB_ERR_LINK;
		}
		unix_reuse_pathbuf(ctx);
	}
	return 0;
}

/* If @dentry represents a directory, create it.  */
static int
unix_create_if_directory(const struct wim_dentry *dentry,
			 struct unix_apply_ctx *ctx)
{
	const char *path;
	struct stat stbuf;

	if (!dentry_is_directory(dentry))
		return 0;

	path = unix_build_extraction_path(dentry, ctx);
	if (mkdir(path, 0755) &&
	    /* It's okay if the path already exists, as long as it's a
	     * directory.  */
	    !(errno == EEXIST && !lstat(path, &stbuf) && S_ISDIR(stbuf.st_mode)))
	{
		ERROR_WITH_ERRNO("Can't create directory \"%s\"", path);
		return WIMLIB_ERR_MKDIR;
	}

	return report_file_created(&ctx->common);
}

/* If @dentry represents an empty regular file or a special file, create it, set
 * its metadata, and create any needed hard links.  */
static int
unix_extract_if_empty_file(const struct wim_dentry *dentry,
			   struct unix_apply_ctx *ctx)
{
	const struct wim_inode *inode;
	struct wimlib_unix_data unix_data;
	const char *path;
	int ret;

	inode = dentry->d_inode;

	/* Extract all aliases only when the "first" comes up.  */
	if (dentry != inode_first_extraction_dentry(inode))
		return 0;

	/* Is this a directory, a symbolic link, or any type of nonempty file?
	 */
	if (inode_is_directory(inode) || inode_is_symlink(inode) ||
	    inode_unnamed_lte_resolved(inode))
		return 0;

	/* Recognize special files in UNIX_DATA mode  */
	if ((ctx->common.extract_flags & WIMLIB_EXTRACT_FLAG_UNIX_DATA) &&
	    inode_get_unix_data(inode, &unix_data) &&
	    !S_ISREG(unix_data.mode))
	{
		path = unix_build_extraction_path(dentry, ctx);
	retry_mknod:
		if (mknod(path, unix_data.mode, unix_data.rdev)) {
			if (errno == EPERM) {
				WARNING_WITH_ERRNO("Can't create special "
						   "file \"%s\"", path);
				ctx->num_special_files_ignored++;
				return 0;
			}
			if (errno == EEXIST && !unlink(path))
				goto retry_mknod;
			ERROR_WITH_ERRNO("Can't create special file \"%s\"",
					 path);
			return WIMLIB_ERR_MKNOD;
		}
		/* On special files, we can set timestamps immediately because
		 * we don't need to write any data to them.  */
		ret = unix_set_metadata(-1, inode, path, ctx);
	} else {
		int fd;

		path = unix_build_extraction_path(dentry, ctx);
	retry_create:
		fd = open(path, O_TRUNC | O_CREAT | O_WRONLY | O_NOFOLLOW, 0644);
		if (fd < 0) {
			if (errno == EEXIST && !unlink(path))
				goto retry_create;
			ERROR_WITH_ERRNO("Can't create regular file \"%s\"", path);
			return WIMLIB_ERR_OPEN;
		}
		/* On empty files, we can set timestamps immediately because we
		 * don't need to write any data to them.  */
		ret = unix_set_metadata(fd, inode, path, ctx);
		if (close(fd) && !ret) {
			ERROR_WITH_ERRNO("Error closing \"%s\"", path);
			ret = WIMLIB_ERR_WRITE;
		}
	}
	if (ret)
		return ret;

	ret = unix_create_hardlinks(inode, dentry, path, ctx);
	if (ret)
		return ret;

	return report_file_created(&ctx->common);
}

static int
unix_create_dirs_and_empty_files(const struct list_head *dentry_list,
				 struct unix_apply_ctx *ctx)
{
	const struct wim_dentry *dentry;
	int ret;

	list_for_each_entry(dentry, dentry_list, d_extraction_list_node) {
		ret = unix_create_if_directory(dentry, ctx);
		if (ret)
			return ret;
	}
	list_for_each_entry(dentry, dentry_list, d_extraction_list_node) {
		ret = unix_extract_if_empty_file(dentry, ctx);
		if (ret)
			return ret;
	}
	return 0;
}

static int
unix_create_symlink(const struct wim_inode *inode, const char *path,
		    const u8 *rpdata, u16 rpdatalen, bool rpfix,
		    const char *apply_dir, size_t apply_dir_nchars)
{
	char link_target[REPARSE_DATA_MAX_SIZE];
	int ret;
	struct wim_lookup_table_entry lte_override;

	lte_override.resource_location = RESOURCE_IN_ATTACHED_BUFFER;
	lte_override.attached_buffer = (void *)rpdata;
	lte_override.size = rpdatalen;

	ret = wim_inode_readlink(inode, link_target,
				 sizeof(link_target) - 1, &lte_override);
	if (ret < 0) {
		errno = -ret;
		return WIMLIB_ERR_READLINK;
	}

	link_target[ret] = 0;

	if (rpfix && link_target[0] == '/') {

		/* "Fix" the absolute symbolic link by prepending the absolute
		 * path to the target directory.  */

		if (sizeof(link_target) - (ret + 1) < apply_dir_nchars) {
			errno = ENAMETOOLONG;
			return WIMLIB_ERR_REPARSE_POINT_FIXUP_FAILED;
		}
		memmove(link_target + apply_dir_nchars, link_target,
			ret + 1);
		memcpy(link_target, apply_dir, apply_dir_nchars);
	}
retry_symlink:
	if (symlink(link_target, path)) {
		if (errno == EEXIST && !unlink(path))
			goto retry_symlink;
		return WIMLIB_ERR_LINK;
	}
	return 0;
}

static void
unix_cleanup_open_fds(struct unix_apply_ctx *ctx, unsigned offset)
{
	for (unsigned i = offset; i < ctx->num_open_fds; i++)
		filedes_close(&ctx->open_fds[i]);
	ctx->num_open_fds = 0;
}

static int
unix_begin_extract_stream_instance(const struct wim_lookup_table_entry *stream,
				   const struct wim_inode *inode,
				   struct unix_apply_ctx *ctx)
{
	const struct wim_dentry *first_dentry;
	const char *first_path;
	int fd;

	if (inode_is_symlink(inode)) {
		/* On UNIX, symbolic links must be created with symlink(), which
		 * requires that the full link target be available.  */
		if (stream->size > REPARSE_DATA_MAX_SIZE) {
			ERROR_WITH_ERRNO("Reparse data of \"%s\" has size "
					 "%"PRIu64" bytes (exceeds %u bytes)",
					 inode_first_full_path(inode),
					 stream->size, REPARSE_DATA_MAX_SIZE);
			return WIMLIB_ERR_INVALID_REPARSE_DATA;
		}
		ctx->reparse_ptr = ctx->reparse_data;
		return 0;
	}

	/* This should be ensured by extract_stream_list()  */
	wimlib_assert(ctx->num_open_fds < MAX_OPEN_STREAMS);

	first_dentry = inode_first_extraction_dentry(inode);
	first_path = unix_build_extraction_path(first_dentry, ctx);
retry_create:
	fd = open(first_path, O_TRUNC | O_CREAT | O_WRONLY | O_NOFOLLOW, 0644);
	if (fd < 0) {
		if (errno == EEXIST && !unlink(first_path))
			goto retry_create;
		ERROR_WITH_ERRNO("Can't create regular file \"%s\"", first_path);
		return WIMLIB_ERR_OPEN;
	}
	filedes_init(&ctx->open_fds[ctx->num_open_fds++], fd);
	return unix_create_hardlinks(inode, first_dentry, first_path, ctx);
}

/* Called when starting to read a single-instance stream for extraction  */
static int
unix_begin_extract_stream(struct wim_lookup_table_entry *stream, void *_ctx)
{
	struct unix_apply_ctx *ctx = _ctx;
	const struct stream_owner *owners = stream_owners(stream);
	int ret;

	for (u32 i = 0; i < stream->out_refcnt; i++) {
		const struct wim_inode *inode = owners[i].inode;

		ret = unix_begin_extract_stream_instance(stream, inode, ctx);
		if (ret) {
			ctx->reparse_ptr = NULL;
			unix_cleanup_open_fds(ctx, 0);
			return ret;
		}
	}
	return 0;
}

/* Called when the next chunk of a single-instance stream has been read for
 * extraction  */
static int
unix_extract_chunk(const void *chunk, size_t size, void *_ctx)
{
	struct unix_apply_ctx *ctx = _ctx;
	int ret;

	for (unsigned i = 0; i < ctx->num_open_fds; i++) {
		ret = full_write(&ctx->open_fds[i], chunk, size);
		if (ret) {
			ERROR_WITH_ERRNO("Error writing data to filesystem");
			return ret;
		}
	}
	if (ctx->reparse_ptr)
		ctx->reparse_ptr = mempcpy(ctx->reparse_ptr, chunk, size);
	return 0;
}

/* Called when a single-instance stream has been fully read for extraction  */
static int
unix_end_extract_stream(struct wim_lookup_table_entry *stream, int status,
			void *_ctx)
{
	struct unix_apply_ctx *ctx = _ctx;
	int ret;
	unsigned j;
	const struct stream_owner *owners = stream_owners(stream);

	ctx->reparse_ptr = NULL;

	if (status) {
		unix_cleanup_open_fds(ctx, 0);
		return status;
	}

	j = 0;
	ret = 0;
	for (u32 i = 0; i < stream->out_refcnt; i++) {
		struct wim_inode *inode = owners[i].inode;

		if (inode_is_symlink(inode)) {
			/* We finally have the symlink data, so we can create
			 * the symlink.  */
			const char *path;
			bool rpfix;

			rpfix = (ctx->common.extract_flags &
				 WIMLIB_EXTRACT_FLAG_RPFIX) &&
					!inode->i_not_rpfixed;

			path = unix_build_inode_extraction_path(inode, ctx);
			ret = unix_create_symlink(inode, path,
						  ctx->reparse_data,
						  stream->size,
						  rpfix,
						  ctx->target_abspath,
						  ctx->target_abspath_nchars);
			if (ret) {
				ERROR_WITH_ERRNO("Can't create symbolic link "
						 "\"%s\"", path);
				break;
			}
			ret = unix_set_metadata(-1, inode, path, ctx);
			if (ret)
				break;
		} else {
			/* Set metadata on regular file just before closing it.
			 */
			struct filedes *fd = &ctx->open_fds[j];

			ret = unix_set_metadata(fd->fd, inode, NULL, ctx);
			if (ret)
				break;

			if (filedes_close(fd)) {
				ERROR_WITH_ERRNO("Error closing \"%s\"",
						 unix_build_inode_extraction_path(inode, ctx));
				ret = WIMLIB_ERR_WRITE;
				break;
			}
			j++;
		}
	}
	unix_cleanup_open_fds(ctx, j);
	return ret;
}

static int
unix_set_dir_metadata(struct list_head *dentry_list, struct unix_apply_ctx *ctx)
{
	const struct wim_dentry *dentry;
	int ret;

	list_for_each_entry_reverse(dentry, dentry_list, d_extraction_list_node) {
		if (dentry_is_directory(dentry)) {
			ret = unix_set_metadata(-1, dentry->d_inode, NULL, ctx);
			if (ret)
				return ret;
			ret = report_file_metadata_applied(&ctx->common);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int
unix_extract(struct list_head *dentry_list, struct apply_ctx *_ctx)
{
	int ret;
	struct unix_apply_ctx *ctx = (struct unix_apply_ctx *)_ctx;
	size_t path_max;

	/* Compute the maximum path length that will be needed, then allocate
	 * some path buffers.  */
	path_max = unix_compute_path_max(dentry_list, ctx);

	for (unsigned i = 0; i < NUM_PATHBUFS; i++) {
		ctx->pathbufs[i] = MALLOC(path_max);
		if (!ctx->pathbufs[i]) {
			ret = WIMLIB_ERR_NOMEM;
			goto out;
		}
		/* Pre-fill the target in each path buffer.  We'll just append
		 * the rest of the paths after this.  */
		memcpy(ctx->pathbufs[i],
		       ctx->common.target, ctx->common.target_nchars);
	}

	/* Extract directories and empty regular files.  Directories are needed
	 * because we can't extract any other files until their directories
	 * exist.  Empty files are needed because they don't have
	 * representatives in the stream list.  */
	reset_file_progress(&ctx->common);
	ret = unix_create_dirs_and_empty_files(dentry_list, ctx);
	if (ret)
		goto out;

	/* Get full path to target if needed for absolute symlink fixups.  */
	if ((ctx->common.extract_flags & WIMLIB_EXTRACT_FLAG_RPFIX) &&
	    ctx->common.required_features.symlink_reparse_points)
	{
		ctx->target_abspath = realpath(ctx->common.target, NULL);
		if (!ctx->target_abspath) {
			ret = WIMLIB_ERR_NOMEM;
			goto out;
		}
		ctx->target_abspath_nchars = strlen(ctx->target_abspath);
	}

	/* Extract nonempty regular files and symbolic links.  */

	struct read_stream_list_callbacks cbs = {
		.begin_stream      = unix_begin_extract_stream,
		.begin_stream_ctx  = ctx,
		.consume_chunk     = unix_extract_chunk,
		.consume_chunk_ctx = ctx,
		.end_stream        = unix_end_extract_stream,
		.end_stream_ctx    = ctx,
	};
	ret = extract_stream_list(&ctx->common, &cbs);
	if (ret)
		goto out;


	/* Set directory metadata.  We do this last so that we get the right
	 * directory timestamps.  */
	reset_file_progress(&ctx->common);
	ret = unix_set_dir_metadata(dentry_list, ctx);
	if (ret)
		goto out;
	if (ctx->num_special_files_ignored) {
		WARNING("%lu special files were not extracted due to EPERM!",
			ctx->num_special_files_ignored);
	}
out:
	for (unsigned i = 0; i < NUM_PATHBUFS; i++)
		FREE(ctx->pathbufs[i]);
	FREE(ctx->target_abspath);
	return ret;
}

const struct apply_operations unix_apply_ops = {
	.name			= "UNIX",
	.get_supported_features = unix_get_supported_features,
	.extract                = unix_extract,
	.context_size           = sizeof(struct unix_apply_ctx),
};
