#ifndef _WIMLIB_WRITE_H
#define _WIMLIB_WRITE_H

#include "wimlib.h"
#include "wimlib/types.h"

/* Internal use only */
#define WIMLIB_WRITE_FLAG_NO_LOOKUP_TABLE		0x80000000
#define WIMLIB_WRITE_FLAG_CHECKPOINT_AFTER_XML		0x40000000
#define WIMLIB_WRITE_FLAG_HEADER_AT_END			0x20000000
#define WIMLIB_WRITE_FLAG_FILE_DESCRIPTOR		0x10000000
#define WIMLIB_WRITE_FLAG_USE_EXISTING_TOTALBYTES	0x08000000
#define WIMLIB_WRITE_FLAG_NO_METADATA			0x04000000
#define WIMLIB_WRITE_FLAG_OVERWRITE			0x02000000

/* Keep in sync with wimlib.h  */
#define WIMLIB_WRITE_MASK_PUBLIC (			  \
	WIMLIB_WRITE_FLAG_CHECK_INTEGRITY		| \
	WIMLIB_WRITE_FLAG_NO_CHECK_INTEGRITY		| \
	WIMLIB_WRITE_FLAG_PIPABLE			| \
	WIMLIB_WRITE_FLAG_NOT_PIPABLE			| \
	WIMLIB_WRITE_FLAG_RECOMPRESS			| \
	WIMLIB_WRITE_FLAG_FSYNC				| \
	WIMLIB_WRITE_FLAG_REBUILD			| \
	WIMLIB_WRITE_FLAG_SOFT_DELETE			| \
	WIMLIB_WRITE_FLAG_IGNORE_READONLY_FLAG		| \
	WIMLIB_WRITE_FLAG_SKIP_EXTERNAL_WIMS		| \
	WIMLIB_WRITE_FLAG_STREAMS_OK			| \
	WIMLIB_WRITE_FLAG_RETAIN_GUID			| \
	WIMLIB_WRITE_FLAG_PACK_STREAMS			| \
	WIMLIB_WRITE_FLAG_SEND_DONE_WITH_FILE_MESSAGES)

#if defined(HAVE_SYS_FILE_H) && defined(HAVE_FLOCK)
extern int
lock_wim_for_append(WIMStruct *wim);
extern void
unlock_wim_for_append(WIMStruct *wim);
#else
static inline int
lock_wim_for_append(WIMStruct *wim)
{
	return 0;
}
static inline void
unlock_wim_for_append(WIMStruct *wim)
{
}
#endif

struct list_head;

int
write_wim_part(WIMStruct *wim,
	       const void *path_or_fd,
	       int image,
	       int write_flags,
	       unsigned num_threads,
	       unsigned part_number,
	       unsigned total_parts,
	       struct list_head *stream_list_override,
	       const u8 *guid);

int
write_wim_resource_from_buffer(const void *buf, size_t buf_size,
			       int reshdr_flags, struct filedes *out_fd,
			       int out_ctype,
			       u32 out_chunk_size,
			       struct wim_reshdr *out_reshdr,
			       u8 *hash,
			       int write_resource_flags);

#endif /* _WIMLIB_WRITE_H */
