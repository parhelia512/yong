#include "wui-buffer.h"
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

// ── buffer release callback (compositor is done with this buffer) ──

static void buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	W_BUFFER_SLOT *slot = (W_BUFFER_SLOT *)data;
	slot->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

// ── shared-memory file creation ──

static int create_shm_file(size_t size)
{
#ifdef __linux__
	int fd=memfd_create("wui-buffer",MFD_CLOEXEC);
	if(fd==-1)
		return -1;
	if (ftruncate(fd, (off_t)size) < 0)
	{
		close(fd);
		return -1;
	}
	return fd;
#else
	static atomic_int counter = 0;
	char name[64];

	for (int retry = 0; retry < 10; retry++)
	{
		snprintf(name, sizeof(name), "/wui-buffer-%d-%d",
			 getpid(), atomic_fetch_add(&counter, 1));

		int fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
		if (fd >= 0) {
			shm_unlink(name);

			if (ftruncate(fd, (off_t)size) < 0) {
				perror("[wui-buffer] ftruncate");
				close(fd);
				return -1;
			}
			return fd;
		}
	}

	perror("[wui-buffer] shm_open failed after retries");
	return -1;
#endif
}

// ── capacity rounding ──

static inline size_t calc_capacity(size_t size)
{
	const size_t align = 65536;
	return (size + align - 1) & ~(align - 1);
}

// ── public API ──

WUI_BUFFER *w_buffer_new(struct wl_shm *shm)
{
	if (!shm)
		return NULL;

	WUI_BUFFER *buf = (WUI_BUFFER *)calloc(1, sizeof(WUI_BUFFER));
	if (!buf)
		return NULL;

	buf->shm       = shm;
	buf->draw_slot = -1;
	return buf;
}

// ── prepare a single slot (allocate pool / wl_buffer / cairo surface) ──

static int w_buffer_slot_prepare(WUI_BUFFER *buf, W_BUFFER_SLOT *slot,
				 int width, int height)
{
	// Already the right size → reuse.
	if (slot->surface && slot->buffer &&
	    slot->width == width && slot->height == height)
		return 0;

	// ── tear down old wl_buffer and cairo surface ──
	if (slot->surface)
	{
		cairo_surface_destroy(slot->surface);
		slot->surface = NULL;
	}
	if (slot->buffer)
	{
		wl_buffer_destroy(slot->buffer);
		slot->buffer = NULL;
	}

	size_t size    = (size_t)(width * height * 4);
	size_t aligned = calc_capacity(size);

	// ── grow shm file + mmap + pool only when needed ──
	if (aligned > slot->capacity)
	{
		if (slot->pool)
		{
			wl_shm_pool_destroy(slot->pool);
			slot->pool = NULL;
		}
		if (slot->data && slot->capacity > 0)
		{
			munmap(slot->data, slot->capacity);
			slot->data     = NULL;
			slot->capacity = 0;
		}

		int fd = create_shm_file(aligned);
		if (fd < 0)
			return -1;

		void *data = mmap(NULL, aligned, PROT_READ | PROT_WRITE,
				  MAP_SHARED, fd, 0);
		if (data == MAP_FAILED)
		{
			perror("[wui-buffer] mmap");
			close(fd);
			return -1;
		}

		struct wl_shm_pool *pool = wl_shm_create_pool(buf->shm, fd,
							      (int32_t)aligned);
		if (!pool)
		{
			fprintf(stderr, "[wui-buffer] create_pool failed\n");
			munmap(data, aligned);
			close(fd);
			return -1;
		}
		close(fd);

		slot->pool     = pool;
		slot->data     = data;
		slot->capacity = aligned;
	}

	// ── create wl_buffer from the (new or existing) pool ──
	int32_t stride = width * 4;
	struct wl_buffer *wb = wl_shm_pool_create_buffer(
		slot->pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
	if (!wb)
	{
		fprintf(stderr, "[wui-buffer] wl_shm_pool_create_buffer failed\n");
		return -1;
	}

	// ── create cairo surface backed by the same memory ──
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		(unsigned char *)slot->data, CAIRO_FORMAT_ARGB32,
		width, height, stride);
	if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS)
	{
		fprintf(stderr, "[wui-buffer] cairo image surface failed\n");
		wl_buffer_destroy(wb);
		return -1;
	}

	// ── release listener ──
	wl_buffer_add_listener(wb, &buffer_listener, slot);

	// ── commit new slot state ──
	slot->width   = width;
	slot->height  = height;
	slot->buffer  = wb;
	slot->surface = cs;

	return 0;
}

bool w_buffer_busy(WUI_BUFFER *buf)
{
	return buf->slot[0].busy && buf->slot[1].busy;
}

cairo_t *w_buffer_begin_frame(WUI_BUFFER *buf, int width, int height)
{
	if (!buf)
	{
		return NULL;
	}

	// Pick a slot.  draw_slot starts at -1 – treat as slot 0 on first use.
	int slot = buf->draw_slot;
	if (slot < 0)
	{
		slot = 0;
		buf->draw_slot = slot;
	}

	// Prefer the current draw slot if it's free.
	if (buf->slot[slot].busy)
	{
		slot = 1 - slot;
		if (buf->slot[slot].busy)
		{
			return NULL;          // both held by compositor
		}
		buf->draw_slot = slot;
	}

	if (w_buffer_slot_prepare(buf, &buf->slot[slot], width, height) != 0)
	{
		return NULL;
	}

	cairo_t *cr=cairo_create(buf->slot[slot].surface);
	return cr;
}

struct wl_buffer *w_buffer_end_frame(WUI_BUFFER *buf)
{
	if (!buf || buf->draw_slot < 0)
		return NULL;

	int slot = buf->draw_slot;

	// flush cairo drawing to the underlying bitmap
	cairo_surface_flush(buf->slot[slot].surface);

	// mark busy so we won't draw into it again until released
	buf->slot[slot].busy = true;

	return buf->slot[slot].buffer;
}

void w_buffer_free(WUI_BUFFER *buf)
{
	if (!buf)
		return;

	for (int i = 0; i < 2; i++)
	{
		if (buf->slot[i].surface)
			cairo_surface_destroy(buf->slot[i].surface);
		if (buf->slot[i].buffer)
			wl_buffer_destroy(buf->slot[i].buffer);
		if (buf->slot[i].pool)
			wl_shm_pool_destroy(buf->slot[i].pool);
		if (buf->slot[i].data && buf->slot[i].capacity > 0)
			munmap(buf->slot[i].data, buf->slot[i].capacity);
	}

	free(buf);
}

void w_buffer_write_to_png(WUI_BUFFER *buf,const char *file)
{
	int slot = buf->draw_slot;
	cairo_surface_t *surface=buf->slot[slot].surface;
	cairo_surface_write_to_png(surface,file);
}
