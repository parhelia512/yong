#if GTK_MAJOR_VERSION

#include "llib.h"
#include <string.h>

static GtkWidget *toast_win = NULL;
static GtkWidget *toast_label = NULL;
static guint toast_timeout_id = 0;

static void toast_cleanup(void)
{
	toast_label = NULL;
	if(toast_timeout_id)
	{
		g_source_remove(toast_timeout_id);
		toast_timeout_id = 0;
	}
}

static gboolean toast_timeout_hide(gpointer data)
{
	gtk_widget_hide(toast_win);
	toast_timeout_id = 0;
	return G_SOURCE_REMOVE;
}

static void adjust_toast_win_pos(GtkWidget *w)
{
	int ret,x,y;
	if(!w_input_hook || !w_input_hook->get_pos)
	{
		gtk_window_set_position(GTK_WINDOW(w), GTK_WIN_POS_CENTER);
		return;
	}
	ret=w_input_hook->get_pos(&x,&y);
	if(ret<0 || x<=0 || y<=0 || ret==1)
	{
		gtk_window_set_position(GTK_WINDOW(w), GTK_WIN_POS_CENTER);
		return;
	}
	gtk_window_move(GTK_WINDOW(w), x, y);
}

int w_toast(const char *s)
{
	if(!s || !*s)
		return -1;

	// gdk_display is a static in wui-gtk3-x11.c
	if(!gdk_display)
		return -1;

	if(toast_win)
	{
		// append to label
		// if(toast_timeout_id==0)
		{
			gtk_label_set_text(GTK_LABEL(toast_label), s);
		}
#if 0
		else
		{
			const gchar *prev=gtk_label_get_text(GTK_LABEL(toast_label));
			char *temp=l_sprintf("%s\n%s",prev,s);
			gtk_label_set_text(GTK_LABEL(toast_label), temp);
			l_free(temp);
		}
#endif
		adjust_toast_win_pos(toast_win);
		gtk_widget_show_all(toast_win);
	}
	else
	{
		// Create cached toast window
		toast_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
		if(!toast_win)
			return -1;

		gtk_window_set_decorated(GTK_WINDOW(toast_win), FALSE);
		gtk_window_set_skip_taskbar_hint(GTK_WINDOW(toast_win), TRUE);
		gtk_window_set_skip_pager_hint(GTK_WINDOW(toast_win), TRUE);
		gtk_window_set_keep_above(GTK_WINDOW(toast_win), TRUE);
		gtk_window_set_accept_focus(GTK_WINDOW(toast_win), FALSE);
		gtk_window_set_resizable(GTK_WINDOW(toast_win), FALSE);
		gtk_window_set_type_hint(GTK_WINDOW(toast_win), GDK_WINDOW_TYPE_HINT_NOTIFICATION);

		// Enable RGBA visual for translucent background
		GdkScreen *screen = gdk_screen_get_default();
		GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
		if(visual)
			gtk_widget_set_visual(toast_win, visual);

		// Create the message label
		toast_label = gtk_label_new(s);
		gtk_label_set_max_width_chars(GTK_LABEL(toast_label), 50);
		gtk_label_set_line_wrap(GTK_LABEL(toast_label), TRUE);
		gtk_misc_set_padding(GTK_MISC(toast_label), 20, 12);
		gtk_container_add(GTK_CONTAINER(toast_win), toast_label);

		// Apply toast styling via CSS
		GtkCssProvider *provider = gtk_css_provider_new();
		const char *css =
			"window {"
			"	background-color: rgba(50,50,50,0.93);"
			"	border-radius: 8px;"
			"}"
			"label {"
			"	color: #ffffff;"
			"	font-size: 14px;"
			"}";
		GError *err = NULL;
		gtk_css_provider_load_from_data(provider, css, -1, &err);
		if(err)
		{
			g_warning("toast CSS: %s", err->message);
			g_error_free(err);
		}
		GtkStyleContext *ctx = gtk_widget_get_style_context(toast_win);
		gtk_style_context_add_provider(ctx, GTK_STYLE_PROVIDER(provider),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
		g_object_unref(provider);

		adjust_toast_win_pos(toast_win);

		// On external destroy: clear cached pointer so we recreate next time
		g_signal_connect(toast_win, "destroy", G_CALLBACK(gtk_widget_destroyed), &toast_win);
		g_signal_connect(toast_win, "destroy", G_CALLBACK(toast_cleanup), NULL);

		gtk_widget_show_all(toast_win);
	}

	// Reset auto-hide timeout
	if(toast_timeout_id)
		g_source_remove(toast_timeout_id);
	toast_timeout_id = g_timeout_add_seconds(1, toast_timeout_hide, NULL);

	return 0;
}

#elif defined(WUI_BACKEND_WAYLAND)

#include <string.h>
#include <math.h>

// ── cached toast state ──

static w_win_t toast_win_layer = NULL;
static w_win_t toast_win_input = NULL;
static char *toast_text = NULL;
static guint toast_timeout_id = 0;

// ── draw callback: renders text via the W_DRAW event ──

static int toast_draw_cb(w_win_t win, const W_EVENT *e, void *data)
{
	const char *text = (const char *)data;
	if(!text)
		return 0;

	cairo_t *cr = e->draw.cr;
	int w, h;
	w_win_get_size(win, &w, &h);

	// Dark rounded background
	cairo_set_source_rgba(cr, 0.2, 0.2, 0.2, 0.93);
	double r = 8;
	cairo_new_path(cr);
	cairo_arc(cr, r, r, r, M_PI, 3 * M_PI / 2);
	cairo_arc(cr, w - r, r, r, 3 * M_PI / 2, 0);
	cairo_arc(cr, w - r, h - r, r, 0, M_PI / 2);
	cairo_arc(cr, r, h - r, r, M_PI / 2, M_PI);
	cairo_close_path(cr);
	cairo_fill(cr);

	// White text
	PangoLayout *layout = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(layout, font_desc);
	pango_layout_set_text(layout, text, -1);
	int text_w, text_h;
	pango_layout_get_pixel_size(layout, &text_w, &text_h);
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	cairo_move_to(cr, (w-text_w)/2, (h-text_h)/2);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);

	return 0;
}

// ── destroy callback: clear cache when window is destroyed externally ──

static int toast_destroy_cb(w_win_t win, const W_EVENT *e, void *data)
{
	(void)e;
	(void)data;
	if(toast_timeout_id)
	{
		g_source_remove(toast_timeout_id);
		toast_timeout_id = 0;
	}
	l_free(toast_text);
	toast_text = NULL;
	if(win==toast_win_input)
		toast_win_input = NULL;
	if(win==toast_win_layer)
		toast_win_layer = NULL;	
	return 0;
}

// ── timeout hide ──

static gboolean toast_timeout_hide(gpointer data)
{
	w_win_t win=data;
	if(win)
	{
		if(win->wl_info.role_type!=W_ROLE_DEFAULT)
		{
			w_win_hide(win);
		}
		else
		{
			w_win_destroy(win);
		}
	}
	toast_timeout_id = 0;
	return G_SOURCE_REMOVE;
}

// ── public API ──

int w_toast(const char *s)
{
	if(!s || !*s)
		return -1;
	if(!wl_display)
		return -1;

	// Measure text with Pango to determine window size
	int pad = 16;
	int max_w = 380;

	cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(tmp);
	PangoLayout *layout = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(layout, font_desc);
	pango_layout_set_text(layout, s, -1);
	pango_layout_set_width(layout, (max_w - 2 * pad) * PANGO_SCALE);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	int text_w, text_h;
	pango_layout_get_pixel_size(layout, &text_w, &text_h);
	g_object_unref(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(tmp);

	int w = text_w + 2 * pad;
	int h = text_h + 2 * pad;

	int ret=-1;
	int x,y;
	if(w_input_hook && w_input_hook->get_pos)
		ret=w_input_hook->get_pos(&x,&y);

	w_win_t toast_win;

	if(ret!=2)
	{
		if(!toast_win_layer)
		{
			// Choose role based on layer-shell availability
			bool use_layer = w_wayland_has_interface("zwlr_layer_shell_v1", 1, NULL);
			const char *role = use_layer ? "layer" : "toplevel";

			toast_win_layer = w_win_create("", role, NULL);
			if(!toast_win_layer)
				return -1;

			w_win_connect(toast_win_layer, W_DESTROY, toast_destroy_cb, NULL);
		}
		toast_win=toast_win_layer;

		if(toast_win->wl_info.role_type==W_ROLE_LAYER)
		{
			if(ret<0 || x<=0 || y<=0 || ret==1)
			{
				toast_win->wl_info.layer.anchor=0;
			}
			else
			{
				toast_win->wl_info.layer.anchor=ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP|ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
				w_win_move(toast_win,x,y);
			}
		}
	}
	else
	{
		if(!toast_win_input)
		{
			toast_win_input=w_win_create("","input",NULL);
			if(!toast_win_input)
				return -1;
			w_win_connect(toast_win_input, W_DESTROY, toast_destroy_cb, NULL);
		}
		toast_win=toast_win_input;
	}
	w_win_preferred_size(toast_win,&w,&h);

	// Update text
	l_free(toast_text);
	toast_text = l_strdup(s);
	w_win_disconnect(toast_win, W_DRAW);
	w_win_connect(toast_win, W_DRAW, toast_draw_cb, toast_text);

	int toast_w,toast_h;
	w_win_get_size(toast_win,&toast_w,&toast_h);

	// Resize if text dimensions changed
	if(w != toast_w || h != toast_h)
	{
		w_win_resize(toast_win, w, h);
		toast_w = w;
		toast_h = h;
	}

	// Re-show and redraw
	w_win_show(toast_win);
	w_win_redraw(toast_win);

	// Reset auto-hide timeout
	if(toast_timeout_id)
		g_source_remove(toast_timeout_id);
	toast_timeout_id = g_timeout_add_seconds(1, toast_timeout_hide, toast_win);

	return 0;
}

#else

int w_toast(const char *s)
{
	(void)s;
	return -1;
}

#endif
