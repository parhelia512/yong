// Wayland alert dialog (non-blocking)
// Uses xdg-toplevel with Pango/cairo drawing, cached window, auto-dismiss on click/key/close

// Style parameters — global so callers can customize
typedef struct {
	uint8_t bg_color[4];       // window background RGBA
	uint8_t fg_color[4];       // message text RGBA
	uint8_t btn_bg[4];         // OK button fill RGBA
	uint8_t btn_bg_hover[4];   // OK button hover fill RGBA
	uint8_t btn_bg_press[4];   // OK button pressed fill RGBA
	uint8_t btn_border[4];     // OK button border RGBA
	uint8_t btn_fg[4];         // OK button text RGBA
	int padding;               // inner margin
	int btn_w;                 // OK button width
	int btn_h;                 // OK button height
	int btn_margin;            // space between text area and button
	int min_w;                 // minimum window width
	int max_w;                 // maximum window width (text wraps beyond this)
} AlertStyle;

static AlertStyle alert_style = {
	.bg_color    = {255, 255, 255, 255},
	.fg_color    = {  0,   0,   0, 255},
	.btn_bg      = {230, 230, 230, 255},
	.btn_bg_hover= {200, 200, 200, 255},
	.btn_bg_press= {170, 170, 170, 255},
	.btn_border  = {128, 128, 128, 255},
	.btn_fg      = {  0,   0,   0, 255},
	.padding     = 16,
	.btn_w       = 80,
	.btn_h       = 28,
	.btn_margin  = 12,
	.min_w       = 300,
	.max_w       = 500,
};

typedef struct {
	char *text;           // owned copy of message text
	const char *btn_text; // OK button label (static, not owned)
	int btn_x, btn_y;     // OK button position (updated on draw)
	int btn_w, btn_h;     // OK button size
	bool hovered;         // mouse hovering over button
	bool pressed;         // mouse button held on button
} AlertData;

static w_win_t alert_win = NULL;

// ── dismiss helper ──

static void alert_dismiss(void)
{
	if(!alert_win)
		return;
	w_win_t w = alert_win;
	alert_win = NULL;  // prevent re-entrancy in destroy callback
	w_win_destroy(w);
	// AlertData is freed by the W_DESTROY callback
}

// ── draw callback ──

static int alert_draw_cb(w_win_t win, const W_EVENT *e, void *data)
{
	AlertData *ad = (AlertData *)data;
	if(!ad || !ad->text)
		return 0;

	cairo_t *cr = e->draw.cr;
	int w, h;
	w_win_get_size(win, &w, &h);

	int pad = alert_style.padding;
	int btn_w = ad->btn_w;
	int btn_h = ad->btn_h;
	int btn_y = h - pad - btn_h;
	int btn_x = w - pad - btn_w;

	// Save button rect for hit testing
	ad->btn_x = btn_x;
	ad->btn_y = btn_y;
	ad->btn_w = btn_w;
	ad->btn_h = btn_h;

	// Window background
	cairo_set_source_rgba(cr,
		alert_style.bg_color[0] / 255.0,
		alert_style.bg_color[1] / 255.0,
		alert_style.bg_color[2] / 255.0,
		alert_style.bg_color[3] / 255.0);
	cairo_paint(cr);

	// Message text
	PangoLayout *layout = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(layout, font_desc);
	pango_layout_set_text(layout, ad->text, -1);
	pango_layout_set_width(layout, (w - 2 * pad) * PANGO_SCALE);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	cairo_set_source_rgba(cr,
		alert_style.fg_color[0] / 255.0,
		alert_style.fg_color[1] / 255.0,
		alert_style.fg_color[2] / 255.0,
		alert_style.fg_color[3] / 255.0);
	cairo_move_to(cr, pad, pad);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);

	// OK button background + border (state-dependent color)
	const uint8_t *bg = alert_style.btn_bg;
	if(ad->pressed)
		bg = alert_style.btn_bg_press;
	else if(ad->hovered)
		bg = alert_style.btn_bg_hover;
	cairo_set_source_rgba(cr,
		bg[0] / 255.0, bg[1] / 255.0, bg[2] / 255.0, bg[3] / 255.0);
	cairo_rectangle(cr, btn_x, btn_y+0.5, btn_w, btn_h);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr,
		alert_style.btn_border[0] / 255.0,
		alert_style.btn_border[1] / 255.0,
		alert_style.btn_border[2] / 255.0,
		alert_style.btn_border[3] / 255.0);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);

	// OK button text
	layout = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(layout, font_desc);
	pango_layout_set_text(layout, ad->btn_text, -1);
	int tw, th;
	pango_layout_get_pixel_size(layout, &tw, &th);
	cairo_set_source_rgba(cr,
		alert_style.btn_fg[0] / 255.0,
		alert_style.btn_fg[1] / 255.0,
		alert_style.btn_fg[2] / 255.0,
		alert_style.btn_fg[3] / 255.0);
	cairo_move_to(cr, btn_x + (btn_w - tw) / 2, btn_y + (btn_h - th) / 2);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);

	return 0;
}

// ── mouse callback ──

static bool alert_is_over_button(AlertData *ad, int mx, int my)
{
	return mx >= ad->btn_x && mx < ad->btn_x + ad->btn_w &&
	       my >= ad->btn_y && my < ad->btn_y + ad->btn_h;
}

static int alert_mouse_cb(w_win_t win, const W_EVENT *e, void *data)
{
	AlertData *ad = (AlertData *)data;
	if(!ad)
		return 0;

	bool redraw = false;

	if(e->type == W_MOUSE_MOVE)
	{
		bool over = alert_is_over_button(ad, e->mouse.x, e->mouse.y);
		if(over != ad->hovered)
		{
			ad->hovered = over;
			redraw = true;
		}
	}
	else if(e->type == W_MOUSE_ENTER)
	{
		// Cursor entered the window — check if already over the button
		ad->hovered = alert_is_over_button(ad, e->mouse.x, e->mouse.y);
		if(ad->hovered)
			redraw = true;
	}
	else if(e->type == W_MOUSE_LEAVE)
	{
		if(ad->hovered || ad->pressed)
		{
			ad->hovered = false;
			ad->pressed = false;
			redraw = true;
		}
	}
	else if(e->type == W_MOUSE_DOWN && e->mouse.button == W_BUTTON_LEFT)
	{
		if(alert_is_over_button(ad, e->mouse.x, e->mouse.y))
		{
			ad->pressed = true;
			redraw = true;
		}
	}
	else if(e->type == W_MOUSE_UP && e->mouse.button == W_BUTTON_LEFT)
	{
		bool was_pressed = ad->pressed;
		ad->pressed = false;
		if(was_pressed && alert_is_over_button(ad, e->mouse.x, e->mouse.y))
		{
			alert_dismiss();
			return 0;
		}
		redraw = true;
	}

	if(redraw)
		w_win_redraw(win);
	return 0;
}

// ── keyboard callback ──

static int alert_key_cb(w_win_t win, const W_EVENT *e, void *data)
{
	AlertData *ad = (AlertData *)data;
	if(!ad)
		return 0;

	if(e->type == W_KEY_DOWN)
	{
		if(e->key.key == KEY_ENTER || e->key.key == KEY_KPENTER ||
		   e->key.key == KEY_SPACE || e->key.key == KEY_ESC)
		{
			alert_dismiss();
		}
	}
	return 0;
}

// ── close callback ──

static int alert_close_cb(w_win_t win, const W_EVENT *e, void *data)
{
	(void)win;
	(void)e;
	(void)data;
	alert_dismiss();
	return 0;
}

// ── destroy callback (frees AlertData) ──

static int alert_destroy_cb(w_win_t win, const W_EVENT *e, void *data)
{
	(void)win;
	(void)e;
	AlertData *ad = (AlertData *)data;
	if(ad)
	{
		l_free(ad->text);
		l_free(ad);
	}
	alert_win = NULL;
	return 0;
}

// ── public API ──

int w_win_alert(w_win_t win, const char *title, const char *text)
{
	if(!wl_display || !text || !*text)
		return -1;

	// If an alert is already showing, ignore (non-blocking: don't stack)
	if(alert_win)
		return 0;

	// Measure text to determine window size
	int pad = alert_style.padding;
	int btn_h = alert_style.btn_h;
	int btn_margin = alert_style.btn_margin;

	cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(tmp);
	PangoLayout *layout = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(layout, font_desc);
	pango_layout_set_text(layout, text, -1);
	// First pass: measure natural width (no wrap)
	pango_layout_set_width(layout, -1);
	int tw, th;
	pango_layout_get_pixel_size(layout, &tw, &th);
	int nat_w = tw + 2 * pad;
	int win_w;
	if(nat_w < alert_style.min_w)
	{
		// Below minimum: use min width, no wrapping
		win_w = alert_style.min_w;
	}
	else if(nat_w > alert_style.max_w)
	{
		// Exceeds maximum: wrap text at max width
		win_w = alert_style.max_w;
		pango_layout_set_width(layout, (win_w - 2 * pad) * PANGO_SCALE);
		pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
		pango_layout_get_pixel_size(layout, &tw, &th);
	}
	else
	{
		// Between min and max: use natural width, no wrapping
		win_w = nat_w;
	}
	g_object_unref(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(tmp);

	int win_h = pad + th + btn_margin + btn_h + pad;

	// Create xdg-toplevel window (compositor provides title bar decorations)
	alert_win = w_win_create(title ? title : "", "toplevel,decorated,center", NULL);
	if(!alert_win)
		return -1;

	if(win && win->wl_info.role_type==W_ROLE_DEFAULT)
	{
		xdg_toplevel_set_parent(
				alert_win->wl_info.toplevel.xdg_toplevel,
				win->wl_info.toplevel.xdg_toplevel);
	}

	w_win_preferred_size(alert_win,&win_w,&win_h);

	AlertData *ad = l_new0(AlertData);
	ad->text = l_strdup(text);
	// Choose button text based on system locale
	ad->btn_text = w_translate ? w_translate("确定") : "确定";
	ad->btn_w = alert_style.btn_w;
	ad->btn_h = alert_style.btn_h;

	w_win_connect(alert_win, W_DRAW, alert_draw_cb, ad);
	w_win_connect(alert_win, W_MOUSE_MOVE, alert_mouse_cb, ad);
	w_win_connect(alert_win, W_MOUSE_DOWN, alert_mouse_cb, ad);
	w_win_connect(alert_win, W_MOUSE_UP, alert_mouse_cb, ad);
	w_win_connect(alert_win, W_MOUSE_ENTER, alert_mouse_cb, ad);
	w_win_connect(alert_win, W_MOUSE_LEAVE, alert_mouse_cb, ad);
	w_win_connect(alert_win, W_KEY_DOWN, alert_key_cb, ad);
	w_win_connect(alert_win, W_CLOSE, alert_close_cb, NULL);
	w_win_connect(alert_win, W_DESTROY, alert_destroy_cb, ad);

	w_win_resize(alert_win, win_w, win_h);
	w_win_show(alert_win);
	w_win_redraw(alert_win);

	return 0;
}
