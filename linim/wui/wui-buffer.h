#pragma once

#include <wayland-client.h>
#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct{
	struct wl_buffer   *buffer;
	cairo_surface_t    *surface;
	struct wl_shm_pool *pool;
	void               *data;       // mmap'd base pointer
	size_t              capacity;   // mmap size for this slot
	int                  width;
	int                  height;
	bool                busy;       // compositor is displaying this buffer
} W_BUFFER_SLOT;

typedef struct {
	struct wl_shm       *shm;
	W_BUFFER_SLOT slot[2];
	int draw_slot;                 // index (0 or 1) of current draw target
} WUI_BUFFER;

WUI_BUFFER *w_buffer_new(struct wl_shm *shm);
cairo_t *w_buffer_begin_frame(WUI_BUFFER *buf,int width,int height);
struct wl_buffer *w_buffer_end_frame(WUI_BUFFER *buf);
void w_buffer_free(WUI_BUFFER *buf);
bool w_buffer_busy(WUI_BUFFER *buf);
void w_buffer_write_to_png(WUI_BUFFER *buf,const char *file);

