// 是否是从配置中读取
static bool w_workarea_from_config;
// 左上右下顺序的额外空间，单位是逻辑像素
static int w_workarea_padding[4];

static int w_workarea_init(int *workarea)
{
	if(!workarea)
	{
		return 0;
	}
	else
	{
		w_workarea_from_config=true;
		memcpy(w_workarea_padding,workarea,sizeof(w_workarea_padding));
	}
	return 0;
}

static bool get_xprop_workarea(int *x, int *y, int *w, int *h)
{
	FILE *fp = popen("xprop -root _NET_WORKAREA", "r");
	if(!fp)
		return false;

	char line[256];
	if(!fgets(line, sizeof(line), fp))
	{
		pclose(fp);
		return false;
	}
	pclose(fp);

	// Parse: _NET_WORKAREA(CARDINAL) = 0, 0, 1920, 1080
	int vx, vy, vw, vh;
	if(sscanf(line, "_NET_WORKAREA(CARDINAL) = %d, %d, %d, %d", &vx, &vy, &vw, &vh) != 4)
		return false;

	if(x) *x = vx;
	if(y) *y = vy;
	if(w) *w = vw;
	if(h) *h = vh;
	return true;
}

#ifdef GTK_MAJOR_VERSION
static int get_gtk_workarea(int *x,int *y,int *width,int *height)
{
	GdkAtom net_workarea_atom = gdk_atom_intern ("_NET_WORKAREA", TRUE);
	GdkWindow *root_window = gdk_get_default_root_window ();
	GdkAtom atom_ret;
	gint format, length;
	guint current_desktop = 0;
	guchar *data;
	int ret=-1;

	if (net_workarea_atom != GDK_NONE)
	{
		gboolean found = gdk_property_get (root_window,
			net_workarea_atom, GDK_NONE, 0, G_MAXLONG, FALSE,
			&atom_ret, &format, &length, &data);
		if (found && format == 32 && length / sizeof(glong) >= (current_desktop + 1) * 4)
		{
			*x      = ((glong*)data)[current_desktop * 4];
			*y      = ((glong*)data)[current_desktop * 4 + 1];
			*width  = ((glong*)data)[current_desktop * 4 + 2];
			*height = ((glong*)data)[current_desktop * 4 + 3];
			ret=0;
		}
		if (found)
			g_free (data);
	}
	return ret;
}
#endif

int w_workarea_update(w_output_t *output)
{
	if(!output)
		return -1;
	if(w_workarea_from_config)
		return 0;
	int x,y,w,h;
#ifdef GTK_MAJOR_VERSION
	int ret=get_gtk_workarea(&x, &y, &w, &h);
	if(ret!=0 && !get_xprop_workarea(&x, &y, &w, &h))
	{
		return -1;
	}
#else
	if(!get_xprop_workarea(&x, &y, &w, &h))
	{
		return -1;
	}
#endif
#if 0
	fprintf(stderr,"[wui-wayland] workarea %d %d %d %d scale %d\n",
			x,y,w,h,output->scale);
#endif
	int n=w_get_n_outputs();
	if(n>1)
	{
		int oh=output->height*output->scale;
		if(y+h > oh)
			h=oh - x;
		w_workarea_padding[0]=0;
		w_workarea_padding[1]=y;
		w_workarea_padding[2]=0;
		w_workarea_padding[3]=oh-y-h;
	}
	else
	{
		int ow=output->width*output->scale;
		int oh=output->height*output->scale;
		if(x+w > ow)
			w=ow-x;
		if(y+h > oh)
			h=oh - x;
		w_workarea_padding[0]=x;
		w_workarea_padding[1]=y;
		w_workarea_padding[2]=ow-x-w;
		w_workarea_padding[3]=oh-y-h;
	}
	return 0;
}

int w_workarea_fallback(int *x, int *y, int *w, int *h, w_output_t *output)
{
	if(x) *x = w_workarea_padding[0];
	if(y) *y = w_workarea_padding[1];
	if(w) *w = output->width*output->scale-w_workarea_padding[0]-w_workarea_padding[2];
	if(h) *h = output->height*output->scale-w_workarea_padding[1]-w_workarea_padding[3];
	return 0;
}

/*
 * 工作区域是所有显示器的联合区域，它是一个巨大的逻辑区域
 * X11下直接返回这个逻辑区域，但Wayland下只能再某个显示器下操作，所以返回这个显示器中的区域
 */
