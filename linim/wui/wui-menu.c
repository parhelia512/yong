#include <glib.h>
#include <gio/gio.h>
#include <wayland-client.h>
#include <linux/input-event-codes.h>
#include "llib.h"
#include "wui.h"

// 是否按两列形式显示菜单
#define W_MENU_COLUMN2		1

#define WL_KEY(k)	(k)
// 两列形式显示菜单的最小菜单项数量
#define W_MENU_COLUMN2_MIN	10

#define W_MENU_ITEM_NORMAL		0
#define W_MENU_ITEM_SEPARATOR	1
#define W_MENU_ITEM_CHECK		2
#define W_MENU_ITEM_SUBMENU		3

typedef struct W_MENU_ITEM W_MENU_ITEM;
typedef struct W_MENU W_MENU;

struct W_MENU_ITEM{
	char *text;
	union{
		char *action;
		W_MENU *submenu;
	};
	int type;
	bool checked;
	GVariant *target;
	cairo_rectangle_int_t pos;
};

struct W_MENU{
	W_MENU_ITEM *items;
	int nitems;
	int flags;
	GSimpleActionGroup *actions;

	// Font measurements (same across all menus, stored on root for convenience)
	int check_width;
	int check_height;
	int arrow_width;
	int arrow_height;

	// Per-menu layout & state
	int selected_index;
	bool column2;    // Whether this menu uses two-column layout
	int column_gap;  // Gap between columns in two-column mode
	int column_width; // Width of each column in two-column mode

	W_MENU *root;    // Pointer to root menu (self for root menu)
	W_MENU *parent;  // Parent menu (for submenus, NULL for root)

	w_win_t parent_win;  // the window menu open for
	w_win_t root_win;    // Root menu popup window (set when shown)
	w_win_t submenu_win; // Currently open submenu popup (0 if none)
	
	PangoLayout *pango_layout;

	guint popup_timer_id;
	int popup_index;
	int popup_pending;
};

// Forward declarations
static W_MENU *w_menu_from_gmenu_internal(GMenu *gmenu, GSimpleActionGroup *actions, int flags, W_MENU *root);
static w_win_t w_menu_submenu_open(w_win_t parent_popup, W_MENU *parent_menu, W_MENU_ITEM *item);

// Count total items including separators for sections
static int count_menu_items(GMenuModel *model, bool is_first)
{
	int n_items = g_menu_model_get_n_items(model);
	int total = 0;

	for(int i = 0; i < n_items; i++)
	{
		// Check for section link first
		GMenuModel *section = g_menu_model_get_item_link(model, i, G_MENU_LINK_SECTION);
		if(section)
		{
			// Add separator before non-first section
			if(!is_first)
				total++;
			total += count_menu_items(section, true);
			g_object_unref(section);
			is_first = false;
			continue;
		}

		total++; // Count the actual item
		is_first = false;
	}
	return total;
}

// Fill menu items from GMenuModel
static int fill_menu_items(GMenuModel *model, GSimpleActionGroup *actions, int flags,
	W_MENU_ITEM *items, int index, bool is_first, W_MENU *root)
{
	int n_items = g_menu_model_get_n_items(model);

	for(int i = 0; i < n_items; i++)
	{
		// Check for section link first
		GMenuModel *section = g_menu_model_get_item_link(model, i, G_MENU_LINK_SECTION);
		if(section)
		{
			// Add separator before non-first section
			if(!is_first)
			{
				W_MENU_ITEM *sep = &items[index++];
				sep->type = W_MENU_ITEM_SEPARATOR;
				sep->text = NULL;
			}
			index = fill_menu_items(section, actions, flags, items, index, true, root);
			g_object_unref(section);
			is_first = false;
			continue;
		}

		W_MENU_ITEM *item = &items[index++];
		is_first = false;

		// Get label attribute
		GVariant *label_var = g_menu_model_get_item_attribute_value(model, i,
			G_MENU_ATTRIBUTE_LABEL, G_VARIANT_TYPE_STRING);
		if(label_var)
		{
			item->text = l_strdup(g_variant_get_string(label_var, NULL));
			g_variant_unref(label_var);
		}

		// Check if this is a separator (no label or empty label)
		if(!item->text || item->text[0] == '\0')
		{
			item->type = W_MENU_ITEM_SEPARATOR;
			l_free(item->text);
			item->text = NULL;
			continue;
		}

		// Check for submenu
		GMenuModel *submenu_model = g_menu_model_get_item_link(model, i, G_MENU_LINK_SUBMENU);
		if(submenu_model)
		{
			item->type = W_MENU_ITEM_SUBMENU;
			item->submenu = w_menu_from_gmenu_internal((GMenu*)submenu_model, actions, flags, root);
			g_object_unref(submenu_model);
			continue;
		}

		// Normal/check menu item - get action attribute
		item->type = W_MENU_ITEM_NORMAL;
		GVariant *action_var = g_menu_model_get_item_attribute_value(model, i,
			G_MENU_ATTRIBUTE_ACTION, G_VARIANT_TYPE_STRING);
		if(action_var)
		{
			item->action = l_strdup(g_variant_get_string(action_var, NULL));
			g_variant_unref(action_var);
		}

		// Get target attribute for radio items
		GVariant *target_var = g_menu_model_get_item_attribute_value(model, i,
			G_MENU_ATTRIBUTE_TARGET, NULL);

		// Check action state for check/radio items
		if(actions && item->action)
		{
			const char *action_name = item->action;
			if(strncmp(action_name, "app.", 4) == 0)
				action_name += 4;

			GAction *action = g_action_map_lookup_action(G_ACTION_MAP(actions), action_name);
			if(action)
			{
				const GVariantType *state_type = g_action_get_state_type(action);
				GVariant *state = g_action_get_state(action);

				// Boolean state = check item
				if(state_type && g_variant_type_equal(state_type, G_VARIANT_TYPE_BOOLEAN))
				{
					item->type = W_MENU_ITEM_CHECK;
					item->checked = g_variant_get_boolean(state);
				}
				// Has target = radio item (treat as check), compare state with target
				else if(target_var && state)
				{
					item->type = W_MENU_ITEM_CHECK;
					item->checked = g_variant_equal(state, target_var);
				}
				if(state)
					g_variant_unref(state);
			}
		}

		if(target_var)
		{
			item->target=target_var;
		}
	}
	return index;
}

static W_MENU *w_menu_from_gmenu_internal(GMenu *gmenu, GSimpleActionGroup *actions, int flags, W_MENU *root)
{
	if(!gmenu || !actions)
		return NULL;

	GMenuModel *model = G_MENU_MODEL(gmenu);
	int total_items = count_menu_items(model, true);

	if(total_items == 0)
		return NULL;

	W_MENU *menu = l_new0(W_MENU);
	menu->items = l_cnew0(total_items, W_MENU_ITEM);
	menu->nitems = total_items;
	menu->flags = flags;
	menu->root = root?root:menu;

	fill_menu_items(model, actions, flags, menu->items, 0, true, menu->root);

	menu->actions = g_object_ref(actions);

	return menu;
}

static W_MENU *w_menu_from_gmenu(GMenu *gmenu, GSimpleActionGroup *actions, int flags)
{
	W_MENU *menu = w_menu_from_gmenu_internal(gmenu, actions, flags, NULL);
	return menu;
}

static void w_menu_free(W_MENU *menu);
static void w_menu_item_free(W_MENU_ITEM *item)
{
	if(item->type == W_MENU_ITEM_SUBMENU)
		w_menu_free(item->submenu);
	else if(item->type != W_MENU_ITEM_SEPARATOR)
		l_free(item->action);
	if(item->target)
		g_variant_unref(item->target);
	l_free(item->text);
}

static void w_menu_free(W_MENU *menu)
{
	if(!menu)
		return;
	for(int i = 0; i < menu->nitems; i++)
		w_menu_item_free(&menu->items[i]);
	if(menu->actions)
		g_object_unref(menu->actions);
	if(menu->pango_layout)
		g_object_unref(menu->pango_layout);
	if(menu->popup_timer_id)
		g_source_remove(menu->popup_timer_id);
	l_free(menu->items);
	l_free(menu);
}

static int w_menu_calc_size(W_MENU *menu, const w_ui_style_t *style, int *width, int *height)
{
	if(!menu || !style || !width || !height)
		return -1;

	int sep_height = style->sep_height;
	int border_width = style->border_width;
	bool use_column2 = (menu->flags & W_MENU_COLUMN2) && (menu->nitems >= W_MENU_COLUMN2_MIN);
	int column_gap = 0;//style->padding[1];

	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(surface);
	double scale=menu->root->parent_win->scale;
	cairo_scale(cr,scale,scale);
	PangoLayout *layout = menu->root->pango_layout;
	if(!layout)
		layout=menu->root->pango_layout=pango_cairo_create_layout(cr);
	if(!layout) { cairo_destroy(cr); cairo_surface_destroy(surface); return -1; }
	pango_layout_set_font_description(layout, font_desc);

	int check_w = 0, check_h = 0, arrow_w = 0, arrow_h = 0;
	pango_layout_set_text(layout, "✓", -1);
	pango_layout_get_pixel_size(layout, &check_w, &check_h);
	pango_layout_set_text(layout, ">", -1);
	pango_layout_get_pixel_size(layout, &arrow_w, &arrow_h);

	int indicator = check_w + arrow_w + 2 * style->padding[1];
	int max_width = 0, max_height = 0;

	// Single pass: measure text + init pos.width/height, track max width
	for(int i = 0; i < menu->nitems; i++)
	{
		W_MENU_ITEM *item = &menu->items[i];
		if(item->type == W_MENU_ITEM_SEPARATOR)
		{
			item->pos.width = 0;
			item->pos.height = sep_height;
			continue;
		}

		int tw = 0, th = 0;
		if(item->text)
		{
			pango_layout_set_text(layout, item->text, -1);
			pango_layout_get_pixel_size(layout, &tw, &th);
		}

		item->pos.width = tw + 2 * style->padding[1] + indicator;
		item->pos.height = th + 2 * style->padding[0];

		if(item->pos.width > max_width) max_width = item->pos.width;
		if(th > max_height) max_height = th;
	}

	// Uniform item height for consistent display
	int item_h = max_height + 2 * style->padding[0];
	for(int i = 0; i < menu->nitems; i++)
		if(menu->items[i].type != W_MENU_ITEM_SEPARATOR)
			menu->items[i].pos.height = item_h;

	// Column width: same for both sides (symmetric)
	int col_w = max_width;
	int final_width = use_column2 ? col_w * 2 + column_gap + 2 * border_width
	                              : max_width + 2 * border_width;

	// Position items
	int y_left = border_width, y_right = border_width;
	int left_h = 0, right_h = 0;
	int left_count = (menu->nitems + 1) / 2;

	for(int i = 0; i < menu->nitems; i++)
	{
		W_MENU_ITEM *item = &menu->items[i];
		if(use_column2 && i < left_count)
		{
			item->pos.x = border_width;
			item->pos.y = y_left;
			item->pos.width = col_w;
			y_left += item->pos.height;
			left_h += item->pos.height;
		}
		else if(use_column2)
		{
			item->pos.x = border_width + col_w + column_gap;
			item->pos.y = y_right;
			item->pos.width = col_w;
			y_right += item->pos.height;
			right_h += item->pos.height;
		}
		else
		{
			item->pos.x = border_width;
			item->pos.y = y_left; // reuse y_left as single accumulator
			item->pos.width = max_width;
			y_left += item->pos.height;
			left_h += item->pos.height;
		}
	}

	// Store per-menu layout state
	W_MENU *root = menu->root;
	root->check_width = check_w;
	root->check_height = check_h;
	root->arrow_width = arrow_w;
	root->arrow_height = arrow_h;
	menu->selected_index = -1;
	menu->column2 = use_column2;
	menu->column_gap = column_gap;
	menu->column_width = use_column2 ? col_w : 0;

	*width = final_width;
	*height = (use_column2 ? (left_h > right_h ? left_h : right_h) : left_h) + 2 * border_width;

	cairo_destroy(cr);
	cairo_surface_destroy(surface);

	return 0;
}
// xdg_popup configure listener
static void xdg_popup_configure(void *data, struct xdg_popup *xdg_popup,
	int32_t x, int32_t y, int32_t width, int32_t height)
{
	w_win_t win = (w_win_t)data;
	w_win_resize_internal(win,width,height);
}

// xdg_popup done (closed) listener - destroy window
static void xdg_popup_done(void *data, struct xdg_popup *xdg_popup)
{
	w_win_t win = (w_win_t)data;
	if(!win)
		return;
	// Destroy window, menu resources will be cleaned in W_DESTROY callback
	w_win_destroy(win);
}

static const struct xdg_popup_listener xdg_popup_listener = {
	.configure = xdg_popup_configure,
	.popup_done = xdg_popup_done,
	.repositioned = NULL,
};

// Menu window destroy callback - cleanup menu resources
static int menu_destroy_cb(w_win_t win, const W_EVENT *e, void *data)
{
	W_MENU *menu = (W_MENU*)data;
	if(!menu)
		return 0;

	W_MENU *parent = menu->parent;
	bool is_root = (menu == menu->root);

	if(menu->submenu_win)
	{
		w_win_t old_sub = menu->submenu_win;
		menu->submenu_win = NULL;
		w_win_destroy(old_sub);
	}

	if(parent && parent->submenu_win == win)
		parent->submenu_win = NULL;

	if(is_root)
	{
		if(menu->root_win==wl_modal_win)
			wl_modal_win=NULL;
		w_menu_free(menu);
	}
	return 0;
}

// Menu window draw callback
static int  menu_draw_cb(w_win_t win, const W_EVENT *e, void *data)
{
	W_MENU *menu = (W_MENU*)data;
	if(!menu || !e->draw.cr)
		return 0;

	W_MENU *root = menu->root;
	W_MENU *current = menu;  // Each popup draws its own menu

	cairo_t *cr = e->draw.cr;
	const w_ui_style_t *style = &default_ui_style;

	// Draw background
	cairo_set_source_rgb(cr, style->bg_menu[0] / 255.0, style->bg_menu[1] / 255.0, style->bg_menu[2] / 255.0);
	cairo_paint(cr);

	// Get rendering params from root menu
	int border_width = style->border_width;
	int padding = style->padding[1];
	int check_width = root->check_width;
	int check_height = root->check_height;
	int arrow_width = root->arrow_width;
	int arrow_height = root->arrow_height;

	// Draw border
	cairo_set_source_rgb(cr, style->border_color[0] / 255.0, style->border_color[1] / 255.0, style->border_color[2] / 255.0);
	cairo_set_line_width(cr, border_width);
	int w = win->width;
	int h = win->height;
	cairo_rectangle(cr, border_width / 2.0, border_width / 2.0, w - border_width, h - border_width);
	cairo_stroke(cr);

	// Draw column separator line in two-column mode
	if(current->column2 && current->column_width > 0)
	{
		cairo_set_source_rgb(cr, style->separator_color[0] / 255.0, style->separator_color[1] / 255.0, style->separator_color[2] / 255.0);
		cairo_set_line_width(cr, style->sep_height);
		double sep_x = border_width + current->column_width + current->column_gap / 2.0;
		cairo_move_to(cr, sep_x, border_width);
		cairo_line_to(cr, sep_x, h - border_width);
		cairo_stroke(cr);
	}

	// Create pango layout for text
	PangoLayout *layout = menu->root->pango_layout;
	if(!layout)
		return 0;

	// Draw each menu item
	for(int i = 0; i < current->nitems; i++)
	{
		W_MENU_ITEM *item = &current->items[i];

		if(item->type == W_MENU_ITEM_SEPARATOR)
		{
			// Draw separator line
			cairo_set_source_rgb(cr, style->separator_color[0] / 255.0, style->separator_color[1] / 255.0, style->separator_color[2] / 255.0);
			cairo_set_line_width(cr, 1);
			double sep_y = item->pos.y + item->pos.height / 2.0;
			cairo_move_to(cr, item->pos.x, sep_y);
			cairo_line_to(cr, item->pos.x + item->pos.width, sep_y);
			cairo_stroke(cr);
			continue;
		}

		// Draw selection background if selected
		if(i == current->selected_index)
		{
			cairo_set_source_rgb(cr, style->sep_color[0] / 255.0, style->sep_color[1] / 255.0, style->sep_color[2] / 255.0);
			cairo_rectangle(cr, item->pos.x, item->pos.y, item->pos.width, item->pos.height);
			cairo_fill(cr);
		}

		// Draw check mark if checked (left side, always reserve space)
		int check_x = item->pos.x + padding;
		int check_y = item->pos.y + (item->pos.height - check_height) / 2;

		if(item->type == W_MENU_ITEM_CHECK && item->checked)
		{
			cairo_set_source_rgb(cr, style->fg_menu[0] / 255.0, style->fg_menu[1] / 255.0, style->fg_menu[2] / 255.0);
			pango_layout_set_text(layout, "✓", -1);
			cairo_move_to(cr, check_x, check_y);
			pango_cairo_show_layout(cr, layout);
		}

		// Draw text (centered between check and arrow areas)
		int text_start_x = item->pos.x + padding + check_width + padding;

		if(item->text)
		{
			cairo_set_source_rgb(cr, style->fg_menu[0] / 255.0, style->fg_menu[1] / 255.0, style->fg_menu[2] / 255.0);
			pango_layout_set_text(layout, item->text, -1);
			int text_w, text_h;
			pango_layout_get_pixel_size(layout, &text_w, &text_h);
			int text_x = text_start_x;
			int text_y = item->pos.y + (item->pos.height - text_h) / 2;
			cairo_move_to(cr, text_x, text_y);
			pango_cairo_show_layout(cr, layout);
		}

		// Draw submenu arrow (right side, always reserve space)
		int arrow_x = item->pos.x + item->pos.width - padding - arrow_width;
		int arrow_y = item->pos.y + (item->pos.height - arrow_height) / 2;

		if(item->type == W_MENU_ITEM_SUBMENU)
		{
			cairo_set_source_rgb(cr, style->fg_menu[0] / 255.0, style->fg_menu[1] / 255.0, style->fg_menu[2] / 255.0);
			pango_layout_set_text(layout, ">", -1);
			cairo_move_to(cr, arrow_x, arrow_y);
			pango_cairo_show_layout(cr, layout);
		}
	}

	return 1;
}

// Find next selectable item in dir (-1 for up, 1 for down), returns -1 if none
static int w_menu_find_next_item(W_MENU *menu, int from, int dir)
{
	for(int i = from + dir; dir > 0 ? i < menu->nitems : i >= 0; i += dir)
	{
		if(menu->items[i].type != W_MENU_ITEM_SEPARATOR)
			return i;
	}
	return -1;
}

// Find menu item at given position (returns index and optionally item pointer)
// Returns -1 if no item found at position
static int w_menu_find_item_at(W_MENU *current, int mx, int my, W_MENU_ITEM **item_out)
{
	if(!current)
		return -1;

	for(int i = 0; i < current->nitems; i++)
	{
		W_MENU_ITEM *item = &current->items[i];

		// Check if position is within this item's bounds
		if(mx >= item->pos.x && mx < item->pos.x + item->pos.width &&
		   my >= item->pos.y && my < item->pos.y + item->pos.height)
		{
			if(item_out)
				*item_out = item;
			return i;
		}
	}

	if(item_out)
		*item_out = NULL;
	return -1;
}

// Activate menu item (handle action or open submenu)
static void w_menu_activate_item(w_win_t win, W_MENU *current, int index)
{
	if(!win || !current)
		return;
	W_MENU_ITEM *item=current->items+index;
	W_MENU *root = current->root;

	// Handle action items (normal and check items)
	if(item->type == W_MENU_ITEM_NORMAL || item->type == W_MENU_ITEM_CHECK)
	{
		if(item->action)
		{
			const char *action_name = item->action;
			if(strncmp(action_name, "app.", 4) == 0)
				action_name += 4;

			GAction *action = g_action_map_lookup_action(G_ACTION_MAP(root->actions), action_name);
			if(action)
			{
				GSimpleActionGroup *actions=root->actions;
				GVariant *target=item->target;
				root->actions=NULL;
				item->target=NULL;
				w_win_destroy(root->root_win);
				g_action_activate(action, target);
				if(actions)
					g_object_unref(actions);
				if(target)
					g_variant_unref(target);
				return;
			}
		}

		// Close the entire menu hierarchy (action in submenu closes root too)
		w_win_destroy(root->root_win ? root->root_win : win);
		return;
	}

	// Submenu items: open in a separate xdg-popup
	if(item->type == W_MENU_ITEM_SUBMENU)
	{
		// Close existing submenu before opening a new one
		if(current->submenu_win)
		{
			if(current->popup_index==index)
				return;
			w_win_destroy(current->submenu_win);
			current->submenu_win = NULL;
		}

		// Open submenu in new popup positioned to the right of the item
		w_win_t sub_win = w_menu_submenu_open(win, current, item);
		if(sub_win)
		{
			w_win_show(sub_win);
			current->popup_index=index;
		}
		return;
	}
}

static gboolean menu_popup_timer(gpointer data)
{
	w_win_t win=data;
	W_MENU *menu=w_win_get_data(win);
	menu->popup_timer_id=0;
	if(menu->submenu_win)
	{
		w_win_destroy(menu->submenu_win);
		menu->submenu_win=NULL;
	}
	return G_SOURCE_REMOVE;
}

static void menu_select_item(w_win_t win,W_MENU *menu,int index)
{
	if(index==menu->selected_index || index<0)
		return;
	if(menu->popup_timer_id)
	{
		g_source_remove(menu->popup_timer_id);
		menu->popup_timer_id=0;
	}
	W_MENU *pmenu=menu->parent;
	if(pmenu && pmenu->popup_index!=pmenu->selected_index)
	{
		pmenu->selected_index=pmenu->popup_index;
		w_win_t pwin=pmenu->parent?pmenu->parent->submenu_win:pmenu->root_win;
		w_win_redraw(pwin);
	}
	W_MENU_ITEM *item=&menu->items[index];
	if(menu->submenu_win && index!=menu->popup_index)
	{
		menu->popup_timer_id=g_timeout_add(100,menu_popup_timer,win);
	}
	if(item->type==W_MENU_ITEM_SEPARATOR)
		index=-1;
	menu->selected_index=index;
	w_win_redraw(win);
}

static int menu_mouse_down_cb(w_win_t win, const W_EVENT *e, void *data)
{
	W_MENU *menu = (W_MENU*)data;
	int mx = e->mouse.x;
	int my = e->mouse.y;

	W_MENU_ITEM *clicked_item = NULL;
	int clicked_index = w_menu_find_item_at(menu, mx, my, &clicked_item);
	if(clicked_index < 0 || !clicked_item)
		return 0;
	menu_select_item(win,menu,clicked_index);
	if(clicked_item->type!=W_MENU_ITEM_SEPARATOR)
		w_menu_activate_item(win, menu, clicked_index);
	return 0;
}

static int menu_mouse_leave_cb(w_win_t win, const W_EVENT *e, void *data)
{
	W_MENU *menu = (W_MENU*)data;
	if(menu->popup_timer_id)
	{
		g_source_remove(menu->popup_timer_id);
		menu->popup_timer_id=0;
	}
	return 0;
}

static int menu_mouse_move_cb(w_win_t win, const W_EVENT *e, void *data)
{
	W_MENU *menu = (W_MENU*)data;
	int mx = e->mouse.x;
	int my = e->mouse.y;
	int hovered_index = w_menu_find_item_at(menu, mx, my, NULL);
	menu_select_item(win,menu,hovered_index);
	return 0;
}

// Menu window key press callback - handle keyboard navigation
static int menu_key_down_cb(w_win_t win, const W_EVENT *e, void *data)
{
	W_MENU *menu = (W_MENU*)data;
	if(!menu || !e->key.key)
		return 0;

	W_MENU *current = menu;  // This popup's own menu
	if(!current)
		return 0;

	if(current->submenu_win)
	{
		menu_key_down_cb(current->submenu_win,e,w_win_get_data(current->submenu_win));
		return 0;
	}

	uint32_t key = e->key.key;

	// ESC key - close this popup (submenu goes back, root closes everything)
	if(key == WL_KEY(KEY_ESC))
	{
		// Close child submenu of this popup first if any
		if(current->submenu_win)
		{
			w_win_t old_sub = current->submenu_win;
			current->submenu_win = NULL;
			w_win_destroy(old_sub);
			return 1;
		}
		w_win_destroy(win);
		return 1;
	}

	// Arrow keys for navigation
	int old_index = current->selected_index;
	int new_index = old_index;

	if(key == WL_KEY(KEY_UP) || key == WL_KEY(KEY_DOWN))
	{
		int dir = (key == WL_KEY(KEY_UP)) ? -1 : 1;
		int found = w_menu_find_next_item(current, old_index, dir);
		if(found >= 0)
			new_index = found;
		else if(current->nitems > 0)
		{
			// Wrap around
			int wrap_start = (dir > 0) ? -1 : current->nitems;
			found = w_menu_find_next_item(current, wrap_start, dir);
			if(found >= 0)
				new_index = found;
		}
		menu_select_item(win,menu,new_index);
	}
	else if(key == WL_KEY(KEY_RIGHT))
	{
		if(old_index >= 0 && old_index < current->nitems)
		{
			W_MENU_ITEM *item = &current->items[old_index];
			if(item->type == W_MENU_ITEM_SUBMENU)
			{
				w_menu_activate_item(win, current, old_index);
				return 1;
			}
		}
	}
	else if(key == WL_KEY(KEY_LEFT))
	{
		// Close this popup if it's a submenu (go back to parent menu)
		if(current->parent)
		{
			w_win_destroy(win);
			return 1;
		}
	}
	else if(key == WL_KEY(KEY_ENTER))
	{
		if(old_index >= 0 && old_index < current->nitems)
		{
			w_menu_activate_item(win, current, old_index);
			return 1;
		}
	}

	return 1;
}

static w_win_t w_menu_win_create(w_win_t win, int width, int height, void *data,
    int anchor_x, int anchor_y, int anchor_w, int anchor_h)
{
	if((win->wl_info.role_type!=W_ROLE_DEFAULT &&
	    win->wl_info.role_type!=W_ROLE_LAYER &&
	    win->wl_info.role_type!=W_ROLE_POPUP) || !win->surface)
		return NULL;
	w_win_t res=w_win_create("", "popup", data);
	if(!res)
		return NULL;
	if(res->wl_info.popup.xdg_surface==NULL)
	{
		w_win_destroy(res);
		return NULL;
	}
	struct xdg_positioner *positioner=xdg_wm_base_create_positioner(wl_xdg_wm_base);
	xdg_positioner_set_size(positioner, width, height);
	xdg_positioner_set_anchor_rect(positioner, anchor_x, anchor_y, anchor_w, anchor_h);
	xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_TOP_RIGHT);
	xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	xdg_positioner_set_constraint_adjustment(positioner,
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y
		);

	if(win->wl_info.role_type==W_ROLE_LAYER)
	{
		res->wl_info.popup.xdg_popup = xdg_surface_get_popup(
			res->wl_info.popup.xdg_surface,
			NULL,
			positioner);
		zwlr_layer_surface_v1_get_popup(win->wl_info.layer.layer_surface, res->wl_info.popup.xdg_popup);
	}
	else if(win->wl_info.role_type==W_ROLE_POPUP)
	{
		res->wl_info.popup.xdg_popup = xdg_surface_get_popup(
			res->wl_info.popup.xdg_surface,
			win->wl_info.popup.xdg_surface,
			positioner);
	}
	else
	{
		res->wl_info.popup.xdg_popup = xdg_surface_get_popup(
			res->wl_info.popup.xdg_surface,
			win->wl_info.toplevel.xdg_surface,
			positioner);
	}
	xdg_popup_grab(res->wl_info.popup.xdg_popup,wl_seat,wl_last_serial);

	// Destroy positioner after use
	res->wl_info.popup.xdg_positioner=positioner;

	// Add popup configure listener
	xdg_popup_add_listener(res->wl_info.popup.xdg_popup, &xdg_popup_listener, res);

	// Connect event handlers
	w_win_connect(res, W_DRAW, menu_draw_cb, data);
	w_win_connect(res, W_MOUSE_DOWN, menu_mouse_down_cb, data);
	w_win_connect(res, W_MOUSE_MOVE, menu_mouse_move_cb, data);
	w_win_connect(res, W_MOUSE_LEAVE, menu_mouse_leave_cb, data);
	w_win_connect(res, W_KEY_DOWN, menu_key_down_cb, data);
	w_win_connect(res, W_DESTROY, menu_destroy_cb, data);

	// Initial commit
	wl_surface_commit(res->surface);

	return res;
}

// Open a submenu as a new xdg-popup positioned to the right of the activating item
static w_win_t w_menu_submenu_open(w_win_t parent_popup, W_MENU *parent_menu, W_MENU_ITEM *item)
{
	if(!parent_popup || !parent_menu || !item || item->type != W_MENU_ITEM_SUBMENU)
		return NULL;

	W_MENU *submenu = item->submenu;
	if(!submenu)
		return NULL;

	// Calculate submenu size
	int width, height;
	if(0 != w_menu_calc_size(submenu, &default_ui_style, &width, &height))
		return NULL;

	// Create new popup window anchored to the right of the item, top-aligned
	w_win_t sub_win = w_menu_win_create(parent_popup, width, height, submenu,
		item->pos.x,
		item->pos.y,
		item->pos.width,
		item->pos.height
	);
	if(!sub_win)
		return NULL;

	// Link parent menu for cleanup
	submenu->parent = parent_menu;
	submenu->root = parent_menu->root;

	// Initialize submenu selection to none
	submenu->selected_index = -1;

	// Store ref to submenu popup in parent menu (each level tracks its own)
	parent_menu->submenu_win = sub_win;

	return sub_win;
}

static int w_popup_menu_internal(W_MENU *menu, w_win_t win)
{
	if(!win || !menu || !font_desc)
		return -1;

	menu->parent_win = win;
	int width, height;
	if(0 != w_menu_calc_size(menu, &default_ui_style, &width, &height))
	{
		return -1;
	}

	w_win_t menu_win = w_menu_win_create(win, width, height, menu,
		win->mouse_x, win->mouse_y, 1, 1);
	if(!menu_win)
	{
		return -1;
	}

	// Store root popup window reference for closing the full hierarchy
	menu->root_win = menu_win;
	menu->submenu_win = NULL;
	wl_modal_win = menu_win;
	w_win_show(menu_win);
	return 0;
}

