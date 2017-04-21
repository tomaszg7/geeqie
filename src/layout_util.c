/*
 * Copyright (C) 2004 John Ellis
 * Copyright (C) 2008 - 2016 The Geeqie Team
 *
 * Author: John Ellis
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "main.h"
#include "layout_util.h"

#include "advanced_exif.h"
#include "bar_sort.h"
#include "bar.h"
#include "cache_maint.h"
#include "collect.h"
#include "collect-dlg.h"
#include "compat.h"
#include "color-man.h"
#include "dupe.h"
#include "editors.h"
#include "filedata.h"
#include "history_list.h"
#include "image-overlay.h"
#include "histogram.h"
#include "img-view.h"
#include "layout_image.h"
#include "logwindow.h"
#include "misc.h"
#include "pan-view.h"
#include "pixbuf_util.h"
#include "preferences.h"
#include "print.h"
#include "search.h"
#include "ui_fileops.h"
#include "ui_menu.h"
#include "ui_misc.h"
#include "ui_tabcomp.h"
#include "utilops.h"
#include "view_dir.h"
#include "window.h"
#include "metadata.h"
#include "desktop_file.h"

#include <gdk/gdkkeysyms.h> /* for keyboard values */
#include "keymap_template.c"

#define MENU_EDIT_ACTION_OFFSET 16

static gboolean layout_bar_enabled(LayoutWindow *lw);
static gboolean layout_bar_sort_enabled(LayoutWindow *lw);
static void layout_util_sync_views(LayoutWindow *lw);

/*
 *-----------------------------------------------------------------------------
 * keyboard handler
 *-----------------------------------------------------------------------------
 */

static guint tree_key_overrides[] = {
	GDK_KEY_Page_Up,	GDK_KEY_KP_Page_Up,
	GDK_KEY_Page_Down,	GDK_KEY_KP_Page_Down,
	GDK_KEY_Home,	GDK_KEY_KP_Home,
	GDK_KEY_End,	GDK_KEY_KP_End
};

static gboolean layout_key_match(guint keyval)
{
	guint i;

	for (i = 0; i < sizeof(tree_key_overrides) / sizeof(guint); i++)
		{
		if (keyval == tree_key_overrides[i]) return TRUE;
		}

	return FALSE;
}

gboolean layout_key_press_cb(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
	LayoutWindow *lw = data;
	GtkWidget *focused;
	gboolean stop_signal = FALSE;
	gint x = 0;
	gint y = 0;

	if (lw->path_entry && gtk_widget_has_focus(lw->path_entry))
		{
		if (event->keyval == GDK_KEY_Escape && lw->dir_fd)
			{
			gtk_entry_set_text(GTK_ENTRY(lw->path_entry), lw->dir_fd->path);
			}

		/* the gtkaccelgroup of the window is stealing presses before they get to the entry (and more),
		 * so when the some widgets have focus, give them priority (HACK)
		 */
		if (gtk_widget_event(lw->path_entry, (GdkEvent *)event))
			{
			return TRUE;
			}
		}
	if (lw->vd && lw->options.dir_view_type == DIRVIEW_TREE && gtk_widget_has_focus(lw->vd->view) &&
	    !layout_key_match(event->keyval) &&
	    gtk_widget_event(lw->vd->view, (GdkEvent *)event))
		{
		return TRUE;
		}
	if (lw->bar &&
	    bar_event(lw->bar, (GdkEvent *)event))
		{
		return TRUE;
		}

	focused = gtk_container_get_focus_child(GTK_CONTAINER(lw->image->widget));
	if (lw->image &&
	    ((focused && gtk_widget_has_focus(focused)) || (lw->tools && widget == lw->window) || lw->full_screen) )
		{
		stop_signal = TRUE;
		switch (event->keyval)
			{
			case GDK_KEY_Left: case GDK_KEY_KP_Left:
				x -= 1;
				break;
			case GDK_KEY_Right: case GDK_KEY_KP_Right:
				x += 1;
				break;
			case GDK_KEY_Up: case GDK_KEY_KP_Up:
				y -= 1;
				break;
			case GDK_KEY_Down: case GDK_KEY_KP_Down:
				y += 1;
				break;
			default:
				stop_signal = FALSE;
				break;
			}

		if (!stop_signal &&
		    !(event->state & GDK_CONTROL_MASK))
			{
			stop_signal = TRUE;
			switch (event->keyval)
				{
				case GDK_KEY_Menu:
					layout_image_menu_popup(lw);
					break;
				default:
					stop_signal = FALSE;
					break;
				}
			}
		}

	if (x != 0 || y!= 0)
		{
		keyboard_scroll_calc(&x, &y, event);
		layout_image_scroll(lw, x, y, (event->state & GDK_SHIFT_MASK));
		}

	return stop_signal;
}

void layout_keyboard_init(LayoutWindow *lw, GtkWidget *window)
{
	g_signal_connect(G_OBJECT(window), "key_press_event",
			 G_CALLBACK(layout_key_press_cb), lw);
}

/*
 *-----------------------------------------------------------------------------
 * menu callbacks
 *-----------------------------------------------------------------------------
 */


static GtkWidget *layout_window(LayoutWindow *lw)
{
	return lw->full_screen ? lw->full_screen->window : lw->window;
}

static void layout_exit_fullscreen(LayoutWindow *lw)
{
	if (!lw->full_screen) return;
	layout_image_full_screen_stop(lw);
}

static void layout_menu_new_window_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	LayoutWindow *nw;
	LayoutOptions lop;
	gboolean tmp = options->save_window_positions;

	if (!options->use_saved_window_positions_for_new_windows)
		options->save_window_positions = FALSE; /* let the windowmanager decide for the first time */

	layout_exit_fullscreen(lw);

	layout_sync_options_with_current_state(lw);
	lop = lw->options; /* we can copy it directly, no strings are modified */

	lop.id = NULL; /* get a new id */
	nw = layout_new(NULL, &lop);
	layout_sort_set(nw, options->file_sort.method, options->file_sort.ascending);
	layout_set_fd(nw, lw->dir_fd);
	options->save_window_positions = tmp;
}

static void layout_menu_new_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	collection_window_new(NULL);
}

static void layout_menu_open_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	collection_dialog_load(NULL);
}

static void layout_menu_search_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	search_new(lw->dir_fd, layout_image_get_fd(lw));
}

static void layout_menu_dupes_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	dupe_window_new();
}

static void layout_menu_pan_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	pan_window_new(lw->dir_fd);
}

static void layout_menu_print_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	print_window_new(layout_image_get_fd(lw), layout_selection_list(lw), layout_list(lw), layout_window(lw));
}

static void layout_menu_dir_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	if (lw->vd) vd_new_folder(lw->vd, lw->dir_fd);
}

static void layout_menu_copy_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	file_util_copy(NULL, layout_selection_list(lw), NULL, layout_window(lw));
}

static void layout_menu_copy_path_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	file_util_copy_path_list_to_clipboard(layout_selection_list(lw));
}

static void layout_menu_move_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	file_util_move(NULL, layout_selection_list(lw), NULL, layout_window(lw));
}

static void layout_menu_rename_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	file_util_rename(NULL, layout_selection_list(lw), layout_window(lw));
}

static void layout_menu_delete_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	file_util_delete(NULL, layout_selection_list(lw), layout_window(lw));
}

static void layout_menu_delete_key_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	if (options->file_ops.enable_delete_key)
		{
		file_util_delete(NULL, layout_selection_list(lw), layout_window(lw));
		}
}

static void layout_menu_disable_grouping_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	file_data_disable_grouping_list(layout_selection_list(lw), TRUE);
}

static void layout_menu_enable_grouping_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	file_data_disable_grouping_list(layout_selection_list(lw), FALSE);
}

static void layout_menu_close_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	layout_close(lw);
}

static void layout_menu_exit_cb(GtkAction *action, gpointer data)
{
	exit_program();
}

static void layout_menu_alter_90_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_alter_orientation(lw, ALTER_ROTATE_90);
}

static void layout_menu_alter_90cc_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_alter_orientation(lw, ALTER_ROTATE_90_CC);
}

static void layout_menu_alter_180_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_alter_orientation(lw, ALTER_ROTATE_180);
}

static void layout_menu_alter_mirror_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_alter_orientation(lw, ALTER_MIRROR);
}

static void layout_menu_alter_flip_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_alter_orientation(lw, ALTER_FLIP);
}

static void layout_menu_alter_desaturate_cb(GtkToggleAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_set_desaturate(lw, gtk_toggle_action_get_active(action));
}

static void layout_menu_alter_none_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_alter_orientation(lw, ALTER_NONE);
}

static void layout_menu_exif_rotate_cb(GtkToggleAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	options->image.exif_rotate_enable = gtk_toggle_action_get_active(action);
	layout_image_reset_orientation(lw);
}

static void layout_menu_config_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	show_config_window();
}

static void layout_menu_editors_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	show_editor_list_window();
}

static void layout_menu_layout_config_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	layout_show_config_window(lw);
}

static void layout_menu_remove_thumb_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	cache_manager_show();
}

static void layout_menu_wallpaper_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_to_root(lw);
}

/* single window zoom */
static void layout_menu_zoom_in_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_adjust(lw, get_zoom_increment(), FALSE);
}

static void layout_menu_zoom_out_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_adjust(lw, -get_zoom_increment(), FALSE);
}

static void layout_menu_zoom_1_1_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set(lw, 1.0, FALSE);
}

static void layout_menu_zoom_fit_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set(lw, 0.0, FALSE);
}

static void layout_menu_zoom_fit_hor_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set_fill_geometry(lw, FALSE, FALSE);
}

static void layout_menu_zoom_fit_vert_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set_fill_geometry(lw, TRUE, FALSE);
}

static void layout_menu_zoom_2_1_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set(lw, 2.0, FALSE);
}

static void layout_menu_zoom_3_1_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set(lw, 3.0, FALSE);
}
static void layout_menu_zoom_4_1_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set(lw, 4.0, FALSE);
}

static void layout_menu_zoom_1_2_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set(lw, -2.0, FALSE);
}

static void layout_menu_zoom_1_3_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set(lw, -3.0, FALSE);
}

static void layout_menu_zoom_1_4_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set(lw, -4.0, FALSE);
}

/* connected zoom */
static void layout_menu_connect_zoom_in_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_adjust(lw, get_zoom_increment(), TRUE);
}

static void layout_menu_connect_zoom_out_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_adjust(lw, -get_zoom_increment(), TRUE);
}

static void layout_menu_connect_zoom_1_1_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set(lw, 1.0, TRUE);
}

static void layout_menu_connect_zoom_fit_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set(lw, 0.0, TRUE);
}

static void layout_menu_connect_zoom_fit_hor_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set_fill_geometry(lw, FALSE, TRUE);
}

static void layout_menu_connect_zoom_fit_vert_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set_fill_geometry(lw, TRUE, TRUE);
}

static void layout_menu_connect_zoom_2_1_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set(lw, 2.0, TRUE);
}

static void layout_menu_connect_zoom_3_1_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set(lw, 3.0, TRUE);
}
static void layout_menu_connect_zoom_4_1_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set(lw, 4.0, TRUE);
}

static void layout_menu_connect_zoom_1_2_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set(lw, -2.0, TRUE);
}

static void layout_menu_connect_zoom_1_3_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set(lw, -3.0, TRUE);
}

static void layout_menu_connect_zoom_1_4_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_zoom_set(lw, -4.0, TRUE);
}


static void layout_menu_split_cb(GtkRadioAction *action, GtkRadioAction *current, gpointer data)
{
	LayoutWindow *lw = data;
	ImageSplitMode mode;

	layout_exit_fullscreen(lw);
	mode = gtk_radio_action_get_current_value(action);
	layout_split_change(lw, mode);
}


static void layout_menu_thumb_cb(GtkToggleAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_thumb_set(lw, gtk_toggle_action_get_active(action));
}


static void layout_menu_list_cb(GtkRadioAction *action, GtkRadioAction *current, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	layout_views_set(lw, lw->options.dir_view_type, (FileViewType) gtk_radio_action_get_current_value(action));
}

static void layout_menu_view_dir_as_cb(GtkToggleAction *action,  gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);

	if (gtk_toggle_action_get_active(action))
		{
		layout_views_set(lw, DIRVIEW_TREE, lw->options.file_view_type);
		}
	else
		{
		layout_views_set(lw, DIRVIEW_LIST, lw->options.file_view_type);
		}
}

static void layout_menu_view_in_new_window_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	view_window_new(layout_image_get_fd(lw));
}

static void layout_menu_fullscreen_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_full_screen_toggle(lw);
}

static void layout_menu_escape_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
}

static void layout_menu_overlay_toggle_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	image_osd_toggle(lw->image);
	layout_util_sync_views(lw);
}


static void layout_menu_overlay_cb(GtkToggleAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	if (gtk_toggle_action_get_active(action))
		{
		OsdShowFlags flags = image_osd_get(lw->image);

		if ((flags | OSD_SHOW_INFO | OSD_SHOW_STATUS) != flags)
			image_osd_set(lw->image, flags | OSD_SHOW_INFO | OSD_SHOW_STATUS);
		}
	else
		{
		GtkToggleAction *histogram_action = GTK_TOGGLE_ACTION(gtk_action_group_get_action(lw->action_group, "ImageHistogram"));

		image_osd_set(lw->image, OSD_SHOW_NOTHING);
		gtk_toggle_action_set_active(histogram_action, FALSE); /* this calls layout_menu_histogram_cb */
		}
}

static void layout_menu_histogram_cb(GtkToggleAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	if (gtk_toggle_action_get_active(action))
		{
		image_osd_set(lw->image, OSD_SHOW_INFO | OSD_SHOW_STATUS | OSD_SHOW_HISTOGRAM);
		layout_util_sync_views(lw); /* show the overlay state, default channel and mode in the menu */
		}
	else
		{
		OsdShowFlags flags = image_osd_get(lw->image);
		if (flags & OSD_SHOW_HISTOGRAM)
			image_osd_set(lw->image, flags & ~OSD_SHOW_HISTOGRAM);
		}
}

static void layout_menu_animate_cb(GtkToggleAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	if (lw->options.animate == gtk_toggle_action_get_active(action)) return;
	layout_image_animate_toggle(lw);
}

static void layout_menu_rectangular_selection_cb(GtkToggleAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	options->collections.rectangular_selection = gtk_toggle_action_get_active(action);
}

static void layout_menu_histogram_toggle_channel_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	image_osd_histogram_toggle_channel(lw->image);
	layout_util_sync_views(lw);
}

static void layout_menu_histogram_toggle_mode_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	image_osd_histogram_toggle_mode(lw->image);
	layout_util_sync_views(lw);
}

static void layout_menu_histogram_channel_cb(GtkRadioAction *action, GtkRadioAction *current, gpointer data)
{
	LayoutWindow *lw = data;
	gint channel = gtk_radio_action_get_current_value(action);
	GtkToggleAction *histogram_action = GTK_TOGGLE_ACTION(gtk_action_group_get_action(lw->action_group, "ImageHistogram"));

	if (channel < 0 || channel >= HCHAN_COUNT) return;

	gtk_toggle_action_set_active(histogram_action, TRUE); /* this calls layout_menu_histogram_cb */
	image_osd_histogram_set_channel(lw->image, channel);
}

static void layout_menu_histogram_mode_cb(GtkRadioAction *action, GtkRadioAction *current, gpointer data)
{
	LayoutWindow *lw = data;
	gint mode = gtk_radio_action_get_current_value(action);
	GtkToggleAction *histogram_action = GTK_TOGGLE_ACTION(gtk_action_group_get_action(lw->action_group, "ImageHistogram"));

	if (mode < 0 || mode > 1) return;

	gtk_toggle_action_set_active(histogram_action, TRUE); /* this calls layout_menu_histogram_cb */
	image_osd_histogram_set_mode(lw->image, mode);
}

static void layout_menu_refresh_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_refresh(lw);
}

static void layout_menu_bar_exif_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	layout_exif_window_new(lw);
}

static void layout_menu_float_cb(GtkToggleAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	if (lw->options.tools_float == gtk_toggle_action_get_active(action)) return;

	layout_exit_fullscreen(lw);
	layout_tools_float_toggle(lw);
}

static void layout_menu_hide_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	layout_tools_hide_toggle(lw);
}

static void layout_menu_toolbar_cb(GtkToggleAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	if (lw->options.toolbar_hidden == gtk_toggle_action_get_active(action)) return;

	layout_exit_fullscreen(lw);
	layout_toolbar_toggle(lw);
}

static void layout_menu_info_pixel_cb(GtkToggleAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	if (lw->options.show_info_pixel == gtk_toggle_action_get_active(action)) return;

	layout_exit_fullscreen(lw);
	layout_info_pixel_set(lw, !lw->options.show_info_pixel);
}

/* NOTE: these callbacks are called also from layout_util_sync_views */
static void layout_menu_bar_cb(GtkToggleAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	if (layout_bar_enabled(lw) == gtk_toggle_action_get_active(action)) return;

	layout_exit_fullscreen(lw);
	layout_bar_toggle(lw);
}

static void layout_menu_bar_sort_cb(GtkToggleAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	if (layout_bar_sort_enabled(lw) == gtk_toggle_action_get_active(action)) return;

	layout_exit_fullscreen(lw);
	layout_bar_sort_toggle(lw);
}

static void layout_menu_slideshow_cb(GtkToggleAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	if (layout_image_slideshow_active(lw) == gtk_toggle_action_get_active(action)) return;
	layout_image_slideshow_toggle(lw);
}

static void layout_menu_slideshow_pause_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_image_slideshow_pause_toggle(lw);
}


static void layout_menu_stereo_mode_next_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	gint mode = layout_image_stereo_pixbuf_get(lw);

	/* 0->1, 1->2, 2->3, 3->1 - disable auto, then cycle */
	mode = mode % 3 + 1;

	GtkAction *radio = gtk_action_group_get_action(lw->action_group, "StereoAuto");
	gtk_radio_action_set_current_value(GTK_RADIO_ACTION(radio), mode);

	/*
	this is called via fallback in layout_menu_stereo_mode_cb
	layout_image_stereo_pixbuf_set(lw, mode);
	*/

}

static void layout_menu_stereo_mode_cb(GtkRadioAction *action, GtkRadioAction *current, gpointer data)
{
	LayoutWindow *lw = data;
	gint mode = gtk_radio_action_get_current_value(action);
	layout_image_stereo_pixbuf_set(lw, mode);
}

static void layout_menu_help_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	help_window_show("index.html");
}

static void layout_menu_help_keys_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	help_window_show("GuideReferenceKeyboardShortcuts.html");
}

static void layout_menu_notes_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	help_window_show("release_notes");
}

static char *keyboard_map_hardcoded[][2] = {
	{"Scroll","Left"},
	{"FastScroll", "&lt;Shift&gt;Left"},
	{"Left Border", "&lt;Primary&gt;Left"},
	{"Left Border", "&lt;Primary&gt;&lt;Shift&gt;Left"},
	{"Scroll", "Right"},
	{"FastScroll", "&lt;Shift&gt;Right"},
	{"Right Border", "&lt;Primary&gt;Right"},
	{"Right Border", "&lt;Primary&gt;&lt;Shift&gt;Right"},
	{"Scroll", "Up"},
	{"FastScroll", "&lt;Shift&gt;Up"},
	{"Uper Border", "&lt;Primary&gt;Up"},
	{"Uper Border", "&lt;Primary&gt;&lt;Shift&gt;Up"},
	{"Scroll", "Down"},
	{"FastScroll", "&lt;Shift&gt;Down"},
	{"Lower Border", "&lt;Primary&gt;Down"},
	{"Lower Border", "&lt;Primary&gt;&lt;Shift&gt;Down"},
	{"Next/Drag", "M1"},
	{"FastDrag", "&lt;Shift&gt;M1"},
	{"DnD Start", "M2"},
	{"Menu", "M3"},
	{"PrevImage", "MW4"},
	{"NextImage", "MW5"},
	{"ScrollUp", "&lt;Shift&gt;MW4"},
	{"ScrollDown", "&lt;Shift&gt;MW5"},
	{"ZoomIn", "&lt;Primary&gt;MW4"},
	{"ZoomOut", "&lt;Primary&gt;MW5"},
	{NULL, NULL}
};

static void layout_menu_foreach_func(
					gpointer data,
					const gchar *accel_path,
					guint accel_key,
					GdkModifierType accel_mods,
					gboolean changed)
{
	gchar *path, *name;
	gchar *key_name, *menu_name;
	gchar **subset_lt_arr, **subset_gt_arr;
	gchar *subset_lt, *converted_name;
	GPtrArray *array = data;

	path = g_strescape(accel_path, NULL);
	name = gtk_accelerator_name(accel_key, accel_mods);

	menu_name = g_strdup(g_strrstr(path, "/")+1);

	if (g_strrstr(name, ">"))
		{
		subset_lt_arr = g_strsplit_set(name,"<", 4);
		subset_lt = g_strjoinv("&lt;", subset_lt_arr);
		subset_gt_arr = g_strsplit_set(subset_lt,">", 4);
		converted_name = g_strjoinv("&gt;", subset_gt_arr);
		key_name = g_strdup(converted_name);

		g_free(converted_name);
		g_free(subset_lt);
		g_strfreev(subset_lt_arr);
		g_strfreev(subset_gt_arr);
		}
	else
		key_name = g_strdup(name);

	g_ptr_array_add(array, (gpointer)menu_name);
	g_ptr_array_add(array, (gpointer)key_name);

	g_free(name);
	g_free(path);
}

static void layout_menu_kbd_map_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	gint fd = -1;
	GPtrArray *array;
	char * tmp_file;
	GError *error = NULL;
	GIOChannel *channel;
	char **pre_key, **post_key;
	char *key_name, *converted_line;
	int keymap_index, index;

	fd = g_file_open_tmp("geeqie_keymap_XXXXXX.svg", &tmp_file, &error);
	if (error)
		{
		DEBUG_0("Keyboard Map - cannot create file:%s\n",error->message);
		g_error_free(error);
		}
	else
		{
		array = g_ptr_array_new();

		gtk_accel_map_foreach(array, layout_menu_foreach_func);

		channel = g_io_channel_unix_new(fd);

		keymap_index = 0;
		while (keymap_template[keymap_index])
			{
			if (g_strrstr(keymap_template[keymap_index], ">key:"))
				{
				pre_key = g_strsplit(keymap_template[keymap_index],">key:",2);
				post_key = g_strsplit(pre_key[1],"<",2);

				index=0;
				key_name = " ";
				for (index=0; index < array->len-2; index=index+2)
					{
					if (!(g_ascii_strcasecmp(g_ptr_array_index(array,index+1), post_key[0])))
						{
						key_name = g_ptr_array_index(array,index+0);
						break;
						}
					}

				index=0;
				while (keyboard_map_hardcoded[index][0])
					{
					if (!(g_strcmp0(keyboard_map_hardcoded[index][1], post_key[0])))
						{
						key_name = keyboard_map_hardcoded[index][0];
						break;
						}
					index++;
					}

				converted_line = g_strconcat(pre_key[0], ">", key_name, "<", post_key[1], "\n", NULL);
				g_io_channel_write_chars(channel, converted_line, -1, NULL, &error);
				if (error) {DEBUG_0("Keyboard Map:%s\n",error->message); g_error_free(error);}

				g_free(converted_line);
				g_strfreev(pre_key);
				g_strfreev(post_key);
				}
			else
				{
				g_io_channel_write_chars(channel, keymap_template[keymap_index], -1, NULL, &error);
				if (error) {DEBUG_0("Keyboard Map:%s\n",error->message); g_error_free(error);}
				g_io_channel_write_chars(channel, "\n", -1, NULL, &error);
				if (error) {DEBUG_0("Keyboard Map:%s\n",error->message); g_error_free(error);}
				}
			keymap_index++;
			}

		g_io_channel_flush(channel, &error);
		if (error) {DEBUG_0("Keyboard Map:%s\n",error->message); g_error_free(error);}
		g_io_channel_unref(channel);

		index=0;
		for (index=0; index < array->len-2; index=index+2)
			{
			g_free(g_ptr_array_index(array,index));
			g_free(g_ptr_array_index(array,index+1));
			}
		g_ptr_array_unref(array);

		view_window_new(file_data_new_simple(tmp_file));
		g_free(tmp_file);
		}
}

static void layout_menu_about_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	show_about_window();
}

static void layout_menu_log_window_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_exit_fullscreen(lw);
	log_window_new();
}


/*
 *-----------------------------------------------------------------------------
 * select menu
 *-----------------------------------------------------------------------------
 */

static void layout_menu_select_all_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_select_all(lw);
}

static void layout_menu_unselect_all_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_select_none(lw);
}

static void layout_menu_invert_selection_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_select_invert(lw);
}

static void layout_menu_marks_cb(GtkToggleAction *action, gpointer data)
{
	LayoutWindow *lw = data;

	layout_marks_set(lw, gtk_toggle_action_get_active(action));
}


static void layout_menu_set_mark_sel_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	gint mark = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(action), "mark_num"));
	g_assert(mark >= 1 && mark <= FILEDATA_MARKS_SIZE);

	layout_selection_to_mark(lw, mark, STM_MODE_SET);
}

static void layout_menu_res_mark_sel_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	gint mark = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(action), "mark_num"));
	g_assert(mark >= 1 && mark <= FILEDATA_MARKS_SIZE);

	layout_selection_to_mark(lw, mark, STM_MODE_RESET);
}

static void layout_menu_toggle_mark_sel_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	gint mark = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(action), "mark_num"));
	g_assert(mark >= 1 && mark <= FILEDATA_MARKS_SIZE);

	layout_selection_to_mark(lw, mark, STM_MODE_TOGGLE);
}

static void layout_menu_sel_mark_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	gint mark = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(action), "mark_num"));
	g_assert(mark >= 1 && mark <= FILEDATA_MARKS_SIZE);

	layout_mark_to_selection(lw, mark, MTS_MODE_SET);
}

static void layout_menu_sel_mark_or_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	gint mark = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(action), "mark_num"));
	g_assert(mark >= 1 && mark <= FILEDATA_MARKS_SIZE);

	layout_mark_to_selection(lw, mark, MTS_MODE_OR);
}

static void layout_menu_sel_mark_and_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	gint mark = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(action), "mark_num"));
	g_assert(mark >= 1 && mark <= FILEDATA_MARKS_SIZE);

	layout_mark_to_selection(lw, mark, MTS_MODE_AND);
}

static void layout_menu_sel_mark_minus_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	gint mark = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(action), "mark_num"));
	g_assert(mark >= 1 && mark <= FILEDATA_MARKS_SIZE);

	layout_mark_to_selection(lw, mark, MTS_MODE_MINUS);
}

static void layout_menu_mark_filter_toggle_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	gint mark = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(action), "mark_num"));
	g_assert(mark >= 1 && mark <= FILEDATA_MARKS_SIZE);

	layout_marks_set(lw, TRUE);
	layout_mark_filter_toggle(lw, mark);
}


/*
 *-----------------------------------------------------------------------------
 * go menu
 *-----------------------------------------------------------------------------
 */

static void layout_menu_image_first_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	layout_image_first(lw);
}

static void layout_menu_image_prev_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	layout_image_prev(lw);
}

static void layout_menu_image_next_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	layout_image_next(lw);
}

static void layout_menu_image_last_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	layout_image_last(lw);
}

static void layout_menu_back_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	FileData *dir_fd;
	gchar *path = NULL;
	GList *list = history_list_get_by_key("path_list");
	gint n = 0;

	while (list)
		{
		if (n == 1) {
			/* Previous path from history */
			path = (gchar *)list->data;
			break;
		}
		list = list->next;
		n++;
		}

	if (!path) return;

	/* Open previous path */
	dir_fd = file_data_new_dir(path);
	layout_set_fd(lw, dir_fd);
	file_data_unref(dir_fd);
}

static void layout_menu_home_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	const gchar *path;

	if (lw->options.home_path && *lw->options.home_path)
		path = lw->options.home_path;
	else
		path = homedir();

	if (path)
		{
		FileData *dir_fd = file_data_new_dir(path);
		layout_set_fd(lw, dir_fd);
		file_data_unref(dir_fd);
		}
}

static void layout_menu_up_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	ViewDir *vd = lw->vd;
	gchar *path;

	if (!vd->dir_fd || strcmp(vd->dir_fd->path, G_DIR_SEPARATOR_S) == 0) return;
	path = remove_level_from_path(vd->dir_fd->path);

	if (vd->select_func)
		{
		FileData *fd = file_data_new_dir(path);
		vd->select_func(vd, fd, vd->select_data);
		file_data_unref(fd);
		}

	g_free(path);
}


/*
 *-----------------------------------------------------------------------------
 * edit menu
 *-----------------------------------------------------------------------------
 */

static void layout_menu_edit_cb(GtkAction *action, gpointer data)
{
	LayoutWindow *lw = data;
	const gchar *key = gtk_action_get_name(action);

	if (!editor_window_flag_set(key))
		layout_exit_fullscreen(lw);

	file_util_start_editor_from_filelist(key, layout_selection_list(lw), layout_get_path(lw), lw->window);
}


static void layout_menu_metadata_write_cb(GtkAction *action, gpointer data)
{
	metadata_write_queue_confirm(TRUE, NULL, NULL);
}


/*
 *-----------------------------------------------------------------------------
 * color profile button (and menu)
 *-----------------------------------------------------------------------------
 */

static void layout_color_menu_enable_cb(GtkToggleAction *action, gpointer data)
{
#ifdef HAVE_LCMS
	LayoutWindow *lw = data;

	if (layout_image_color_profile_get_use(lw) == gtk_toggle_action_get_active(action)) return;

	layout_image_color_profile_set_use(lw, gtk_toggle_action_get_active(action));
	layout_util_sync_color(lw);
	layout_image_refresh(lw);
#endif
}

static void layout_color_menu_use_image_cb(GtkToggleAction *action, gpointer data)
{
#ifdef HAVE_LCMS
	LayoutWindow *lw = data;
	gint input;
	gboolean use_image;

	if (!layout_image_color_profile_get(lw, &input, &use_image)) return;
	if (use_image == gtk_toggle_action_get_active(action)) return;
	layout_image_color_profile_set(lw, input, gtk_toggle_action_get_active(action));
	layout_util_sync_color(lw);
	layout_image_refresh(lw);
#endif
}

static void layout_color_menu_input_cb(GtkRadioAction *action, GtkRadioAction *current, gpointer data)
{
#ifdef HAVE_LCMS
	LayoutWindow *lw = data;
	gint type;
	gint input;
	gboolean use_image;

	type = gtk_radio_action_get_current_value(action);
	if (type < 0 || type >= COLOR_PROFILE_FILE + COLOR_PROFILE_INPUTS) return;

	if (!layout_image_color_profile_get(lw, &input, &use_image)) return;
	if (type == input) return;

	layout_image_color_profile_set(lw, type, use_image);
	layout_image_refresh(lw);
#endif
}


/*
 *-----------------------------------------------------------------------------
 * recent menu
 *-----------------------------------------------------------------------------
 */

static void layout_menu_recent_cb(GtkWidget *widget, gpointer data)
{
	gint n;
	gchar *path;

	n = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "recent_index"));

	path = g_list_nth_data(history_list_get_by_key("recent"), n);

	if (!path) return;

	/* make a copy of it */
	path = g_strdup(path);
	collection_window_new(path);
	g_free(path);
}

static void layout_menu_recent_update(LayoutWindow *lw)
{
	GtkWidget *menu;
	GtkWidget *recent;
	GtkWidget *item;
	GList *list;
	gint n;

	if (!lw->ui_manager) return;

	list = history_list_get_by_key("recent");
	n = 0;

	menu = gtk_menu_new();

	while (list)
		{
		const gchar *filename = filename_from_path((gchar *)list->data);
		gchar *name;
		gboolean free_name = FALSE;

		if (file_extension_match(filename, GQ_COLLECTION_EXT))
			{
			name = remove_extension_from_path(filename);
			free_name = TRUE;
			}
		else
			{
			name = (gchar *) filename;
			}

		item = menu_item_add_simple(menu, name, G_CALLBACK(layout_menu_recent_cb), lw);
		if (free_name) g_free(name);
		g_object_set_data(G_OBJECT(item), "recent_index", GINT_TO_POINTER(n));
		list = list->next;
		n++;
		}

	if (n == 0)
		{
		menu_item_add(menu, _("Empty"), NULL, NULL);
		}

	recent = gtk_ui_manager_get_widget(lw->ui_manager, "/MainMenu/FileMenu/OpenRecent");
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(recent), menu);
	gtk_widget_set_sensitive(recent, (n != 0));
}

void layout_recent_update_all(void)
{
	GList *work;

	work = layout_window_list;
	while (work)
		{
		LayoutWindow *lw = work->data;
		work = work->next;

		layout_menu_recent_update(lw);
		}
}

void layout_recent_add_path(const gchar *path)
{
	if (!path) return;

	history_list_add_to_key("recent", path, options->open_recent_list_maxsize);

	layout_recent_update_all();
}

/*
 *-----------------------------------------------------------------------------
 * menu
 *-----------------------------------------------------------------------------
 */

#define CB G_CALLBACK

static GtkActionEntry menu_entries[] = {
  { "FileMenu",		NULL,			N_("_File"),				NULL,			NULL,					NULL },
  { "GoMenu",		NULL,			N_("_Go"),				NULL,			NULL,					NULL },
  { "EditMenu",		NULL,			N_("_Edit"),				NULL,			NULL,					NULL },
  { "SelectMenu",	NULL,			N_("_Select"),				NULL,			NULL,					NULL },
  { "OrientationMenu",	NULL,			N_("_Orientation"),			NULL,			NULL,					NULL },
  { "ExternalMenu",	NULL,			N_("E_xternal Editors"),		NULL,			NULL,					NULL },
  { "PreferencesMenu",	NULL,			N_("P_references"),			NULL,			NULL,					NULL },
  { "ViewMenu",		NULL,			N_("_View"),				NULL,			NULL,					NULL },
  { "FileDirMenu",	NULL,			N_("_Files and Folders"),		NULL,			NULL,					NULL },
  { "ZoomMenu",		NULL,			N_("_Zoom"),				NULL,			NULL,					NULL },
  { "ColorMenu",	NULL,			N_("_Color Management"),		NULL,			NULL,					NULL },
  { "ConnectZoomMenu",	NULL,			N_("_Connected Zoom"),			NULL,			NULL,					NULL },
  { "SplitMenu",	NULL,			N_("Spli_t"),				NULL,			NULL,					NULL },
  { "StereoMenu",	NULL,			N_("Stere_o"),				NULL,			NULL,					NULL },
  { "OverlayMenu",	NULL,			N_("Image _Overlay"),			NULL,			NULL,					NULL },
  { "HelpMenu",		NULL,			N_("_Help"),				NULL,			NULL,					NULL },

  { "FirstImage",	GTK_STOCK_GOTO_TOP,	N_("_First Image"),			"Home",			N_("First Image"),			CB(layout_menu_image_first_cb) },
  { "PrevImage",	GTK_STOCK_GO_UP,	N_("_Previous Image"),			"BackSpace",		N_("Previous Image"),			CB(layout_menu_image_prev_cb) },
  { "PrevImageAlt1",	GTK_STOCK_GO_UP,	N_("_Previous Image"),			"Page_Up",		N_("Previous Image"),			CB(layout_menu_image_prev_cb) },
  { "PrevImageAlt2",	GTK_STOCK_GO_UP,	N_("_Previous Image"),			"KP_Page_Up",		N_("Previous Image"),			CB(layout_menu_image_prev_cb) },
  { "NextImage",	GTK_STOCK_GO_DOWN,	N_("_Next Image"),			"space",		N_("Next Image"),			CB(layout_menu_image_next_cb) },
  { "NextImageAlt1",	GTK_STOCK_GO_DOWN,	N_("_Next Image"),			"Page_Down",		N_("Next Image"),			CB(layout_menu_image_next_cb) },
  { "NextImageAlt2",	GTK_STOCK_GO_DOWN,	N_("_Next Image"),			"KP_Page_Down",		N_("Next Image"),			CB(layout_menu_image_next_cb) },
  { "LastImage",	GTK_STOCK_GOTO_BOTTOM,	N_("_Last Image"),			"End",			N_("Last Image"),			CB(layout_menu_image_last_cb) },
  { "Back",		GTK_STOCK_GO_BACK,	N_("_Back"),				NULL,			N_("Back"),				CB(layout_menu_back_cb) },
  { "Home",		GTK_STOCK_HOME,		N_("_Home"),				NULL,			N_("Home"),				CB(layout_menu_home_cb) },
  { "Up",		GTK_STOCK_GO_UP,	N_("_Up"),				NULL,			N_("Up"),				CB(layout_menu_up_cb) },

  { "NewWindow",	GTK_STOCK_NEW,		N_("New _window"),			"<control>N",		N_("New window"),			CB(layout_menu_new_window_cb) },
  { "NewCollection",	GTK_STOCK_INDEX,	N_("_New collection"),			"C",			N_("New collection"),			CB(layout_menu_new_cb) },
  { "OpenCollection",	GTK_STOCK_OPEN,		N_("_Open collection..."),		"O",			N_("Open collection..."),		CB(layout_menu_open_cb) },
  { "OpenRecent",	NULL,			N_("Open recen_t"),			NULL,			N_("Open recent"),			NULL },
  { "Search",		GTK_STOCK_FIND,		N_("_Search..."),			"F3",			N_("Search..."),			CB(layout_menu_search_cb) },
  { "FindDupes",	GTK_STOCK_FIND,		N_("_Find duplicates..."),		"D",			N_("Find duplicates..."),		CB(layout_menu_dupes_cb) },
  { "PanView",		NULL,			N_("Pa_n view"),			"<control>J",		N_("Pan view"),				CB(layout_menu_pan_cb) },
  { "Print",		GTK_STOCK_PRINT,	N_("_Print..."),			"<shift>P",		N_("Print..."),				CB(layout_menu_print_cb) },
  { "NewFolder",	GTK_STOCK_DIRECTORY,	N_("N_ew folder..."),			"<control>F",		N_("New folder..."),			CB(layout_menu_dir_cb) },
  { "Copy",		GTK_STOCK_COPY,		N_("_Copy..."),				"<control>C",		N_("Copy..."),				CB(layout_menu_copy_cb) },
  { "Move",		NULL,			N_("_Move..."),				"<control>M",		N_("Move..."),				CB(layout_menu_move_cb) },
  { "Rename",		NULL,			N_("_Rename..."),			"<control>R",		N_("Rename..."),			CB(layout_menu_rename_cb) },
  { "Delete",		GTK_STOCK_DELETE,	N_("_Delete..."),			"<control>D",		N_("Delete..."),			CB(layout_menu_delete_cb) },
  { "DeleteAlt1",	GTK_STOCK_DELETE,	N_("_Delete..."),			"Delete",		N_("Delete..."),			CB(layout_menu_delete_key_cb) },
  { "DeleteAlt2",	GTK_STOCK_DELETE,	N_("_Delete..."),			"KP_Delete",		N_("Delete..."),			CB(layout_menu_delete_key_cb) },
  { "EnableGrouping",	NULL,			N_("Enable file _grouping"),		NULL,			N_("Enable file grouping"),		CB(layout_menu_enable_grouping_cb) },
  { "DisableGrouping",	NULL,			N_("Disable file groupi_ng"),		NULL,			N_("Disable file grouping"),		CB(layout_menu_disable_grouping_cb) },
  { "CopyPath",		NULL,			N_("_Copy path to clipboard"),		NULL,			N_("Copy path to clipboard"),		CB(layout_menu_copy_path_cb) },
  { "CloseWindow",	GTK_STOCK_CLOSE,	N_("C_lose window"),			"<control>W",		N_("Close window"),			CB(layout_menu_close_cb) },
  { "Quit",		GTK_STOCK_QUIT, 	N_("_Quit"),				"<control>Q",		N_("Quit"),				CB(layout_menu_exit_cb) },
  { "RotateCW",		NULL,			N_("_Rotate clockwise"),		"bracketright",		N_("Rotate clockwise"),			CB(layout_menu_alter_90_cb) },
  { "RotateCCW",	NULL,			N_("Rotate _counterclockwise"),		"bracketleft",		N_("Rotate counterclockwise"),		CB(layout_menu_alter_90cc_cb) },
  { "Rotate180",	NULL,			N_("Rotate 1_80"),			"<shift>R",		N_("Rotate 180"),			CB(layout_menu_alter_180_cb) },
  { "Mirror",		NULL,			N_("_Mirror"),				"<shift>M",		N_("Mirror"),				CB(layout_menu_alter_mirror_cb) },
  { "Flip",		NULL,			N_("_Flip"),				"<shift>F",		N_("Flip"),				CB(layout_menu_alter_flip_cb) },
  { "AlterNone",	NULL,			N_("_Original state"),  		"<shift>O",		N_("Original state"),			CB(layout_menu_alter_none_cb) },
  { "SelectAll",	NULL,			N_("Select _all"),			"<control>A",		N_("Select all"),			CB(layout_menu_select_all_cb) },
  { "SelectNone",	NULL,			N_("Select _none"),			"<control><shift>A",	N_("Select none"),			CB(layout_menu_unselect_all_cb) },
  { "SelectInvert",	NULL,			N_("_Invert Selection"),		"<control><shift>I",	N_("Invert Selection"),			CB(layout_menu_invert_selection_cb) },
  { "Preferences",	GTK_STOCK_PREFERENCES,	N_("P_references..."),			"<control>O",		N_("Preferences..."),			CB(layout_menu_config_cb) },
  { "Editors",		GTK_STOCK_PREFERENCES,	N_("Configure _Editors..."),		NULL,			N_("Configure Editors..."),		CB(layout_menu_editors_cb) },
  { "LayoutConfig",	GTK_STOCK_PREFERENCES,	N_("_Configure this window..."),	NULL,			N_("Configure this window..."),		CB(layout_menu_layout_config_cb) },
  { "Maintenance",	NULL,			N_("_Thumbnail maintenance..."),	NULL,			N_("Thumbnail maintenance..."),		CB(layout_menu_remove_thumb_cb) },
  { "Wallpaper",	NULL,			N_("Set as _wallpaper"),		NULL,			N_("Set as wallpaper"),			CB(layout_menu_wallpaper_cb) },
  { "SaveMetadata",	GTK_STOCK_SAVE,		N_("_Save metadata"),			"<control>S",		N_("Save metadata"),			CB(layout_menu_metadata_write_cb) },
  { "ZoomIn",		GTK_STOCK_ZOOM_IN,	N_("Zoom _in"),				"equal",		N_("Zoom in"),				CB(layout_menu_zoom_in_cb) },
  { "ZoomInAlt1",	GTK_STOCK_ZOOM_IN,	N_("Zoom _in"),				"KP_Add",		N_("Zoom in"),				CB(layout_menu_zoom_in_cb) },
  { "ZoomOut",		GTK_STOCK_ZOOM_OUT,	N_("Zoom _out"),			"minus",		N_("Zoom out"),				CB(layout_menu_zoom_out_cb) },
  { "ZoomOutAlt1",	GTK_STOCK_ZOOM_OUT,	N_("Zoom _out"),			"KP_Subtract",		N_("Zoom out"),				CB(layout_menu_zoom_out_cb) },
  { "Zoom100",		GTK_STOCK_ZOOM_100,	N_("Zoom _1:1"),			"Z",			N_("Zoom 1:1"),				CB(layout_menu_zoom_1_1_cb) },
  { "Zoom100Alt1",	GTK_STOCK_ZOOM_100,	N_("Zoom _1:1"),			"KP_Divide",		N_("Zoom 1:1"),				CB(layout_menu_zoom_1_1_cb) },
  { "ZoomFit",		GTK_STOCK_ZOOM_FIT,	N_("_Zoom to fit"),			"X",			N_("Zoom to fit"),			CB(layout_menu_zoom_fit_cb) },
  { "ZoomFitAlt1",	GTK_STOCK_ZOOM_FIT,	N_("_Zoom to fit"),			"KP_Multiply",		N_("Zoom to fit"),			CB(layout_menu_zoom_fit_cb) },
  { "ZoomFillHor",	NULL,			N_("Fit _Horizontally"),		"H",			N_("Fit Horizontally"),			CB(layout_menu_zoom_fit_hor_cb) },
  { "ZoomFillVert",	NULL,			N_("Fit _Vertically"),			"W",			N_("Fit Vertically"),			CB(layout_menu_zoom_fit_vert_cb) },
  { "Zoom200",	        NULL,			N_("Zoom _2:1"),			NULL,			N_("Zoom 2:1"),				CB(layout_menu_zoom_2_1_cb) },
  { "Zoom300",	        NULL,			N_("Zoom _3:1"),			NULL,			N_("Zoom 3:1"),				CB(layout_menu_zoom_3_1_cb) },
  { "Zoom400",		NULL,			N_("Zoom _4:1"),			NULL,			N_("Zoom 4:1"),				CB(layout_menu_zoom_4_1_cb) },
  { "Zoom50",		NULL,			N_("Zoom 1:2"),				NULL,			N_("Zoom 1:2"),				CB(layout_menu_zoom_1_2_cb) },
  { "Zoom33",		NULL,			N_("Zoom 1:3"),				NULL,			N_("Zoom 1:3"),				CB(layout_menu_zoom_1_3_cb) },
  { "Zoom25",		NULL,			N_("Zoom 1:4"),				NULL,			N_("Zoom 1:4"),				CB(layout_menu_zoom_1_4_cb) },
  { "ConnectZoomIn",	GTK_STOCK_ZOOM_IN,	N_("Zoom _in"),				"plus",			N_("Connected Zoom in"),		CB(layout_menu_connect_zoom_in_cb) },
  { "ConnectZoomInAlt1",GTK_STOCK_ZOOM_IN,	N_("Zoom _in"),				"<shift>KP_Add",	N_("Connected Zoom in"),		CB(layout_menu_connect_zoom_in_cb) },
  { "ConnectZoomOut",	GTK_STOCK_ZOOM_OUT,	N_("Zoom _out"),			"underscore",		N_("Connected Zoom out"),		CB(layout_menu_connect_zoom_out_cb) },
  { "ConnectZoomOutAlt1",GTK_STOCK_ZOOM_OUT,	N_("Zoom _out"),			"<shift>KP_Subtract",	N_("Connected Zoom out"),		CB(layout_menu_connect_zoom_out_cb) },
  { "ConnectZoom100",	GTK_STOCK_ZOOM_100,	N_("Zoom _1:1"),			"<shift>Z",		N_("Connected Zoom 1:1"),		CB(layout_menu_connect_zoom_1_1_cb) },
  { "ConnectZoom100Alt1",GTK_STOCK_ZOOM_100,	N_("Zoom _1:1"),			"<shift>KP_Divide",	N_("Connected Zoom 1:1"),		CB(layout_menu_connect_zoom_1_1_cb) },
  { "ConnectZoomFit",	GTK_STOCK_ZOOM_FIT,	N_("_Zoom to fit"),			"<shift>X",		N_("Connected Zoom to fit"),		CB(layout_menu_connect_zoom_fit_cb) },
  { "ConnectZoomFitAlt1",GTK_STOCK_ZOOM_FIT,	N_("_Zoom to fit"),			"<shift>KP_Multiply",	N_("Connected Zoom to fit"),		CB(layout_menu_connect_zoom_fit_cb) },
  { "ConnectZoomFillHor",NULL,			N_("Fit _Horizontally"),		"<shift>H",		N_("Connected Fit Horizontally"),	CB(layout_menu_connect_zoom_fit_hor_cb) },
  { "ConnectZoomFillVert",NULL,			N_("Fit _Vertically"),			"<shift>W",		N_("Connected Fit Vertically"),		CB(layout_menu_connect_zoom_fit_vert_cb) },
  { "ConnectZoom200",	NULL,			N_("Zoom _2:1"),			NULL,			N_("Connected Zoom 2:1"),		CB(layout_menu_connect_zoom_2_1_cb) },
  { "ConnectZoom300",	NULL,			N_("Zoom _3:1"),			NULL,			N_("Connected Zoom 3:1"),		CB(layout_menu_connect_zoom_3_1_cb) },
  { "ConnectZoom400",	NULL,			N_("Zoom _4:1"),			NULL,			N_("Connected Zoom 4:1"),		CB(layout_menu_connect_zoom_4_1_cb) },
  { "ConnectZoom50",	NULL,			N_("Zoom 1:2"),				NULL,			N_("Connected Zoom 1:2"),		CB(layout_menu_connect_zoom_1_2_cb) },
  { "ConnectZoom33",	NULL,			N_("Zoom 1:3"),				NULL,			N_("Connected Zoom 1:3"),		CB(layout_menu_connect_zoom_1_3_cb) },
  { "ConnectZoom25",	NULL,			N_("Zoom 1:4"),				NULL,			N_("Connected Zoom 1:4"),		CB(layout_menu_connect_zoom_1_4_cb) },
  { "ViewInNewWindow",	NULL,			N_("_View in new window"),		"<control>V",		N_("View in new window"),		CB(layout_menu_view_in_new_window_cb) },
  { "FullScreen",	GTK_STOCK_FULLSCREEN,	N_("F_ull screen"),			"F",			N_("Full screen"),			CB(layout_menu_fullscreen_cb) },
  { "FullScreenAlt1",	GTK_STOCK_FULLSCREEN,	N_("F_ull screen"),			"V",			N_("Full screen"),			CB(layout_menu_fullscreen_cb) },
  { "FullScreenAlt2",	GTK_STOCK_FULLSCREEN,	N_("F_ull screen"),			"F11",			N_("Full screen"),			CB(layout_menu_fullscreen_cb) },
  { "Escape",		GTK_STOCK_LEAVE_FULLSCREEN,N_("_Leave full screen"),		"Escape",		N_("Leave full screen"),		CB(layout_menu_escape_cb) },
  { "EscapeAlt1",	GTK_STOCK_LEAVE_FULLSCREEN,N_("_Leave full screen"),		"Q",			N_("Leave full screen"),		CB(layout_menu_escape_cb) },
  { "ImageOverlayCycle",NULL,			N_("_Cycle through overlay modes"),	"I",			N_("Cycle through Overlay modes"),	CB(layout_menu_overlay_toggle_cb) },
  { "HistogramChanCycle",NULL,			N_("Cycle through histogram ch_annels"),"K",			N_("Cycle through histogram channels"),	CB(layout_menu_histogram_toggle_channel_cb) },
  { "HistogramModeCycle",NULL,			N_("Cycle through histogram mo_des"),	"J",			N_("Cycle through histogram modes"),	CB(layout_menu_histogram_toggle_mode_cb) },
  { "HideTools",	NULL,			N_("_Hide file list"),			"<control>H",		N_("Hide file list"),			CB(layout_menu_hide_cb) },
  { "SlideShowPause",	GTK_STOCK_MEDIA_PAUSE,	N_("_Pause slideshow"), 		"P",			N_("Pause slideshow"), 			CB(layout_menu_slideshow_pause_cb) },
  { "Refresh",		GTK_STOCK_REFRESH,	N_("_Refresh"),				"R",			N_("Refresh"),				CB(layout_menu_refresh_cb) },
  { "HelpContents",	GTK_STOCK_HELP,		N_("_Contents"),			"F1",			N_("Contents"),				CB(layout_menu_help_cb) },
  { "HelpShortcuts",	NULL,			N_("_Keyboard shortcuts"),		NULL,			N_("Keyboard shortcuts"),		CB(layout_menu_help_keys_cb) },
  { "HelpKbd",		NULL,			N_("_Keyboard map"),			NULL,			N_("Keyboard map"),			CB(layout_menu_kbd_map_cb) },
  { "HelpNotes",	NULL,			N_("_Release notes"),			NULL,			N_("Release notes"),			CB(layout_menu_notes_cb) },
  { "About",		GTK_STOCK_ABOUT,	N_("_About"),				NULL,			N_("About"),				CB(layout_menu_about_cb) },
  { "LogWindow",	NULL,			N_("_Log Window"),			NULL,			N_("Log Window"),			CB(layout_menu_log_window_cb) },
  { "ExifWin",		NULL,			N_("_Exif window"),			"<control>E",		N_("Exif window"),			CB(layout_menu_bar_exif_cb) },
  { "StereoCycle",	NULL,			N_("_Cycle through stereo modes"),	NULL,			N_("Cycle through stereo modes"),	CB(layout_menu_stereo_mode_next_cb) },

};

static GtkToggleActionEntry menu_toggle_entries[] = {
  { "Thumbnails",	PIXBUF_INLINE_ICON_THUMB,N_("Show _Thumbnails"),		"T",			N_("Show Thumbnails"),			CB(layout_menu_thumb_cb),	 FALSE },
  { "ShowMarks",        NULL,			N_("Show _Marks"),			"M",			N_("Show Marks"),			CB(layout_menu_marks_cb),	 FALSE  },
  { "ShowInfoPixel",	GTK_STOCK_COLOR_PICKER,	N_("Pi_xel Info"),			NULL,			N_("Show Pixel Info"),			CB(layout_menu_info_pixel_cb),	 FALSE  },
  { "FloatTools",	PIXBUF_INLINE_ICON_FLOAT,N_("_Float file list"),		"L",			N_("Float file list"),			CB(layout_menu_float_cb),	 FALSE  },
  { "HideToolbar",	NULL,			N_("Hide tool_bar"),			NULL,			N_("Hide toolbar"),			CB(layout_menu_toolbar_cb),	 FALSE  },
  { "SBar",		NULL,			N_("_Info sidebar"),			"<control>K",		N_("Info sidebar"),			CB(layout_menu_bar_cb),		 FALSE  },
  { "SBarSort",		NULL,			N_("Sort _manager"),			"<shift>S",		N_("Sort manager"),			CB(layout_menu_bar_sort_cb),	 FALSE  },
  { "SlideShow",	GTK_STOCK_MEDIA_PLAY,	N_("Toggle _slideshow"),		"S",			N_("Toggle slideshow"),			CB(layout_menu_slideshow_cb),	 FALSE  },
  { "UseColorProfiles",	GTK_STOCK_SELECT_COLOR,	N_("Use _color profiles"), 		NULL,			N_("Use color profiles"), 		CB(layout_color_menu_enable_cb), FALSE},
  { "UseImageProfile",	NULL,			N_("Use profile from _image"),		NULL,			N_("Use profile from image"),		CB(layout_color_menu_use_image_cb), FALSE},
  { "Grayscale",	NULL,			N_("Toggle _grayscale"),		"<shift>G",		N_("Toggle grayscale"),			CB(layout_menu_alter_desaturate_cb), FALSE},
  { "ImageOverlay",	NULL,			N_("Image _Overlay"),			NULL,			N_("Image Overlay"),			CB(layout_menu_overlay_cb),	 FALSE },
  { "ImageHistogram",	NULL,			N_("_Show Histogram"),			NULL,			N_("Show Histogram"),			CB(layout_menu_histogram_cb),	 FALSE },
  { "RectangularSelection",	NULL,			N_("Rectangular Selection"),			"<alt>R",			N_("Rectangular Selection"),			CB(layout_menu_rectangular_selection_cb),	 FALSE },
  { "Animate",	NULL,	N_("GIF _animation"),		"A",			N_("Toggle GIF animation"),			CB(layout_menu_animate_cb),	 FALSE  },
  { "ExifRotate",	GTK_STOCK_ORIENTATION_PORTRAIT,			N_("_Exif rotate"),  		"<alt>X",		N_("Exif rotate"),			CB(layout_menu_exif_rotate_cb), FALSE },
};

static GtkRadioActionEntry menu_radio_entries[] = {
  { "ViewList",		NULL,			N_("Image _List"),			"<control>L",		N_("View Images as List"),		FILEVIEW_LIST },
  { "ViewIcons",	NULL,			N_("I_cons"),				"<control>I",		N_("View Images as Icons"),		FILEVIEW_ICON }
};

static GtkToggleActionEntry menu_view_dir_toggle_entries[] = {
  { "FolderTree",	NULL,			N_("T_oggle Folder View"),			"<control>T",		N_("Toggle Folders View"), 		CB(layout_menu_view_dir_as_cb),FALSE },
};

static GtkRadioActionEntry menu_split_radio_entries[] = {
  { "SplitHorizontal",	NULL,			N_("_Horizontal"),			"E",			N_("Split Horizontal"),			SPLIT_HOR },
  { "SplitVertical",	NULL,			N_("_Vertical"),			"U",			N_("Split Vertical"),				SPLIT_VERT },
  { "SplitQuad",	NULL,			N_("_Quad"),				NULL,			N_("Split Quad"),				SPLIT_QUAD },
  { "SplitSingle",	NULL,			N_("_Single"),				"Y",			N_("Split Single"),				SPLIT_NONE }
};

static GtkRadioActionEntry menu_color_radio_entries[] = {
  { "ColorProfile0",	NULL,			N_("Input _0: sRGB"),			NULL,			N_("Input 0: sRGB"),			COLOR_PROFILE_SRGB },
  { "ColorProfile1",	NULL,			N_("Input _1: AdobeRGB compatible"),	NULL,			N_("Input 1: AdobeRGB compatible"),	COLOR_PROFILE_ADOBERGB },
  { "ColorProfile2",	NULL,			N_("Input _2"),				NULL,			N_("Input 2"),				COLOR_PROFILE_FILE },
  { "ColorProfile3",	NULL,			N_("Input _3"),				NULL,			N_("Input 3"),				COLOR_PROFILE_FILE + 1 },
  { "ColorProfile4",	NULL,			N_("Input _4"),				NULL,			N_("Input 4"),				COLOR_PROFILE_FILE + 2 },
  { "ColorProfile5",	NULL,			N_("Input _5"),				NULL,			N_("Input 5"),				COLOR_PROFILE_FILE + 3 }
};

static GtkRadioActionEntry menu_histogram_channel[] = {
  { "HistogramChanR",	NULL,			N_("Histogram on _Red"),		NULL,			N_("Histogram on Red"),		HCHAN_R },
  { "HistogramChanG",	NULL,			N_("Histogram on _Green"),		NULL,			N_("Histogram on Green"),	HCHAN_G },
  { "HistogramChanB",	NULL,			N_("Histogram on _Blue"),		NULL,			N_("Histogram on Blue"),	HCHAN_B },
  { "HistogramChanRGB",	NULL,			N_("_Histogram on RGB"),			NULL,			N_("Histogram on RGB"),		HCHAN_RGB },
  { "HistogramChanV",	NULL,			N_("Histogram on _Value"),		NULL,			N_("Histogram on Value"),	HCHAN_MAX }
};

static GtkRadioActionEntry menu_histogram_mode[] = {
  { "HistogramModeLin",	NULL,			N_("Li_near Histogram"),		NULL,			N_("Linear Histogram"),		0 },
  { "HistogramModeLog",	NULL,			N_("_Log Histogram"),			NULL,			N_("Log Histogram"),		1 },
};

static GtkRadioActionEntry menu_stereo_mode_entries[] = {
  { "StereoAuto",	NULL,			N_("_Auto"),				NULL,			N_("Stereo Auto"),		STEREO_PIXBUF_DEFAULT },
  { "StereoSBS",	NULL,			N_("_Side by Side"),			NULL,			N_("Stereo Side by Side"),	STEREO_PIXBUF_SBS },
  { "StereoCross",	NULL,			N_("_Cross"),				NULL,			N_("Stereo Cross"),		STEREO_PIXBUF_CROSS },
  { "StereoOff",	NULL,			N_("_Off"),				NULL,			N_("Stereo Off"),		STEREO_PIXBUF_NONE }
};


#undef CB

static const gchar *menu_ui_description =
"<ui>"
"  <menubar name='MainMenu'>"
"    <menu action='FileMenu'>"
"      <menuitem action='NewWindow'/>"
"      <menuitem action='NewCollection'/>"
"      <menuitem action='OpenCollection'/>"
"      <menuitem action='OpenRecent'/>"
"      <placeholder name='OpenSection'/>"
"      <separator/>"
"      <menuitem action='Search'/>"
"      <menuitem action='FindDupes'/>"
"      <placeholder name='SearchSection'/>"
"      <separator/>"
"      <menuitem action='Print'/>"
"      <placeholder name='PrintSection'/>"
"      <separator/>"
"      <menuitem action='NewFolder'/>"
"      <menuitem action='Copy'/>"
"      <menuitem action='Move'/>"
"      <menuitem action='Rename'/>"
"      <menuitem action='Delete'/>"
"      <placeholder name='FileOpsSection'/>"
"      <separator/>"
"      <placeholder name='QuitSection'/>"
"      <menuitem action='CloseWindow'/>"
"      <menuitem action='Quit'/>"
"      <separator/>"
"    </menu>"
"    <menu action='GoMenu'>"
"      <menuitem action='FirstImage'/>"
"      <menuitem action='PrevImage'/>"
"      <menuitem action='NextImage'/>"
"      <menuitem action='LastImage'/>"
"      <separator/>"
"      <menuitem action='Back'/>"
"      <menuitem action='Up'/>"
"      <menuitem action='Home'/>"
"      <separator/>"
"    </menu>"
"    <menu action='SelectMenu'>"
"      <menuitem action='SelectAll'/>"
"      <menuitem action='SelectNone'/>"
"      <menuitem action='SelectInvert'/>"
"      <menuitem action='RectangularSelection'/>"
"      <placeholder name='SelectSection'/>"
"      <separator/>"
"      <menuitem action='CopyPath'/>"
"      <placeholder name='ClipboardSection'/>"
"      <separator/>"
"      <menuitem action='ShowMarks'/>"
"      <placeholder name='MarksSection'/>"
"      <separator/>"
"    </menu>"
"    <menu action='EditMenu'>"
"      <menu action='ExternalMenu'>"
"      </menu>"
"      <placeholder name='EditSection'/>"
"      <separator/>"
"      <menu action='OrientationMenu'>"
"        <menuitem action='RotateCW'/>"
"        <menuitem action='RotateCCW'/>"
"        <menuitem action='Rotate180'/>"
"        <menuitem action='Mirror'/>"
"        <menuitem action='Flip'/>"
"        <menuitem action='AlterNone'/>"
"        <separator/>"
"        <menuitem action='ExifRotate'/>"
"        <separator/>"
"      </menu>"
"      <menuitem action='SaveMetadata'/>"
"      <placeholder name='PropertiesSection'/>"
"      <separator/>"
"        <menu action='PreferencesMenu'>"
"        <menuitem action='Preferences'/>"
"        <menuitem action='Editors'/>"
"        <menuitem action='LayoutConfig'/>"
"        <menuitem action='Maintenance'/>"
"      </menu>"
"      <placeholder name='PreferencesSection'/>"
"      <separator/>"
"      <menuitem action='Wallpaper'/>"
"      <separator/>"
"    </menu>"
"    <menu action='ViewMenu'>"
"      <menuitem action='ViewInNewWindow'/>"
"      <menuitem action='PanView'/>"
"      <menuitem action='ExifWin'/>"
"      <placeholder name='WindowSection'/>"
"      <separator/>"
"      <menu action='FileDirMenu'>"
"        <menuitem action='FolderTree'/>"
"        <placeholder name='FolderSection'/>"
"        <separator/>"
"        <menuitem action='ViewList'/>"
"        <menuitem action='ViewIcons'/>"
"        <menuitem action='Thumbnails'/>"
"        <placeholder name='ListSection'/>"
"        <separator/>"
"        <menuitem action='FloatTools'/>"
"        <menuitem action='HideTools'/>"
"        <menuitem action='HideToolbar'/>"
"      </menu>"
"      <placeholder name='DirSection'/>"
"      <separator/>"
"      <menu action='ZoomMenu'>"
"        <menu action='ConnectZoomMenu'>"
"          <menuitem action='ConnectZoomIn'/>"
"          <menuitem action='ConnectZoomOut'/>"
"          <menuitem action='ConnectZoomFit'/>"
"          <menuitem action='ConnectZoomFillHor'/>"
"          <menuitem action='ConnectZoomFillVert'/>"
"          <menuitem action='ConnectZoom100'/>"
"          <menuitem action='ConnectZoom200'/>"
"          <menuitem action='ConnectZoom300'/>"
"          <menuitem action='ConnectZoom400'/>"
"          <menuitem action='ConnectZoom50'/>"
"          <menuitem action='ConnectZoom33'/>"
"          <menuitem action='ConnectZoom25'/>"
"        </menu>"
"        <menuitem action='ZoomIn'/>"
"        <menuitem action='ZoomOut'/>"
"        <menuitem action='ZoomFit'/>"
"        <menuitem action='ZoomFillHor'/>"
"        <menuitem action='ZoomFillVert'/>"
"        <menuitem action='Zoom100'/>"
"        <menuitem action='Zoom200'/>"
"        <menuitem action='Zoom300'/>"
"        <menuitem action='Zoom400'/>"
"        <menuitem action='Zoom50'/>"
"        <menuitem action='Zoom33'/>"
"        <menuitem action='Zoom25'/>"
"      </menu>"
"      <menu action='SplitMenu'>"
"        <menuitem action='SplitHorizontal'/>"
"        <menuitem action='SplitVertical'/>"
"        <menuitem action='SplitQuad'/>"
"        <menuitem action='SplitSingle'/>"
"      </menu>"
"      <menu action='StereoMenu'>"
"        <menuitem action='StereoAuto'/>"
"        <menuitem action='StereoSBS'/>"
"        <menuitem action='StereoCross'/>"
"        <menuitem action='StereoOff'/>"
"        <separator/>"
"        <menuitem action='StereoCycle'/>"
"      </menu>"
"      <menu action='ColorMenu'>"
"        <menuitem action='UseColorProfiles'/>"
"        <menuitem action='UseImageProfile'/>"
"        <menuitem action='ColorProfile0'/>"
"        <menuitem action='ColorProfile1'/>"
"        <menuitem action='ColorProfile2'/>"
"        <menuitem action='ColorProfile3'/>"
"        <menuitem action='ColorProfile4'/>"
"        <menuitem action='ColorProfile5'/>"
"        <separator/>"
"        <menuitem action='Grayscale'/>"
"      </menu>"
"      <menu action='OverlayMenu'>"
"        <menuitem action='ImageOverlay'/>"
"        <menuitem action='ImageHistogram'/>"
"        <menuitem action='ImageOverlayCycle'/>"
"        <separator/>"
"        <menuitem action='HistogramChanR'/>"
"        <menuitem action='HistogramChanG'/>"
"        <menuitem action='HistogramChanB'/>"
"        <menuitem action='HistogramChanRGB'/>"
"        <menuitem action='HistogramChanV'/>"
"        <menuitem action='HistogramChanCycle'/>"
"        <separator/>"
"        <menuitem action='HistogramModeLin'/>"
"        <menuitem action='HistogramModeLog'/>"
"        <menuitem action='HistogramModeCycle'/>"
"      </menu>"
"      <menuitem action='FullScreen'/>"
"      <placeholder name='ViewSection'/>"
"      <separator/>"
"      <menuitem action='SBar'/>"
"      <menuitem action='SBarSort'/>"
"      <menuitem action='ShowInfoPixel'/>"
"      <placeholder name='ToolsSection'/>"
"      <separator/>"
"      <menuitem action='Animate'/>"
"      <menuitem action='SlideShow'/>"
"      <menuitem action='SlideShowPause'/>"
"      <menuitem action='Refresh'/>"
"      <placeholder name='SlideShowSection'/>"
"      <separator/>"
"    </menu>"
"    <menu action='HelpMenu'>"
"      <separator/>"
"      <menuitem action='HelpContents'/>"
"      <menuitem action='HelpShortcuts'/>"
"      <menuitem action='HelpKbd'/>"
"      <menuitem action='HelpNotes'/>"
"      <placeholder name='HelpSection'/>"
"      <separator/>"
"      <menuitem action='About'/>"
"      <separator/>"
"      <menuitem action='LogWindow'/>"
"      <separator/>"
"    </menu>"
"  </menubar>"
"  <toolbar name='ToolBar'>"
"    <toolitem action='Thumbnails'/>"
"    <toolitem action='Back'/>"
"    <toolitem action='Up'/>"
"    <toolitem action='Home'/>"
"    <toolitem action='Refresh'/>"
"    <toolitem action='ZoomIn'/>"
"    <toolitem action='ZoomOut'/>"
"    <toolitem action='ZoomFit'/>"
"    <toolitem action='Zoom100'/>"
"    <toolitem action='Preferences'/>"
"    <toolitem action='FloatTools'/>"
"  </toolbar>"
"  <toolbar name='StatusBar'>"
"    <toolitem action='ExifRotate'/>"
"    <toolitem action='ShowInfoPixel'/>"
"    <toolitem action='UseColorProfiles'/>"
"    <toolitem action='SaveMetadata'/>"
"  </toolbar>"
"<accelerator action='PrevImageAlt1'/>"
"<accelerator action='PrevImageAlt2'/>"
"<accelerator action='NextImageAlt1'/>"
"<accelerator action='NextImageAlt2'/>"
"<accelerator action='DeleteAlt1'/>"
"<accelerator action='DeleteAlt2'/>"
"<accelerator action='FullScreenAlt1'/>"
"<accelerator action='FullScreenAlt2'/>"
"<accelerator action='Escape'/>"
"<accelerator action='EscapeAlt1'/>"

"<accelerator action='ZoomInAlt1'/>"
"<accelerator action='ZoomOutAlt1'/>"
"<accelerator action='Zoom100Alt1'/>"
"<accelerator action='ZoomFitAlt1'/>"

"<accelerator action='ConnectZoomInAlt1'/>"
"<accelerator action='ConnectZoomOutAlt1'/>"
"<accelerator action='ConnectZoom100Alt1'/>"
"<accelerator action='ConnectZoomFitAlt1'/>"
"</ui>";

static gchar *menu_translate(const gchar *path, gpointer data)
{
	return (gchar *)(_(path));
}

static void layout_actions_setup_mark(LayoutWindow *lw, gint mark, gchar *name_tmpl,
				      gchar *label_tmpl, gchar *accel_tmpl, gchar *tooltip_tmpl, GCallback cb)
{
	gchar name[50];
	gchar label[100];
	gchar accel[50];
	gchar tooltip[100];
	GtkActionEntry entry = { name, NULL, label, accel, tooltip, cb };
	GtkAction *action;

	g_snprintf(name, sizeof(name), name_tmpl, mark);
	g_snprintf(label, sizeof(label), label_tmpl, mark);

	if (accel_tmpl)
		g_snprintf(accel, sizeof(accel), accel_tmpl, mark % 10);
	else
		entry.accelerator = NULL;

	if (tooltip_tmpl)
		g_snprintf(tooltip, sizeof(tooltip), tooltip_tmpl, mark);
	else
		entry.tooltip = NULL;

	gtk_action_group_add_actions(lw->action_group, &entry, 1, lw);
	action = gtk_action_group_get_action(lw->action_group, name);
	g_object_set_data(G_OBJECT(action), "mark_num", GINT_TO_POINTER(mark));
}

static void layout_actions_setup_marks(LayoutWindow *lw)
{
	gint mark;
	GError *error;
	GString *desc = g_string_new(
				"<ui>"
				"  <menubar name='MainMenu'>"
				"    <menu action='SelectMenu'>");

	for (mark = 1; mark <= FILEDATA_MARKS_SIZE; mark++)
		{
		layout_actions_setup_mark(lw, mark, "Mark%d",		_("Mark _%d"), NULL, NULL, NULL);
		layout_actions_setup_mark(lw, mark, "SetMark%d",	_("_Set mark %d"),			NULL,		_("Set mark %d"), G_CALLBACK(layout_menu_set_mark_sel_cb));
		layout_actions_setup_mark(lw, mark, "ResetMark%d",	_("_Reset mark %d"),			NULL,		_("Reset mark %d"), G_CALLBACK(layout_menu_res_mark_sel_cb));
		layout_actions_setup_mark(lw, mark, "ToggleMark%d",	_("_Toggle mark %d"),			"%d",		_("Toggle mark %d"), G_CALLBACK(layout_menu_toggle_mark_sel_cb));
		layout_actions_setup_mark(lw, mark, "ToggleMark%dAlt1",	_("_Toggle mark %d"),			"KP_%d",	_("Toggle mark %d"), G_CALLBACK(layout_menu_toggle_mark_sel_cb));
		layout_actions_setup_mark(lw, mark, "SelectMark%d",	_("Se_lect mark %d"),			"<control>%d",	_("Select mark %d"), G_CALLBACK(layout_menu_sel_mark_cb));
		layout_actions_setup_mark(lw, mark, "SelectMark%dAlt1",	_("_Select mark %d"),			"<control>KP_%d", _("Select mark %d"), G_CALLBACK(layout_menu_sel_mark_cb));
		layout_actions_setup_mark(lw, mark, "AddMark%d",	_("_Add mark %d"),			NULL,		_("Add mark %d"), G_CALLBACK(layout_menu_sel_mark_or_cb));
		layout_actions_setup_mark(lw, mark, "IntMark%d",	_("_Intersection with mark %d"),	NULL,		_("Intersection with mark %d"), G_CALLBACK(layout_menu_sel_mark_and_cb));
		layout_actions_setup_mark(lw, mark, "UnselMark%d",	_("_Unselect mark %d"),			NULL,		_("Unselect mark %d"), G_CALLBACK(layout_menu_sel_mark_minus_cb));
		layout_actions_setup_mark(lw, mark, "FilterMark%d",	_("_Filter mark %d"),			NULL,		_("Filter mark %d"), G_CALLBACK(layout_menu_mark_filter_toggle_cb));

		g_string_append_printf(desc,
				"      <menu action='Mark%d'>"
				"        <menuitem action='ToggleMark%d'/>"
				"        <menuitem action='SetMark%d'/>"
				"        <menuitem action='ResetMark%d'/>"
				"        <separator/>"
				"        <menuitem action='SelectMark%d'/>"
				"        <menuitem action='AddMark%d'/>"
				"        <menuitem action='IntMark%d'/>"
				"        <menuitem action='UnselMark%d'/>"
				"        <separator/>"
				"        <menuitem action='FilterMark%d'/>"
				"      </menu>",
				mark, mark, mark, mark, mark, mark, mark, mark, mark);
		}

	g_string_append(desc,
				"    </menu>"
				"  </menubar>");
	for (mark = 1; mark <= FILEDATA_MARKS_SIZE; mark++)
		{
		g_string_append_printf(desc,
				"<accelerator action='ToggleMark%dAlt1'/>"
				"<accelerator action='SelectMark%dAlt1'/>",
				mark, mark);
		}
	g_string_append(desc,   "</ui>" );

	error = NULL;
	if (!gtk_ui_manager_add_ui_from_string(lw->ui_manager, desc->str, -1, &error))
		{
		g_message("building menus failed: %s", error->message);
		g_error_free(error);
		exit(EXIT_FAILURE);
		}
	g_string_free(desc, TRUE);
}

static GList *layout_actions_editor_menu_path(EditorDescription *editor)
{
	gchar **split = g_strsplit(editor->menu_path, "/", 0);
	gint i = 0;
	GList *ret = NULL;

	if (split[0] == NULL)
		{
		g_strfreev(split);
		return NULL;
		}

	while (split[i])
		{
		ret = g_list_prepend(ret, g_strdup(split[i]));
		i++;
		}

	g_strfreev(split);

	ret = g_list_prepend(ret, g_strdup(editor->key));

	return g_list_reverse(ret);
}

static void layout_actions_editor_add(GString *desc, GList *path, GList *old_path)
{
	gint to_open, to_close, i;
	while (path && old_path && strcmp((gchar *)path->data, (gchar *)old_path->data) == 0)
		{
		path = path->next;
		old_path = old_path->next;
		}
	to_open = g_list_length(path) - 1;
	to_close = g_list_length(old_path) - 1;

	if (to_close > 0)
		{
		old_path = g_list_last(old_path);
		old_path = old_path->prev;
		}

	for (i =  0; i < to_close; i++)
		{
		gchar *name = old_path->data;
		if (g_str_has_suffix(name, "Section"))
			{
			g_string_append(desc,	"      </placeholder>");
			}
		else if (g_str_has_suffix(name, "Menu"))
			{
			g_string_append(desc,	"    </menu>");
			}
		else
			{
			g_warning("invalid menu path item %s", name);
			}
		old_path = old_path->prev;
		}

	for (i =  0; i < to_open; i++)
		{
		gchar *name = path->data;
		if (g_str_has_suffix(name, "Section"))
			{
			g_string_append_printf(desc,	"      <placeholder name='%s'>", name);
			}
		else if (g_str_has_suffix(name, "Menu"))
			{
			g_string_append_printf(desc,	"    <menu action='%s'>", name);
			}
		else
			{
			g_warning("invalid menu path item %s", name);
			}
		path = path->next;
		}

	if (path)
		g_string_append_printf(desc, "      <menuitem action='%s'/>", (gchar *)path->data);
}

static void layout_actions_setup_editors(LayoutWindow *lw)
{
	GError *error;
	GList *editors_list;
	GList *work;
	GList *old_path;
	GString *desc;

	if (lw->ui_editors_id)
		{
		gtk_ui_manager_remove_ui(lw->ui_manager, lw->ui_editors_id);
		}

	if (lw->action_group_editors)
		{
		gtk_ui_manager_remove_action_group(lw->ui_manager, lw->action_group_editors);
		g_object_unref(lw->action_group_editors);
		}
	lw->action_group_editors = gtk_action_group_new("MenuActionsExternal");
	gtk_ui_manager_insert_action_group(lw->ui_manager, lw->action_group_editors, 1);

	/* lw->action_group_editors contains translated entries, no translate func is required */
	desc = g_string_new(
				"<ui>"
				"  <menubar name='MainMenu'>");

	editors_list = editor_list_get();

	old_path = NULL;
	work = editors_list;
	while (work)
		{
		GList *path;
		EditorDescription *editor = work->data;
		GtkActionEntry entry = { editor->key,
		                         NULL,
		                         editor->name,
		                         editor->hotkey,
		                         editor->comment ? editor->comment : editor->name,
		                         G_CALLBACK(layout_menu_edit_cb) };

		if (editor->icon)
			{
			entry.stock_id = editor->key;
			}
		gtk_action_group_add_actions(lw->action_group_editors, &entry, 1, lw);

		path = layout_actions_editor_menu_path(editor);
		layout_actions_editor_add(desc, path, old_path);

		string_list_free(old_path);
		old_path = path;
		work = work->next;
		}

	layout_actions_editor_add(desc, NULL, old_path);
	string_list_free(old_path);

	g_string_append(desc,   "  </menubar>"
				"</ui>" );

	error = NULL;

	lw->ui_editors_id = gtk_ui_manager_add_ui_from_string(lw->ui_manager, desc->str, -1, &error);
	if (!lw->ui_editors_id)
		{
		g_message("building menus failed: %s", error->message);
		g_error_free(error);
		exit(EXIT_FAILURE);
		}
	g_string_free(desc, TRUE);
	g_list_free(editors_list);
}

void layout_actions_setup(LayoutWindow *lw)
{
	GError *error;
	gint i;

	DEBUG_1("%s layout_actions_setup: start", get_exec_time());
	if (lw->ui_manager) return;

	lw->action_group = gtk_action_group_new("MenuActions");
	gtk_action_group_set_translate_func(lw->action_group, menu_translate, NULL, NULL);

	gtk_action_group_add_actions(lw->action_group,
				     menu_entries, G_N_ELEMENTS(menu_entries), lw);
	gtk_action_group_add_toggle_actions(lw->action_group,
					    menu_toggle_entries, G_N_ELEMENTS(menu_toggle_entries), lw);
	gtk_action_group_add_radio_actions(lw->action_group,
					   menu_radio_entries, G_N_ELEMENTS(menu_radio_entries),
					   0, G_CALLBACK(layout_menu_list_cb), lw);
	gtk_action_group_add_radio_actions(lw->action_group,
					   menu_split_radio_entries, G_N_ELEMENTS(menu_split_radio_entries),
					   0, G_CALLBACK(layout_menu_split_cb), lw);
	gtk_action_group_add_toggle_actions(lw->action_group,
					   menu_view_dir_toggle_entries, G_N_ELEMENTS(menu_view_dir_toggle_entries),
					    lw);
	gtk_action_group_add_radio_actions(lw->action_group,
					   menu_color_radio_entries, COLOR_PROFILE_FILE + COLOR_PROFILE_INPUTS,
					   0, G_CALLBACK(layout_color_menu_input_cb), lw);
	gtk_action_group_add_radio_actions(lw->action_group,
					   menu_histogram_channel, G_N_ELEMENTS(menu_histogram_channel),
					   0, G_CALLBACK(layout_menu_histogram_channel_cb), lw);
	gtk_action_group_add_radio_actions(lw->action_group,
					   menu_histogram_mode, G_N_ELEMENTS(menu_histogram_mode),
					   0, G_CALLBACK(layout_menu_histogram_mode_cb), lw);
	gtk_action_group_add_radio_actions(lw->action_group,
					   menu_stereo_mode_entries, G_N_ELEMENTS(menu_stereo_mode_entries),
					   0, G_CALLBACK(layout_menu_stereo_mode_cb), lw);


	lw->ui_manager = gtk_ui_manager_new();
	gtk_ui_manager_set_add_tearoffs(lw->ui_manager, TRUE);
	gtk_ui_manager_insert_action_group(lw->ui_manager, lw->action_group, 0);

	DEBUG_1("%s layout_actions_setup: add menu", get_exec_time());
	error = NULL;
	if (!gtk_ui_manager_add_ui_from_string(lw->ui_manager, menu_ui_description, -1, &error))
		{
		g_message("building menus failed: %s", error->message);
		g_error_free(error);
		exit(EXIT_FAILURE);
		}

	DEBUG_1("%s layout_actions_setup: marks", get_exec_time());
	layout_actions_setup_marks(lw);

	DEBUG_1("%s layout_actions_setup: editors", get_exec_time());
	layout_actions_setup_editors(lw);

	DEBUG_1("%s layout_actions_setup: status_update_write", get_exec_time());
	layout_util_status_update_write(lw);

	DEBUG_1("%s layout_actions_setup: actions_add_window", get_exec_time());
	layout_actions_add_window(lw, lw->window);
	DEBUG_1("%s layout_actions_setup: end", get_exec_time());
}

static gint layout_editors_reload_idle_id = -1;
static GList *layout_editors_desktop_files = NULL;

static gboolean layout_editors_reload_idle_cb(gpointer data)
{
	if (!layout_editors_desktop_files)
		{
		DEBUG_1("%s layout_editors_reload_idle_cb: get_desktop_files", get_exec_time());
		layout_editors_desktop_files = editor_get_desktop_files();
		return TRUE;
		}

	editor_read_desktop_file(layout_editors_desktop_files->data);
	g_free(layout_editors_desktop_files->data);
	layout_editors_desktop_files = g_list_delete_link(layout_editors_desktop_files, layout_editors_desktop_files);


	if (!layout_editors_desktop_files)
		{
		GList *work;
		DEBUG_1("%s layout_editors_reload_idle_cb: setup_editors", get_exec_time());
		editor_table_finish();

		work = layout_window_list;
		while (work)
			{
			LayoutWindow *lw = work->data;
			work = work->next;
			layout_actions_setup_editors(lw);
			}

		DEBUG_1("%s layout_editors_reload_idle_cb: setup_editors done", get_exec_time());

		layout_editors_reload_idle_id = -1;
		return FALSE;
		}
	return TRUE;
}

void layout_editors_reload_start(void)
{
	DEBUG_1("%s layout_editors_reload_start", get_exec_time());

	if (layout_editors_reload_idle_id != -1)
		{
		g_source_remove(layout_editors_reload_idle_id);
		string_list_free(layout_editors_desktop_files);
		}

	editor_table_clear();
	layout_editors_reload_idle_id = g_idle_add(layout_editors_reload_idle_cb, NULL);
}

void layout_editors_reload_finish(void)
{
	if (layout_editors_reload_idle_id != -1)
		{
		DEBUG_1("%s layout_editors_reload_finish", get_exec_time());
		g_source_remove(layout_editors_reload_idle_id);
		while (layout_editors_reload_idle_id != -1)
			{
			layout_editors_reload_idle_cb(NULL);
			}
		}
}

void layout_actions_add_window(LayoutWindow *lw, GtkWidget *window)
{
	GtkAccelGroup *group;

	if (!lw->ui_manager) return;

	group = gtk_ui_manager_get_accel_group(lw->ui_manager);
	gtk_window_add_accel_group(GTK_WINDOW(window), group);
}

GtkWidget *layout_actions_menu_bar(LayoutWindow *lw)
{
	if (lw->menu_bar) return lw->menu_bar;
	lw->menu_bar = gtk_ui_manager_get_widget(lw->ui_manager, "/MainMenu");
	g_object_ref(lw->menu_bar);
	return lw->menu_bar;
}

GtkWidget *layout_actions_toolbar(LayoutWindow *lw, ToolbarType type)
{
	if (lw->toolbar[type]) return lw->toolbar[type];
	switch (type)
		{
		case TOOLBAR_MAIN:
			lw->toolbar[type] = gtk_ui_manager_get_widget(lw->ui_manager, "/ToolBar");
			gtk_toolbar_set_icon_size(GTK_TOOLBAR(lw->toolbar[type]), GTK_ICON_SIZE_SMALL_TOOLBAR);
			gtk_toolbar_set_style(GTK_TOOLBAR(lw->toolbar[type]), GTK_TOOLBAR_ICONS);
			break;
		case TOOLBAR_STATUS:
			lw->toolbar[type] = gtk_ui_manager_get_widget(lw->ui_manager, "/StatusBar");
			gtk_toolbar_set_icon_size(GTK_TOOLBAR(lw->toolbar[type]), GTK_ICON_SIZE_MENU);
			gtk_toolbar_set_style(GTK_TOOLBAR(lw->toolbar[type]), GTK_TOOLBAR_ICONS);
			gtk_toolbar_set_show_arrow(GTK_TOOLBAR(lw->toolbar[type]), FALSE);
			break;
		default:
			break;
		}
	g_object_ref(lw->toolbar[type]);
	return lw->toolbar[type];
}

/*
 *-----------------------------------------------------------------------------
 * misc
 *-----------------------------------------------------------------------------
 */

void layout_util_status_update_write(LayoutWindow *lw)
{
	GtkAction *action;
	gint n = metadata_queue_length();
	action = gtk_action_group_get_action(lw->action_group, "SaveMetadata");
	gtk_action_set_sensitive(action, n > 0);
	if (n > 0)
		{
		gchar *buf = g_strdup_printf(_("Number of files with unsaved metadata: %d"), n);
		g_object_set(G_OBJECT(action), "tooltip", buf, NULL);
		g_free(buf);
		}
	else
		{
		g_object_set(G_OBJECT(action), "tooltip", _("No unsaved metadata"), NULL);
		}
}

void layout_util_status_update_write_all(void)
{
	GList *work;

	work = layout_window_list;
	while (work)
		{
		LayoutWindow *lw = work->data;
		work = work->next;

		layout_util_status_update_write(lw);
		}
}

static gchar *layout_color_name_parse(const gchar *name)
{
	if (!name || !*name) return g_strdup(_("Empty"));
	return g_strdelimit(g_strdup(name), "_", '-');
}

void layout_util_sync_color(LayoutWindow *lw)
{
	GtkAction *action;
	gint input = 0;
	gboolean use_color;
	gboolean use_image = FALSE;
	gint i;
	gchar action_name[15];
	gchar *image_profile;
	gchar *screen_profile;


	if (!lw->action_group) return;
	if (!layout_image_color_profile_get(lw, &input, &use_image)) return;

	use_color = layout_image_color_profile_get_use(lw);

	action = gtk_action_group_get_action(lw->action_group, "UseColorProfiles");
#ifdef HAVE_LCMS
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), use_color);
	if (layout_image_color_profile_get_status(lw, &image_profile, &screen_profile))
		{
		gchar *buf;
		buf = g_strdup_printf(_("Image profile: %s\nScreen profile: %s"), image_profile, screen_profile);
		g_object_set(G_OBJECT(action), "tooltip", buf, NULL);
		g_free(image_profile);
		g_free(screen_profile);
		g_free(buf);
		}
	else
		{
		g_object_set(G_OBJECT(action), "tooltip", _("Click to enable color management"), NULL);
		}
#else
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), FALSE);
	gtk_action_set_sensitive(action, FALSE);
	g_object_set(G_OBJECT(action), "tooltip", _("Color profiles not supported"), NULL);
#endif

	action = gtk_action_group_get_action(lw->action_group, "UseImageProfile");
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), use_image);
	gtk_action_set_sensitive(action, use_color);

	for (i = 0; i < COLOR_PROFILE_FILE + COLOR_PROFILE_INPUTS; i++)
		{
		sprintf(action_name, "ColorProfile%d", i);
		action = gtk_action_group_get_action(lw->action_group, action_name);

		if (i >= COLOR_PROFILE_FILE)
			{
			const gchar *name = options->color_profile.input_name[i - COLOR_PROFILE_FILE];
			const gchar *file = options->color_profile.input_file[i - COLOR_PROFILE_FILE];
			gchar *end;
			gchar *buf;

			if (!name || !name[0]) name = filename_from_path(file);

			end = layout_color_name_parse(name);
			buf = g_strdup_printf(_("Input _%d: %s"), i, end);
			g_free(end);

			g_object_set(G_OBJECT(action), "label", buf, NULL);
			g_free(buf);

			gtk_action_set_visible(action, file && file[0]);
			}

		gtk_action_set_sensitive(action, !use_image);
		gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), (i == input));
		}

	action = gtk_action_group_get_action(lw->action_group, "Grayscale");
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), layout_image_get_desaturate(lw));
}

static void layout_util_sync_views(LayoutWindow *lw)
{
	GtkAction *action;
	OsdShowFlags osd_flags = image_osd_get(lw->image);

	if (!lw->action_group) return;

	action = gtk_action_group_get_action(lw->action_group, "FolderTree");
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), lw->options.dir_view_type);

	action = gtk_action_group_get_action(lw->action_group, "SplitSingle");
	gtk_radio_action_set_current_value(GTK_RADIO_ACTION(action), lw->split_mode);

	action = gtk_action_group_get_action(lw->action_group, "ViewIcons");
	gtk_radio_action_set_current_value(GTK_RADIO_ACTION(action), lw->options.file_view_type);

	action = gtk_action_group_get_action(lw->action_group, "FloatTools");
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), lw->options.tools_float);

	action = gtk_action_group_get_action(lw->action_group, "SBar");
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), layout_bar_enabled(lw));

	action = gtk_action_group_get_action(lw->action_group, "SBarSort");
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), layout_bar_sort_enabled(lw));

	action = gtk_action_group_get_action(lw->action_group, "HideToolbar");
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), lw->options.toolbar_hidden);

	action = gtk_action_group_get_action(lw->action_group, "ShowInfoPixel");
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), lw->options.show_info_pixel);

	action = gtk_action_group_get_action(lw->action_group, "ShowMarks");
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), lw->options.show_marks);

	action = gtk_action_group_get_action(lw->action_group, "SlideShow");
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), layout_image_slideshow_active(lw));

	action = gtk_action_group_get_action(lw->action_group, "Animate");
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), lw->options.animate);

	action = gtk_action_group_get_action(lw->action_group, "ImageOverlay");
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), osd_flags != OSD_SHOW_NOTHING);

	action = gtk_action_group_get_action(lw->action_group, "ImageHistogram");
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), osd_flags & OSD_SHOW_HISTOGRAM);

	action = gtk_action_group_get_action(lw->action_group, "ExifRotate");
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), options->image.exif_rotate_enable);

	action = gtk_action_group_get_action(lw->action_group, "RectangularSelection");
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), options->collections.rectangular_selection);

	if (osd_flags & OSD_SHOW_HISTOGRAM)
		{
		action = gtk_action_group_get_action(lw->action_group, "HistogramChanR");
		gtk_radio_action_set_current_value(GTK_RADIO_ACTION(action), image_osd_histogram_get_channel(lw->image));

		action = gtk_action_group_get_action(lw->action_group, "HistogramModeLin");
		gtk_radio_action_set_current_value(GTK_RADIO_ACTION(action), image_osd_histogram_get_mode(lw->image));
		}

	action = gtk_action_group_get_action(lw->action_group, "ConnectZoomMenu");
	gtk_action_set_sensitive(action, lw->split_mode != SPLIT_NONE);

	action = gtk_action_group_get_action(lw->action_group, "StereoAuto");
	gtk_radio_action_set_current_value(GTK_RADIO_ACTION(action), layout_image_stereo_pixbuf_get(lw));

	layout_util_sync_color(lw);
}

void layout_util_sync_thumb(LayoutWindow *lw)
{
	GtkAction *action;

	if (!lw->action_group) return;

	action = gtk_action_group_get_action(lw->action_group, "Thumbnails");
	gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), lw->options.show_thumbnails);
	g_object_set(action, "sensitive", (lw->options.file_view_type == FILEVIEW_LIST), NULL);
}

void layout_util_sync(LayoutWindow *lw)
{
	layout_util_sync_views(lw);
	layout_util_sync_thumb(lw);
	layout_menu_recent_update(lw);
//	layout_menu_edit_update(lw);
}


/*
 *-----------------------------------------------------------------------------
 * sidebars
 *-----------------------------------------------------------------------------
 */

static gboolean layout_bar_enabled(LayoutWindow *lw)
{
	return lw->bar && gtk_widget_get_visible(lw->bar);
}

static void layout_bar_destroyed(GtkWidget *widget, gpointer data)
{
	LayoutWindow *lw = data;

	lw->bar = NULL;
/*
    do not call layout_util_sync_views(lw) here
    this is called either when whole layout is destroyed - no need for update
    or when the bar is replaced - sync is called by upper function at the end of whole operation

*/
}

static void layout_bar_set_default(LayoutWindow *lw)
{
	GtkWidget *bar;

	if (!lw->utility_box) return;

	bar = bar_new(lw);

	layout_bar_set(lw, bar);

	bar_populate_default(bar);
}

static void layout_bar_close(LayoutWindow *lw)
{
	if (lw->bar)
		{
		bar_close(lw->bar);
		lw->bar = NULL;
		}
}


void layout_bar_set(LayoutWindow *lw, GtkWidget *bar)
{
	if (!lw->utility_box) return;

	layout_bar_close(lw); /* if any */

	if (!bar) return;
	lw->bar = bar;

	g_signal_connect(G_OBJECT(lw->bar), "destroy",
			 G_CALLBACK(layout_bar_destroyed), lw);


//	gtk_box_pack_start(GTK_BOX(lw->utility_box), lw->bar, FALSE, FALSE, 0);
	gtk_paned_pack2(GTK_PANED(lw->utility_paned), lw->bar, FALSE, TRUE);

	bar_set_fd(lw->bar, layout_image_get_fd(lw));
}


void layout_bar_toggle(LayoutWindow *lw)
{
	if (layout_bar_enabled(lw))
		{
		gtk_widget_hide(lw->bar);
		}
	else
		{
		if (!lw->bar)
			{
			layout_bar_set_default(lw);
			}
		gtk_widget_show(lw->bar);
		bar_set_fd(lw->bar, layout_image_get_fd(lw));
		}
	layout_util_sync_views(lw);
}

static void layout_bar_new_image(LayoutWindow *lw)
{
	if (!layout_bar_enabled(lw)) return;

	bar_set_fd(lw->bar, layout_image_get_fd(lw));
}

static void layout_bar_new_selection(LayoutWindow *lw, gint count)
{
	if (!layout_bar_enabled(lw)) return;

	bar_notify_selection(lw->bar, count);
}

static gboolean layout_bar_sort_enabled(LayoutWindow *lw)
{
	return lw->bar_sort && gtk_widget_get_visible(lw->bar_sort);
}


static void layout_bar_sort_destroyed(GtkWidget *widget, gpointer data)
{
	LayoutWindow *lw = data;

	lw->bar_sort = NULL;

/*
    do not call layout_util_sync_views(lw) here
    this is called either when whole layout is destroyed - no need for update
    or when the bar is replaced - sync is called by upper function at the end of whole operation

*/
}

static void layout_bar_sort_set_default(LayoutWindow *lw)
{
	GtkWidget *bar;

	if (!lw->utility_box) return;

	bar = bar_sort_new_default(lw);

	layout_bar_sort_set(lw, bar);
}

static void layout_bar_sort_close(LayoutWindow *lw)
{
	if (lw->bar_sort)
		{
		bar_sort_close(lw->bar_sort);
		lw->bar_sort = NULL;
		}
}

void layout_bar_sort_set(LayoutWindow *lw, GtkWidget *bar)
{
	if (!lw->utility_box) return;

	layout_bar_sort_close(lw); /* if any */

	if (!bar) return;
	lw->bar_sort = bar;

	g_signal_connect(G_OBJECT(lw->bar_sort), "destroy",
			 G_CALLBACK(layout_bar_sort_destroyed), lw);

	gtk_box_pack_end(GTK_BOX(lw->utility_box), lw->bar_sort, FALSE, FALSE, 0);
}

void layout_bar_sort_toggle(LayoutWindow *lw)
{
	if (layout_bar_sort_enabled(lw))
		{
		gtk_widget_hide(lw->bar_sort);
		}
	else
		{
		if (!lw->bar_sort)
			{
			layout_bar_sort_set_default(lw);
			}
		gtk_widget_show(lw->bar_sort);
		}
	layout_util_sync_views(lw);
}

void layout_bars_new_image(LayoutWindow *lw)
{
	layout_bar_new_image(lw);

	if (lw->exif_window) advanced_exif_set_fd(lw->exif_window, layout_image_get_fd(lw));

	/* this should be called here to handle the metadata edited in bars */
	if (options->metadata.confirm_on_image_change)
		metadata_write_queue_confirm(FALSE, NULL, NULL);
}

void layout_bars_new_selection(LayoutWindow *lw, gint count)
{
	layout_bar_new_selection(lw, count);
}

GtkWidget *layout_bars_prepare(LayoutWindow *lw, GtkWidget *image)
{
	if (lw->utility_box) return lw->utility_box;
	lw->utility_box = gtk_hbox_new(FALSE, PREF_PAD_GAP);
	lw->utility_paned = gtk_hpaned_new();
	gtk_box_pack_start(GTK_BOX(lw->utility_box), lw->utility_paned, TRUE, TRUE, 0);

	gtk_paned_pack1(GTK_PANED(lw->utility_paned), image, TRUE, FALSE);
	gtk_widget_show(lw->utility_paned);

	gtk_widget_show(image);

	g_object_ref(lw->utility_box);
	return lw->utility_box;
}

void layout_bars_close(LayoutWindow *lw)
{
	layout_bar_sort_close(lw);
	layout_bar_close(lw);
}

static void layout_exif_window_destroy(GtkWidget *widget, gpointer data)
{
	LayoutWindow *lw = data;
	lw->exif_window = NULL;
}

void layout_exif_window_new(LayoutWindow *lw)
{
	if (lw->exif_window) return;

	lw->exif_window = advanced_exif_new();
	if (!lw->exif_window) return;
	g_signal_connect(G_OBJECT(lw->exif_window), "destroy",
			 G_CALLBACK(layout_exif_window_destroy), lw);
	advanced_exif_set_fd(lw->exif_window, layout_image_get_fd(lw));
}

/* vim: set shiftwidth=8 softtabstop=0 cindent cinoptions={1s: */
