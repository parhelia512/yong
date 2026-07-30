#include "llib.h"
#include "llru.h"
#include "lsdf.h"

#include <math.h>

enum{
	CIRCLE,
	DISC,
	DISC_WITH_BORDER,
	SHADOW_OF_RECT,
	SHADOW_OF_RRECT,
};

#define SHADOW_LINEAR		0		// 线性衰减
#define SHADOW_SQUARE		1		// 平方衰减
#define SHADOW_SMOOTHSTEP	2		// Smoothstep 平滑曲线

#define USE_SHADOW			1

typedef struct{
	struct llru_item *next;
	struct llru_item *prev;
	uint64_t key;
	uint8_t buf[];
}sdf_cache_t;

static inline uint64_t get_key(uint8_t shape,uint8_t p0,uint16_t p1,float p2)
{
	union{
		float f;
		uint32_t i;
	}u={.f=p2};
	return (((uint64_t)shape)<<56)|(((uint64_t)p0)<<48)|(((uint64_t)p1)<<32)|u.i;
}

static LLRU *lru;
[[gnu::constructor]]
static void sdf_cache_init(void)
{
	lru=l_lru_new(l_free);
}

static sdf_cache_t *sdf_cache_new(int size)
{
	return l_alloc0(sizeof(sdf_cache_t)+size);
}

static void sdf_gen_circle(uint8_t *restrict buf, int r, float line_width)
{
    const float fr     = (float)r;

    for (int y = 0; y < r; y++)
	{
		float py = y + 0.5f;
		float py2 = py*py;
        for (int x = 0; x < r; x++)
		{
			float px = x + 0.5f;
            /* ---- 计算像素中心到弧线的有符号距离 ---- */
            float dist;  // >0 在弧线外侧, <0 在弧线内侧
			dist = sqrtf(px*px + py2) - fr;

            /* ---- 描边带的 SDF ---- */
            /* 外边界: dist > 0 → 在弧线外侧 (透明)        */
            /* 内边界: dist < -line_width → 越过内缘 (透明) */
            float d_out = dist;                  // >0 外缘之外
            float d_in  = -line_width - dist;    // >0 内缘之内(空洞侧)
            float sdf   = fmaxf(d_out, d_in);    // >0 在描边带外

            /* ---- SDF → 透明度 (1px 线性抗锯齿) ---- */
            float alpha = 0.5f - sdf;
			alpha = fmaxf(0.0f, alpha);
			alpha = fminf(1.0f, alpha);

            buf[y * r + x] = (uint8_t)(alpha * 255.0f + 0.5f);
        }
    }
}

static void sdf_gen_disk(uint8_t *restrict buf, int r)
{
	const float fr     = (float)r;

    for (int y = 0; y < r; y++)
	{
		float py = y + 0.5f;
		float py2 = py*py;
        for (int x = 0; x < r; x++)
		{
			float px = x + 0.5f;
            /* ---- 计算像素中心到弧线的有符号距离 ---- */
            float dist;  // >0 在弧线外侧, <0 在弧线内侧
			dist = sqrtf(px*px + py2) - fr;

			float alpha = 0.5f - dist;
			alpha = fmaxf(0.0f, alpha);
			alpha = fminf(1.0f, alpha);

            buf[y * r + x] = (uint8_t)(alpha * 255.0f + 0.5f);
        }
    }
}

static void sdf_gen_disk2(uint8_t *restrict buf, int r,float line_width)
{
	const float fr = (float)r;

    for (int y = 0; y < r; y++)
	{
		float py = y + 0.5f;
		float py2 = py*py;
        for (int x = 0; x < r; x++)
		{
			float px = x + 0.5f;
            /* ---- 计算像素中心到弧线的有符号距离 ---- */
            float dist;  // >0 在弧线外侧, <0 在弧线内侧
			dist = sqrtf(px*px + py2) - fr;

			// 2. 计算外边缘透明度
            float alpha = 0.5f - dist;
            alpha = fmaxf(0.0f, alpha);
            alpha = fminf(1.0f, alpha);

			// 3. 计算内边缘颜色过渡
            float t = dist + line_width + 0.5f;
            t = fmaxf(0.0f, t);
            t = fminf(1.0f, t);

            buf[(y * r + x)*2+0] = (uint8_t)(t * 255.0f + 0.5f);
            buf[(y * r + x)*2+1] = (uint8_t)(alpha * 255.0f + 0.5f);
        }
    }
}

static inline uint8_t calc_shadow_alpha(float dist,float fr)
{
#if USE_SHADOW==SHADOW_LINEAR
	float t=fminf(fr,dist);
	float alpha=1.0f-t/fr;
#endif

#if USE_SHADOW==SHADOW_SQUARE
	float t = fminf(dist / fr, 1.0f);
	float alpha = 1.0f - t;
	alpha = alpha * alpha;
#endif

#if USE_SHADOW==SHADOW_SMOOTHSTEP
	float t=fminf(dist/fr,1.0f);
	float alpha = 1.0f - (t * t * (3.0f - 2.0f * t));
#endif
	return (uint8_t)(alpha * 255.0f + 0.5f);
}

static void sdf_gen_shadow_of_rect(uint8_t *restrict buf, int r)
{
	const float fr = (float)r;
	for (int y = 0; y < r; y++)
	{
		float py = y + 0.5f;
		float py2 = py*py;
		for (int x = 0; x < r; x++)
		{
			float px = x + 0.5f;

			float dist = sqrtf(px*px + py2);
			uint8_t alpha = calc_shadow_alpha(dist,fr);
			buf[y * r + x]=alpha;
		}
	}
}

static void sdf_gen_shadow_of_rrect(uint8_t *restrict buf, int r0,int r1)
{
	int r01=r0+r1;
	const float fr0 = (float)r0;
	const float fr1= (float)r1;
	for (int y = 0; y < r01; y++)
	{
		float py = y + 0.5f;
		float py2 = py*py;
		for (int x = 0; x < r01; x++)
		{
			float px = x + 0.5f;
			float dist = sqrtf(px*px + py2)-fr1;
			if(dist<=0)
				continue;
			dist=fminf(dist,fr0);
			uint8_t alpha=calc_shadow_alpha(dist,fr0);
			buf[y * r01 + x]=alpha;
		}
	}
}

static const uint8_t *sdf_cache_get(uint8_t shape,uint8_t p0,uint16_t p1,float p2)
{
	uint64_t key=get_key(shape,p0,p1,p2);
	sdf_cache_t *item=(sdf_cache_t*)l_lru_get(lru,key);
	if(item)
		return item->buf;
	switch(shape){
		case CIRCLE:
			item=sdf_cache_new(p0*p0);
			sdf_gen_circle(item->buf,p0,p2);
			break;
		case DISC:
			item=sdf_cache_new(p0*p0);
			sdf_gen_disk(item->buf,p0);
			break;
		case DISC_WITH_BORDER:
			item=sdf_cache_new(p0*p0*2);
			sdf_gen_disk2(item->buf,p0,p2);
			break;
		case SHADOW_OF_RECT:
			item=sdf_cache_new(p0*p0);
			sdf_gen_shadow_of_rect(item->buf,p0);
			break;
		case SHADOW_OF_RRECT:
			item=sdf_cache_new((p0+p1)*(p0+p1));
			sdf_gen_shadow_of_rrect(item->buf,p0,p1);
			break;
		default:
			return NULL;
	}
	item->key=key;
	l_lru_add(lru,(LLRU_ITEM*)item);
	return item->buf;
}

int l_sdf_moveto(L_SDF_CONTEXT *ctx,int x,int y)
{
	if(x<0 || x>=ctx->width)
		return -1;
	if(y<0 || y>=ctx->height)
		return -1;
	ctx->x=x;
	ctx->y=y;
	return 0;
}

static void l_sdf_vline1(L_SDF_CONTEXT *ctx,int x,int y0,int y1,uint32_t c)
{
	int stride=ctx->width;
	uint32_t *p=ctx->pixels+y0*stride+x;
	uint32_t a=L_SDF_ALPHA(c);
	uint32_t ia=255-a;
	for(int i=y0;i<=y1;i++)
	{
		*p=c+l_sdf_byte_mul(*p,ia);
		p+=stride;
	}
}

static void l_sdf_hline1(L_SDF_CONTEXT *ctx,int y,int x0,int x1,uint32_t c)
{
	uint32_t *p=ctx->pixels+y*ctx->width+x0;
	uint32_t a=L_SDF_ALPHA(c);
	uint32_t ia=255-a;
	for(int i=x0;i<=x1;i++)
	{
		*p=c+l_sdf_byte_mul(*p,ia);
		p++;
	}
}

int l_sdf_lineto(L_SDF_CONTEXT *ctx,int x,int y)
{
	if(x<0 || x>=ctx->width)
		return -1;
	if(y<0 || y>=ctx->height)
		return -1;
	float line_width=ctx->line_width;
	if(line_width<=0)
		return -1;
	uint32_t color=l_sdf_premultiply(ctx->fg);
	if(x==ctx->x)
	{
		int y0=MIN(ctx->y,y);
		int y1=MAX(ctx->y,y);
		int step=y<y1?1:-1;
		do{
			if(line_width<1.0f)
			{
				uint8_t a=255.0f*line_width;
				color=l_sdf_byte_mul(color,a);
			}
			l_sdf_vline1(ctx,x,y0,y1,color);
			x+=step;
			if(x<0 || x>=ctx->width)
				break;
			line_width-=1.0f;
		}while(line_width>0);
	}
	else if(y==ctx->y)
	{
		int x0=MIN(ctx->x,x);
		int x1=MAX(ctx->x,x);
		int step=x<x1?-1:1;
		do{
			if(line_width<1)
			{
				uint8_t a=255.0f*line_width;
				color=l_sdf_byte_mul(color,a);
			}
			l_sdf_hline1(ctx,y,x0,x1,color);
			y+=step;
			if(y<0 || y>=ctx->height)
				break;
			line_width-=1.0f;
		}while(line_width>0);

	}
	ctx->x=x;
	ctx->y=y;
	return 0;
}

static void l_sdf_rect1(L_SDF_CONTEXT *ctx,int x,int y,int w,int h,uint32_t c)
{
	l_sdf_hline1(ctx,y,x,x+w-1,c);
	l_sdf_hline1(ctx,y+h-1,x,x+w-1,c);
	l_sdf_vline1(ctx,x,y+1,y+h-2,c);
	l_sdf_vline1(ctx,x+w-1,y+1,y+h-2,c);
}

static int l_sdf_rect_r0(L_SDF_CONTEXT *ctx,int w,int h)
{
	int x=ctx->x;
	int y=ctx->y;
	if(w<=0 || h<=0 || x+w>ctx->width || y+h>ctx->height)
		return -1;
	uint32_t c=l_sdf_premultiply(ctx->bg);
	uint32_t a=L_SDF_ALPHA(c);
	if(a!=0)
	{
		uint32_t ia=255-a;
		uint32_t *p=ctx->pixels+y*ctx->width+x;
		for(int j=0;j<h;j++)
		{
			for(int i=0;i<w;i++)
			{
				p[i]=c+l_sdf_byte_mul(p[i],ia);
			}
			p+=ctx->width;
		}
	}
	float line_width=ctx->line_width;
	if(!line_width)
		return 0;
	if(ceilf(line_width)>w || ceilf(line_width)>h)
		return -1;
	c=l_sdf_premultiply(ctx->fg);
	if(L_SDF_ALPHA(c)==0)
		return 0;
	do{
		if(line_width<1)
		{
			uint8_t a=255.0f*line_width;
			c=l_sdf_byte_mul(c,a);
		}
		l_sdf_rect1(ctx,x,y,w,h,c);
		x++;y++;
		w-=2;h-=2;
		line_width-=1.0f;
	}while(line_width>0);
	return 0;
}

int l_sdf_rect(L_SDF_CONTEXT *ctx,int w,int h,int r)
{
	int x=ctx->x;
	int y=ctx->y;
	if(w<=0 || h<=0 || x+w>ctx->width || y+h>ctx->height)
	{
		return -1;
	}
	if(r<0 || r>255)
	{
		return -2;
	}
	if(r==0 || w==1 || h==1)
		return l_sdf_rect_r0(ctx,w,h);
	if(r > w/2)
		r = w/2;
	if(r > h/2)
		r = h/2;
	uint32_t fillColor=l_sdf_premultiply(ctx->bg);
	uint32_t fa=L_SDF_ALPHA(fillColor);
	uint32_t strokeColor=l_sdf_premultiply(ctx->fg);
	float line_width=L_SDF_ALPHA(strokeColor)?ctx->line_width:0;
	uint32_t *pixels=ctx->pixels;
	if(fa)
	{
		uint32_t ialpha=255-fa;
		for(int i=0;i<r;i++)
		{
			int b=ctx->width*(y+i)+(x+r);
			int e=b+w-2*r-1;
			for(int j=b;j<=e;j++)
				pixels[j]=fillColor+l_sdf_byte_mul(pixels[j],ialpha);
		}
		for(int i=r;i<h-r;i++)
		{
			int b=ctx->width*(y+i)+x;
			int e=b+w-1;
			for(int j=b;j<=e;j++)
				pixels[j]=fillColor+l_sdf_byte_mul(pixels[j],ialpha);
		}
		for(int i=h-r;i<h;i++){
			int b=ctx->width*(y+i)+x+r;
			int e=b+w-2*r-1;
			for(int j=b;j<=e;j++)
				pixels[j]=fillColor+l_sdf_byte_mul(pixels[j],ialpha);
		}
		if(line_width==0.0f)
		{
			const uint8_t *corner=sdf_cache_get(DISC,r,0,0.0f);
			for(int i=0;i<r;i++)
			{
				for(int j=0;j<r;j++)
				{
					uint32_t color=l_sdf_byte_mul(fillColor,corner[i*r+j]);
					uint32_t ialpha=255-L_SDF_ALPHA(color);
					int mx=w-r+j;
					int my=h-r+i;
					int p=ctx->width*(y+my)+(x+mx);
					uint32_t color2=color+l_sdf_byte_mul(pixels[p],ialpha);
					pixels[p]=color2;
					my=h-my-1;
					p=ctx->width*(y+my)+(x+mx);
					color2=color+l_sdf_byte_mul(pixels[p],ialpha);
					pixels[p]=color2;
					mx=w-mx-1;
					p=ctx->width*(y+my)+(x+mx);
					color2=color+l_sdf_byte_mul(pixels[p],ialpha);
					pixels[p]=color2;
					my=h-my-1;
					p=ctx->width*(y+my)+(x+mx);
					color2=color+l_sdf_byte_mul(pixels[p],ialpha);
					pixels[p]=color2;
				}
			}
		}
		else
		{
			const uint8_t *corner=sdf_cache_get(DISC_WITH_BORDER,r,0,line_width);
			for(int i=0;i<r;i++)
			{
				for(int j=0;j<r;j++)
				{
					uint8_t A0=corner[(i*r+j)*2+0];
					uint8_t A1=corner[(i*r+j)*2+1];
					uint32_t color=l_sdf_byte_mul(strokeColor,A0);
					A0=L_SDF_ALPHA(color);

					color=color+l_sdf_byte_mul(fillColor,255-A0);
					color=l_sdf_byte_mul(color,A1);
					A1=L_SDF_ALPHA(color);

					uint32_t ialpha=255-A1;
					int mx=w-r+j;
					int my=h-r+i;
					int p=ctx->width*(y+my)+(x+mx);
					uint32_t color2=color+l_sdf_byte_mul(pixels[p],ialpha);
					pixels[p]=color2;
					my=h-my-1;
					p=ctx->width*(y+my)+(x+mx);
					color2=color+l_sdf_byte_mul(pixels[p],ialpha);
					pixels[p]=color2;
					mx=w-mx-1;
					p=ctx->width*(y+my)+(x+mx);
					color2=color+l_sdf_byte_mul(pixels[p],ialpha);
					pixels[p]=color2;
					my=h-my-1;
					p=ctx->width*(y+my)+(x+mx);
					color2=color+l_sdf_byte_mul(pixels[p],ialpha);
					pixels[p]=color2;
				}
			}
		}
		
	}
	if(line_width)
	{
		// top
		l_sdf_moveto(ctx,x+r,y+0);
		l_sdf_lineto(ctx,x+w-r-1,y+0);
		// right
		l_sdf_moveto(ctx,x+w-1,y+r);
		l_sdf_lineto(ctx,x+w-1,y+h-r-1);
		// bottom
		l_sdf_moveto(ctx,x+w-r-1,y+h-1);
		l_sdf_lineto(ctx,x+r,y+h-1);
		// left
		l_sdf_moveto(ctx,x,y+h-r-1);
		l_sdf_lineto(ctx,x,y+r);
	}
	return 0;
}

int l_sdf_rect_shadow(L_SDF_CONTEXT *ctx,int w,int h,int r0,int r1)
{
	if(r0<=0 || r1<0 || r0+r1>255)
		return -1;
	float fr0=r0;
	int r01=r0+r1;
	int x=ctx->x;
	int y=ctx->y;
	if(x-r0<0 || x+w+r0>ctx->width)
	{
		return -1;
	}
	if(y-r0<0 || y+h+r0>ctx->height)
	{
		return -1;
	}
	if(r1>ctx->width/2 || r1>ctx->height/2)
	{
		return -1;
	}
	uint32_t color=l_sdf_premultiply(ctx->fg);
	uint32_t *pixels=ctx->pixels;
	// top
	for(int j=0;j<r0;j++)
	{
		float dist=fr0-j-0.5f;
		uint32_t alpha=calc_shadow_alpha(dist,fr0);
		uint32_t pcolor=l_sdf_byte_mul(color,alpha);
		alpha=L_SDF_ALPHA(pcolor);
		uint32_t ialpha=255-alpha;
		uint32_t *p=pixels+(y-r0+j)*ctx->width+x+r1;
		for(int i=0;i<w-2*r1;i++)
		{
			*p=pcolor+l_sdf_byte_mul(*p,ialpha);
			p++;
		}
	}
	// bottom
	for(int j=0;j<r0;j++)
	{
		float dist=j+0.5f;
		uint32_t alpha=calc_shadow_alpha(dist,fr0);
		uint32_t pcolor=l_sdf_byte_mul(color,alpha);
		alpha=L_SDF_ALPHA(pcolor);
		uint32_t ialpha=255-alpha;
		uint32_t *p=pixels+(y+h+j)*ctx->width+x+r1;
		for(int i=0;i<w-2*r1;i++)
		{
			*p=pcolor+l_sdf_byte_mul(*p,ialpha);
			p++;
		}
	}
	// left
	for(int j=0;j<h-2*r1;j++)
	{
		uint32_t *p=pixels+(y+r1+j)*ctx->width+x-r0;
		for(int i=0;i<r0;i++)
		{
			float dist=fr0-i-0.5f;
			uint32_t alpha=calc_shadow_alpha(dist,fr0);
			uint32_t pcolor=l_sdf_byte_mul(color,alpha);
			alpha=L_SDF_ALPHA(pcolor);
			uint32_t ialpha=255-alpha;
			*p=pcolor+l_sdf_byte_mul(*p,ialpha);
			p++;
		}
	}
	// right
	for(int j=0;j<h-2*r1;j++)
	{
		uint32_t *p=pixels+(y+r1+j)*ctx->width+x+w;
		for(int i=0;i<r0;i++)
		{
			// distance=i+0.5 alpha=255*(r0-distance)/r0=255*(r0-i-0.5)/r0
			float dist=i+0.5f;
			uint32_t alpha=calc_shadow_alpha(dist,fr0);
			uint32_t pcolor=l_sdf_byte_mul(color,alpha);
			alpha=L_SDF_ALPHA(pcolor);
			uint32_t ialpha=255-alpha;
			*p=pcolor+l_sdf_byte_mul(*p,ialpha);
			p++;
		}
	}

	const uint8_t *corner;
	if(!r1)
		corner=sdf_cache_get(SHADOW_OF_RECT,r0,0,0);
	else
		corner=sdf_cache_get(SHADOW_OF_RRECT,r0,r1,0);
	for(int i=0;i<r01;i++)
	{
		for(int j=0;j<r01;j++)
		{
			uint32_t pcolor=l_sdf_byte_mul(color,corner[i*r01+j]);
			uint32_t ialpha=255-L_SDF_ALPHA(pcolor);
			// right bottom
			int mx=w-r1+j;
			int my=h-r1+i;
			int p=ctx->width*(y+my)+(x+mx);
			pixels[p]=pcolor+l_sdf_byte_mul(pixels[p],ialpha);
			// left bottom
			my=h-my-1;
			p=ctx->width*(y+my)+(x+mx);
			pixels[p]=pcolor+l_sdf_byte_mul(pixels[p],ialpha);;
			// left top
			mx=w-mx-1;
			p=ctx->width*(y+my)+(x+mx);
			pixels[p]=pcolor+l_sdf_byte_mul(pixels[p],ialpha);;
			// right top
			my=h-my-1;
			p=ctx->width*(y+my)+(x+mx);
			pixels[p]=pcolor+l_sdf_byte_mul(pixels[p],ialpha);;
		}
	}

	return 0;
}
