/*
 * This file Copyright (C) 2008-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#pragma once

#include <sys/types.h>
#include <glib.h>
#include <gtk/gtk.h>

#include <libtransmission/transmission.h>
#include <libtransmission/tr-macros.h>

extern int const mem_K;
extern char const* mem_K_str;
extern char const* mem_M_str;
extern char const* mem_G_str;
extern char const* mem_T_str;

extern int const disk_K;
extern char const* disk_K_str;
extern char const* disk_M_str;
extern char const* disk_G_str;
extern char const* disk_T_str;

extern int const speed_K;
extern char const* speed_K_str;
extern char const* speed_M_str;
extern char const* speed_G_str;
extern char const* speed_T_str;

#if GLIB_CHECK_VERSION(2, 33, 12)

#define TR_DEFINE_QUARK G_DEFINE_QUARK

#else

#define TR_DEFINE_QUARK(QN, q_n) \
    GQuark q_n##_quark(void) \
    { \
        static GQuark q; \
\
        if (G_UNLIKELY(q == 0)) \
        { \
            q = g_quark_from_static_string(#QN); \
        } \
\
        return q; \
    }

#endif

// http://cnicholson.net/2009/02/stupid-c-tricks-adventures-in-assert/
#define TR_UNUSED(x) \
    do \
    { \
        ((void)sizeof(x)); \
    } while (0)

enum
{
    GTR_UNICODE_UP,
    GTR_UNICODE_DOWN,
    GTR_UNICODE_INF,
    GTR_UNICODE_BULLET
};

char const* gtr_get_unicode_string(int);

/* return a percent formatted string of either x.xx, xx.x or xxx */
char* tr_strlpercent(char* buf, double x, size_t buflen);

/* return a human-readable string for the size given in bytes. */
char* tr_strlsize(char* buf, guint64 size, size_t buflen);

/* return a human-readable string for the given ratio. */
char* tr_strlratio(char* buf, double ratio, size_t buflen);

/* return a human-readable string for the time given in seconds. */
char* tr_strltime(char* buf, time_t secs, size_t buflen);

/***
****
***/

/* http://www.legaltorrents.com/some/announce/url --> legaltorrents.com */
void gtr_get_host_from_url(char* buf, size_t buflen, char const* url);

gboolean gtr_is_magnet_link(char const* str);

gboolean gtr_is_hex_hashcode(char const* str);

/***
****
***/

void gtr_open_uri(char const* uri);

void gtr_open_file(char const* path);

char const* gtr_get_help_uri(void);

/***
****
***/

/* backwards-compatible wrapper around gtk_widget_set_visible() */
void gtr_widget_set_visible(GtkWidget*, gboolean);

void gtr_dialog_set_content(GtkDialog* dialog, GtkWidget* content);

/***
****
***/

GtkWidget* gtr_priority_combo_new(void);
#define gtr_priority_combo_get_value(w) gtr_combo_box_get_active_enum(w)
#define gtr_priority_combo_set_value(w, val) gtr_combo_box_set_active_enum(w, val)

GtkWidget* gtr_combo_box_new_enum(char const* text_1, ...);
int gtr_combo_box_get_active_enum(GtkComboBox*);
void gtr_combo_box_set_active_enum(GtkComboBox*, int value);

/***
****
***/

struct _TrCore;

GtkWidget* gtr_freespace_label_new(struct _TrCore* core, char const* dir);

void gtr_freespace_label_set_dir(GtkWidget* label, char const* dir);

/***
****
***/

void gtr_unrecognized_url_dialog(GtkWidget* parent, char const* url);

void gtr_add_torrent_error_dialog(GtkWidget* window_or_child, int err, tr_torrent* duplicate_torrent, char const* filename);

/* pop up the context menu if a user right-clicks.
   if the row they right-click on isn't selected, select it. */
gboolean on_tree_view_button_pressed(GtkWidget* view, GdkEventButton* event, gpointer unused);

/* if the click didn't specify a row, clear the selection */
gboolean on_tree_view_button_released(GtkWidget* view, GdkEventButton* event, gpointer unused);

/* move a file to the trashcan if GIO is available; otherwise, delete it */
bool gtr_file_trash_or_remove(char const* filename, struct tr_error** error);

void gtr_paste_clipboard_url_into_entry(GtkWidget* entry);

/* Only call gtk_label_set_text() if the new text differs from the old.
 * This prevents the label from having to recalculate its size
 * and prevents selected text in the label from being deselected */
void gtr_label_set_text(GtkLabel* lb, char const* text);
