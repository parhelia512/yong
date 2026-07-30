#pragma once

#define L_SDF_ALPHA(c)		((c)>>24)

typedef struct{
	uint32_t *pixels;
	int width;
	int height;
	int stride;
}L_SDF_SURFACE;

typedef struct{
	uint32_t *pixels;
	int width;
	int height;
	int stride;

	float line_width;
	uint32_t bg;
	uint32_t fg;
	int x;
	int y;
}L_SDF_CONTEXT;

static inline uint32_t l_sdf_byte_mul(uint32_t c,uint32_t a)
{
	uint32_t rb = (c & 0x00FF00FF) * a;
	uint32_t ag = ((c>>8) & 0x00FF00FF) *a;
	rb=(rb+((rb>>8)&0x00FF00FF)+0x800080)&0xFF00FF00;
	ag=(ag+((ag>>8)&0x00FF00FF)+0x800080)&0xFF00FF00;
	return ag | rb>>8;
}

static inline uint32_t l_sdf_premultiply(uint32_t c)
{
	uint32_t a = L_SDF_ALPHA(c);
	uint32_t rb = (c & 0x00FF00FF) * a;
	uint32_t g  = (c & 0x0000FF00) * a;
	rb=(rb+((rb>>8)&0xFF00FF)+0x800080)&0xFF00FF00;
	g =(g +((g >>8)&0x00FF00)+0x008000)&0x00FF0000;
	return (rb >> 8) | (g>>8) | (a<<24);
}

static inline uint32_t l_sdf_premultiply_with(uint32_t c,uint32_t a)
{
	uint32_t rb = (c & 0x00FF00FF) * a;
	uint32_t g  = (c & 0x0000FF00) * a;
	rb = (rb + 0x00800080 + ((rb + 0x00800080) >> 8)) & 0xFF00FF00;
	g  = (g  + 0x00008000 + ((g  + 0x00008000) >> 8)) & 0x00FF0000;
	return (rb >> 8) | g | (a<<24);
}

int l_sdf_moveto(L_SDF_CONTEXT *ctx,int x,int y);
int l_sdf_lineto(L_SDF_CONTEXT *ctx,int x,int y);
int l_sdf_rect(L_SDF_CONTEXT *ctx,int w,int h,int r);
int l_sdf_rect_shadow(L_SDF_CONTEXT *ctx,int w,int h,int r0,int r1);

static inline void l_sdf_context_init(L_SDF_CONTEXT *ctx,void *pixels,int w,int h)
{
	ctx->pixels=pixels;
	ctx->width=w;
	ctx->height=h;
	ctx->stride=w;
	ctx->line_width=1.0f;
	ctx->x=ctx->y=0;
	ctx->bg=ctx->fg=0;
}

static inline void l_sdf_set_fg(L_SDF_CONTEXT *ctx,uint32_t c)
{
	ctx->fg=c;
}

static inline void l_sdf_set_bg(L_SDF_CONTEXT *ctx,uint32_t c)
{
	ctx->bg=c;
}

static inline void l_sdf_set_line(L_SDF_CONTEXT *ctx,float line_width)
{
	ctx->line_width=line_width;
}

static inline L_SDF_SURFACE *l_sdf_slice(L_SDF_SURFACE *surface,int x,int y,int w,int h)
{
	L_SDF_SURFACE *r=l_newa(L_SDF_SURFACE);
	r->width=w;
	r->height=h;
	r->stride=surface->stride;
	r->pixels=surface->pixels+h*r->stride+x;
	return r;
}

