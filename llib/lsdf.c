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
	SHADOW_OF_LINE,
};

typedef struct{
	struct llru_item *next;
	struct llru_item *prev;
	uint64_t key;
	uint8_t buf[];
}sdf_cache_t;

typedef union{
	uint64_t u64;
	uint32_t u32[2];
	float f32[2];
	uint16_t u16[4];
	uint8_t u8[8];
}sdf_key_t;

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

/**
 * 计算三次贝塞尔曲线上给定 x 坐标对应的 y 值 (精度 0.001, 使用 float)
 * 假设起点 P0 = (0, 1), 终点 P3 = (1, 0)
 * 
 * @param x  目标 x 坐标 (0~1)
 * @param x1 控制点 P1 的 x 坐标
 * @param y1 控制点 P1 的 y 坐标
 * @param x2 控制点 P2 的 x 坐标
 * @param y2 控制点 P2 的 y 坐标
 * @return   对应的 y 坐标
 */
float cubic_bezier_y(float x, float x1, float y1, float x2, float y2)
{
	// 边界情况快速返回
	if (x <= 0.0f) return 1.0f;
	if (x >= 1.0f) return 0.0f;

	float t = x; // 使用 x 作为 t 的初始猜测值
	float lower = 0.0f;
	float upper = 1.0f;

	// 精度要求 0.001，迭代 4 次即可满足
	for (int i = 0; i < 4; ++i)
	{
	float one_minus_t = 1.0f - t;

	// 计算当前的 X(t)
	float x_at_t = 3.0f * one_minus_t * one_minus_t * t * x1 +
		       3.0f * one_minus_t * t * t * x2 +
		       t * t * t;

	// 计算当前的导数 X'(t)
	float dx_at_t = 3.0f * one_minus_t * one_minus_t * x1 +
			6.0f * one_minus_t * t * (x2 - x1) +
			3.0f * t * t * (1.0f - x2);

	float x_diff = x_at_t - x;

	// 如果误差已经小于 0.001，提前退出
	if (fabsf(x_diff) < 1e-3f)
	    break;

	// 更新二分法的上下界
	if (x_diff > 0.0f)
	    upper = t;
	else
	    lower = t;

	// 牛顿迭代法: t_next = t - f(t)/f'(t)
	// 如果导数太小，或者牛顿迭代结果越界，退化为二分法
	if (fabsf(dx_at_t) < 1e-6f || t - x_diff / dx_at_t < lower || t - x_diff / dx_at_t > upper)
	    t = (lower + upper) * 0.5f;
	else
	    t = t - x_diff / dx_at_t;
	}

	// 使用求得的 t 计算对应的 Y(t)
	float one_minus_t = 1.0f - t;
	float y = one_minus_t * one_minus_t * one_minus_t + 
	      3.0f * one_minus_t * one_minus_t * t * y1 + 
	      3.0f * one_minus_t * t * t * y2;

	return y;
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

static inline uint8_t calc_shadow_alpha(float dist,float fr,L_SDF_SHADOW *shadow)
{
	float t=fminf(fr,dist)/fr;
	float alpha=cubic_bezier_y(t,shadow->x1,shadow->y1,shadow->x2,shadow->y2);
	return (uint8_t)(alpha * 255.0f + 0.5f);
}

static void sdf_gen_shadow_of_line(uint8_t *restrict buf,int r,L_SDF_SHADOW *shadow)
{
	const float fr=(float)r;
	for(int i=0;i<r;i++)
	{
		float dist=i+0.5f;
		buf[i]=calc_shadow_alpha(dist,fr,shadow);
	}
}

static void sdf_gen_shadow_of_rect(uint8_t *restrict buf, int r,L_SDF_SHADOW *shadow)
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
			uint8_t alpha = calc_shadow_alpha(dist,fr,shadow);
			buf[y * r + x]=alpha;
		}
	}
}

static void sdf_gen_shadow_of_rrect(uint8_t *restrict buf, int r0,int r1,L_SDF_SHADOW *shadow)
{
	int r01 = r0 + r1;
	const float fr0 = (float)r0;
	const float fr1 = (float)r1;
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
			uint8_t alpha=calc_shadow_alpha(dist,fr0,shadow);
			buf[y * r01 + x]=alpha;
		}
	}
}

static const uint8_t *sdf_cache_get(L_SDF_CONTEXT *ctx,sdf_key_t key)
{
	sdf_cache_t *item=(sdf_cache_t*)l_lru_get(lru,key.u64);
	if(item)
		return item->buf;
	uint8_t shape=key.u8[0];
	switch(shape){
		case CIRCLE:
		{
			uint8_t r=key.u8[1];
			uint32_t line_width=key.f32[1];
			item=sdf_cache_new(r*r);
			sdf_gen_circle(item->buf,r,line_width);
			break;
		}
		case DISC:
		{
			uint8_t r=key.u8[1];
			item=sdf_cache_new(r*r);
			sdf_gen_disk(item->buf,r);
			break;
		}
		case DISC_WITH_BORDER:
		{
			uint8_t r=key.u8[1];
			float line_width=key.f32[1];
			item=sdf_cache_new(r*r*2);
			sdf_gen_disk2(item->buf,r,line_width);
			break;
		}
		case SHADOW_OF_RECT:
		{
			uint8_t r=key.u8[1];
			item=sdf_cache_new(r*r);
			sdf_gen_shadow_of_rect(item->buf,r,&ctx->shadow);
			break;
		}
		case SHADOW_OF_RRECT:
		{
			uint8_t r0=key.u8[1];
			uint8_t r1=key.u8[2];
			item=sdf_cache_new((r0+r1)*(r0+r1));
			sdf_gen_shadow_of_rrect(item->buf,r0,r1,&ctx->shadow);
			break;
		}
		case SHADOW_OF_LINE:
		{
			uint8_t r=key.u8[1];
			item=sdf_cache_new(r);
			sdf_gen_shadow_of_line(item->buf,r,&ctx->shadow);
			break;
		}
		default:
			return NULL;
	}
	item->key=key.u64;
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
	int stride=ctx->stride;
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
		uint32_t *p=ctx->pixels+y*ctx->stride+x;
		for(int j=0;j<h;j++)
		{
			for(int i=0;i<w;i++)
			{
				p[i]=c+l_sdf_byte_mul(p[i],ia);
			}
			p+=ctx->stride;
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
			int b=ctx->stride*(y+i)+(x+r);
			int e=b+w-2*r-1;
			for(int j=b;j<=e;j++)
				pixels[j]=fillColor+l_sdf_byte_mul(pixels[j],ialpha);
		}
		for(int i=r;i<h-r;i++)
		{
			int b=ctx->stride*(y+i)+x;
			int e=b+w-1;
			for(int j=b;j<=e;j++)
				pixels[j]=fillColor+l_sdf_byte_mul(pixels[j],ialpha);
		}
		for(int i=h-r;i<h;i++){
			int b=ctx->stride*(y+i)+x+r;
			int e=b+w-2*r-1;
			for(int j=b;j<=e;j++)
				pixels[j]=fillColor+l_sdf_byte_mul(pixels[j],ialpha);
		}
		if(line_width==0.0f)
		{
			sdf_key_t key={.u8={DISC,(uint8_t)r}};
			const uint8_t *corner=sdf_cache_get(ctx,key);
			for(int i=0;i<r;i++)
			{
				for(int j=0;j<r;j++)
				{
					uint32_t color=l_sdf_byte_mul(fillColor,corner[i*r+j]);
					uint32_t ialpha=255-L_SDF_ALPHA(color);
					int mx=w-r+j;
					int my=h-r+i;
					int p=ctx->stride*(y+my)+(x+mx);
					uint32_t color2=color+l_sdf_byte_mul(pixels[p],ialpha);
					pixels[p]=color2;
					my=h-my-1;
					p=ctx->stride*(y+my)+(x+mx);
					color2=color+l_sdf_byte_mul(pixels[p],ialpha);
					pixels[p]=color2;
					mx=w-mx-1;
					p=ctx->stride*(y+my)+(x+mx);
					color2=color+l_sdf_byte_mul(pixels[p],ialpha);
					pixels[p]=color2;
					my=h-my-1;
					p=ctx->stride*(y+my)+(x+mx);
					color2=color+l_sdf_byte_mul(pixels[p],ialpha);
					pixels[p]=color2;
				}
			}
		}
		else
		{
			sdf_key_t key={.u8={DISC_WITH_BORDER,(uint8_t)r}};
			key.f32[1]=line_width;
			const uint8_t *corner=sdf_cache_get(ctx,key);
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
					int p=ctx->stride*(y+my)+(x+mx);
					uint32_t color2=color+l_sdf_byte_mul(pixels[p],ialpha);
					pixels[p]=color2;
					my=h-my-1;
					p=ctx->stride*(y+my)+(x+mx);
					color2=color+l_sdf_byte_mul(pixels[p],ialpha);
					pixels[p]=color2;
					mx=w-mx-1;
					p=ctx->stride*(y+my)+(x+mx);
					color2=color+l_sdf_byte_mul(pixels[p],ialpha);
					pixels[p]=color2;
					my=h-my-1;
					p=ctx->stride*(y+my)+(x+mx);
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

static inline sdf_key_t sdf_shadow_key(L_SDF_SHADOW *s,int shape,int len,int r)
{
	sdf_key_t key={
		.u8={
			shape,
				len,
			r,
			0,
			(uint8_t)(s->x1*100.0f+0.5f),
			(uint8_t)(s->y1*100.0f+0.5f),
			(uint8_t)(s->x2*100.0f+0.5f),
			(uint8_t)(s->y2*100.0f+0.5f),
		}
	};
	return key;
}

int l_sdf_rect_shadow(L_SDF_CONTEXT *ctx,int w,int h,int r0,int r1)
{
	if(r0<=0 || r1<0 || r0+r1>255)
		return -1;
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
	const uint8_t *corner=sdf_cache_get(ctx,sdf_shadow_key(&ctx->shadow,SHADOW_OF_LINE,r0,0));
	// top
	for(int j=0;j<r0;j++)
	{
		uint32_t alpha=corner[r0-j-1];
		uint32_t pcolor=l_sdf_byte_mul(color,alpha);
		alpha=L_SDF_ALPHA(pcolor);
		uint32_t ialpha=255-alpha;
		uint32_t *p=pixels+(y-r0+j)*ctx->stride+x+r1;
		for(int i=0;i<w-2*r1;i++)
		{
			*p=pcolor+l_sdf_byte_mul(*p,ialpha);
			p++;
		}
	}
	// bottom
	for(int j=0;j<r0;j++)
	{
		uint32_t alpha=corner[j];
		uint32_t pcolor=l_sdf_byte_mul(color,alpha);
		alpha=L_SDF_ALPHA(pcolor);
		uint32_t ialpha=255-alpha;
		uint32_t *p=pixels+(y+h+j)*ctx->stride+x+r1;
		for(int i=0;i<w-2*r1;i++)
		{
			*p=pcolor+l_sdf_byte_mul(*p,ialpha);
			p++;
		}
	}
	// left
	for(int j=0;j<h-2*r1;j++)
	{
		uint32_t *p=pixels+(y+r1+j)*ctx->stride+x-r0;
		for(int i=0;i<r0;i++)
		{
			uint32_t alpha=corner[r0-i-1];
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
		uint32_t *p=pixels+(y+r1+j)*ctx->stride+x+w;
		for(int i=0;i<r0;i++)
		{
			uint32_t alpha=corner[i];
			uint32_t pcolor=l_sdf_byte_mul(color,alpha);
			alpha=L_SDF_ALPHA(pcolor);
			uint32_t ialpha=255-alpha;
			*p=pcolor+l_sdf_byte_mul(*p,ialpha);
			p++;
		}
	}

	if(!r1)
	{
		corner=sdf_cache_get(ctx,sdf_shadow_key(&ctx->shadow,SHADOW_OF_RECT,r0,r1));
	}
	else
	{
		corner=sdf_cache_get(ctx,sdf_shadow_key(&ctx->shadow,SHADOW_OF_RRECT,r0,r1));
	}
	for(int i=0;i<r01;i++)
	{
		for(int j=0;j<r01;j++)
		{
			uint32_t pcolor=l_sdf_byte_mul(color,corner[i*r01+j]);
			uint32_t ialpha=255-L_SDF_ALPHA(pcolor);
			// right bottom
			int mx=w-r1+j;
			int my=h-r1+i;
			int p=ctx->stride*(y+my)+(x+mx);
			pixels[p]=pcolor+l_sdf_byte_mul(pixels[p],ialpha);
			// left bottom
			my=h-my-1;
			p=ctx->stride*(y+my)+(x+mx);
			pixels[p]=pcolor+l_sdf_byte_mul(pixels[p],ialpha);;
			// left top
			mx=w-mx-1;
			p=ctx->stride*(y+my)+(x+mx);
			pixels[p]=pcolor+l_sdf_byte_mul(pixels[p],ialpha);;
			// right top
			my=h-my-1;
			p=ctx->stride*(y+my)+(x+mx);
			pixels[p]=pcolor+l_sdf_byte_mul(pixels[p],ialpha);;
		}
	}

	return 0;
}

void l_sdf_context_init(L_SDF_CONTEXT *ctx,void *pixels,int w,int h)
{
	ctx->pixels=pixels;
	ctx->width=w;
	ctx->height=h;
	ctx->stride=w;
	ctx->line_width=1.0f;
	ctx->x=ctx->y=0;
	ctx->bg=ctx->fg=0;
	ctx->shadow.x1=0.0f;
	ctx->shadow.y1=1.0f;
	ctx->shadow.x2=0.5f;
	ctx->shadow.y2=0.0f;
}

