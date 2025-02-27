/*
 * This file Copyright (C) 2008-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <libtransmission/transmission.h>
#include <libtransmission/log.h>

#include "conf.h"
#include "hig.h"
#include "msgwin.h"
#include "tr-core.h"
#include "tr-prefs.h"
#include "util.h"

enum
{
    COL_SEQUENCE,
    COL_NAME,
    COL_MESSAGE,
    COL_TR_MSG,
    N_COLUMNS
};

struct MsgData
{
    TrCore* core;
    GtkTreeView* view;
    GtkListStore* store;
    GtkTreeModel* filter;
    GtkTreeModel* sort;
    tr_log_level maxLevel;
    gboolean isPaused;
    guint refresh_tag;
};

static struct tr_log_message* myTail = nullptr;
static struct tr_log_message* myHead = nullptr;

/****
*****
****/

/* is the user looking at the latest messages? */
static gboolean is_pinned_to_new(struct MsgData* data)
{
    gboolean pinned_to_new = FALSE;

    if (data->view == nullptr)
    {
        pinned_to_new = TRUE;
    }
    else
    {
        GtkTreePath* last_visible;

        if (gtk_tree_view_get_visible_range(data->view, nullptr, &last_visible))
        {
            GtkTreeIter iter;
            int const row_count = gtk_tree_model_iter_n_children(data->sort, nullptr);

            if (gtk_tree_model_iter_nth_child(data->sort, &iter, nullptr, row_count - 1))
            {
                GtkTreePath* last_row = gtk_tree_model_get_path(data->sort, &iter);
                pinned_to_new = !gtk_tree_path_compare(last_visible, last_row);
                gtk_tree_path_free(last_row);
            }

            gtk_tree_path_free(last_visible);
        }
    }

    return pinned_to_new;
}

static void scroll_to_bottom(struct MsgData* data)
{
    if (data->sort != nullptr)
    {
        GtkTreeIter iter;
        int const row_count = gtk_tree_model_iter_n_children(data->sort, nullptr);

        if (gtk_tree_model_iter_nth_child(data->sort, &iter, nullptr, row_count - 1))
        {
            GtkTreePath* last_row = gtk_tree_model_get_path(data->sort, &iter);
            gtk_tree_view_scroll_to_cell(data->view, last_row, nullptr, TRUE, 1, 0);
            gtk_tree_path_free(last_row);
        }
    }
}

/****
*****
****/

static void level_combo_changed_cb(GtkComboBox* combo_box, gpointer gdata)
{
    auto* data = static_cast<MsgData*>(gdata);
    auto const level = static_cast<tr_log_level>(gtr_combo_box_get_active_enum(combo_box));
    gboolean const pinned_to_new = is_pinned_to_new(data);

    tr_logSetLevel(level);
    gtr_core_set_pref_int(data->core, TR_KEY_message_level, level);
    data->maxLevel = level;
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(data->filter));

    if (pinned_to_new)
    {
        scroll_to_bottom(data);
    }
}

/* similar to asctime, but is utf8-clean */
static char* gtr_asctime(time_t t)
{
    GDateTime* date_time = g_date_time_new_from_unix_local(t);
    gchar* ret = g_date_time_format(date_time, "%a %b %2e %T %Y%n"); /* ctime equiv */
    g_date_time_unref(date_time);
    return ret;
}

static void doSave(GtkWindow* parent, struct MsgData* data, char const* filename)
{
    FILE* fp = fopen(filename, "w+");

    if (fp == nullptr)
    {
        GtkWidget*
            w = gtk_message_dialog_new(parent, {}, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, _("Couldn't save \"%s\""), filename);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(w), "%s", g_strerror(errno));
        g_signal_connect_swapped(w, "response", G_CALLBACK(gtk_widget_destroy), w);
        gtk_widget_show(w);
    }
    else
    {
        GtkTreeIter iter;
        GtkTreeModel* model = GTK_TREE_MODEL(data->sort);

        if (gtk_tree_model_iter_children(model, &iter, nullptr))
        {
            do
            {
                char const* levelStr;
                struct tr_log_message const* node;

                gtk_tree_model_get(model, &iter, COL_TR_MSG, &node, -1);
                gchar* date = gtr_asctime(node->when);

                switch (node->level)
                {
                case TR_LOG_DEBUG:
                    levelStr = "debug";
                    break;

                case TR_LOG_ERROR:
                    levelStr = "error";
                    break;

                default:
                    levelStr = "     ";
                    break;
                }

                fprintf(
                    fp,
                    "%s\t%s\t%s\t%s\n",
                    date,
                    levelStr,
                    node->name != nullptr ? node->name : "",
                    node->message != nullptr ? node->message : "");
                g_free(date);
            } while (gtk_tree_model_iter_next(model, &iter));
        }

        fclose(fp);
    }
}

static void onSaveDialogResponse(GtkWidget* d, int response, gpointer data)
{
    if (response == GTK_RESPONSE_ACCEPT)
    {
        char* file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        doSave(GTK_WINDOW(d), static_cast<MsgData*>(data), file);
        g_free(file);
    }

    gtk_widget_destroy(d);
}

static void onSaveRequest(GtkWidget* w, gpointer data)
{
    GtkWindow* window = GTK_WINDOW(gtk_widget_get_toplevel(w));
    GtkWidget* d = gtk_file_chooser_dialog_new(
        _("Save Log"),
        window,
        GTK_FILE_CHOOSER_ACTION_SAVE,
        TR_ARG_TUPLE(_("_Cancel"), GTK_RESPONSE_CANCEL),
        TR_ARG_TUPLE(_("_Save"), GTK_RESPONSE_ACCEPT),
        nullptr);

    g_signal_connect(d, "response", G_CALLBACK(onSaveDialogResponse), data);
    gtk_widget_show(d);
}

static void onClearRequest(GtkWidget* w, gpointer gdata)
{
    TR_UNUSED(w);

    auto* data = static_cast<MsgData*>(gdata);

    gtk_list_store_clear(data->store);
    tr_logFreeQueue(myHead);
    myHead = myTail = nullptr;
}

static void onPauseToggled(GtkToggleToolButton* w, gpointer gdata)
{
    auto* data = static_cast<MsgData*>(gdata);

    data->isPaused = gtk_toggle_tool_button_get_active(w);
}

static char const* getForegroundColor(int msgLevel)
{
    switch (msgLevel)
    {
    case TR_LOG_DEBUG:
        return "forestgreen";

    case TR_LOG_INFO:
        return "black";

    case TR_LOG_ERROR:
        return "red";

    default:
        g_assert_not_reached();
        return "black";
    }
}

static void renderText(
    GtkTreeViewColumn* column,
    GtkCellRenderer* renderer,
    GtkTreeModel* tree_model,
    GtkTreeIter* iter,
    gpointer gcol)
{
    TR_UNUSED(column);

    int const col = GPOINTER_TO_INT(gcol);
    char* str = nullptr;
    struct tr_log_message const* node;

    gtk_tree_model_get(tree_model, iter, col, &str, COL_TR_MSG, &node, -1);
    g_object_set(
        renderer,
        "text",
        str,
        "foreground",
        getForegroundColor(node->level),
        "ellipsize",
        PANGO_ELLIPSIZE_END,
        nullptr);
}

static void renderTime(
    GtkTreeViewColumn* column,
    GtkCellRenderer* renderer,
    GtkTreeModel* tree_model,
    GtkTreeIter* iter,
    gpointer data)
{
    TR_UNUSED(column);
    TR_UNUSED(data);

    struct tr_log_message const* node;
    gtk_tree_model_get(tree_model, iter, COL_TR_MSG, &node, -1);
    GDateTime* date_time = g_date_time_new_from_unix_local(node->when);
    gchar* buf = g_date_time_format(date_time, "%T");
    g_object_set(renderer, "text", buf, "foreground", getForegroundColor(node->level), nullptr);
    g_free(buf);
    g_date_time_unref(date_time);
}

static void appendColumn(GtkTreeView* view, int col)
{
    GtkCellRenderer* r;
    GtkTreeViewColumn* c;
    char const* title = nullptr;

    switch (col)
    {
    case COL_SEQUENCE:
        title = _("Time");
        break;

    /* noun. column title for a list */
    case COL_NAME:
        title = _("Name");
        break;

    /* noun. column title for a list */
    case COL_MESSAGE:
        title = _("Message");
        break;

    default:
        g_assert_not_reached();
    }

    switch (col)
    {
    case COL_NAME:
        r = gtk_cell_renderer_text_new();
        c = gtk_tree_view_column_new_with_attributes(title, r, nullptr);
        gtk_tree_view_column_set_cell_data_func(c, r, renderText, GINT_TO_POINTER(col), nullptr);
        gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(c, 200);
        gtk_tree_view_column_set_resizable(c, TRUE);
        break;

    case COL_MESSAGE:
        r = gtk_cell_renderer_text_new();
        c = gtk_tree_view_column_new_with_attributes(title, r, nullptr);
        gtk_tree_view_column_set_cell_data_func(c, r, renderText, GINT_TO_POINTER(col), nullptr);
        gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(c, 500);
        gtk_tree_view_column_set_resizable(c, TRUE);
        break;

    case COL_SEQUENCE:
        r = gtk_cell_renderer_text_new();
        c = gtk_tree_view_column_new_with_attributes(title, r, nullptr);
        gtk_tree_view_column_set_cell_data_func(c, r, renderTime, nullptr, nullptr);
        gtk_tree_view_column_set_resizable(c, TRUE);
        break;

    default:
        g_assert_not_reached();
        break;
    }

    gtk_tree_view_append_column(view, c);
}

static gboolean isRowVisible(GtkTreeModel* model, GtkTreeIter* iter, gpointer gdata)
{
    struct tr_log_message const* node;
    auto const* data = static_cast<MsgData const*>(gdata);

    gtk_tree_model_get(model, iter, COL_TR_MSG, &node, -1);

    return node->level <= data->maxLevel;
}

static void onWindowDestroyed(gpointer gdata, GObject* deadWindow)
{
    TR_UNUSED(deadWindow);

    auto* data = static_cast<MsgData*>(gdata);

    g_source_remove(data->refresh_tag);

    g_free(data);
}

static tr_log_message* addMessages(GtkListStore* store, struct tr_log_message* head)
{
    tr_log_message* i;
    static unsigned int sequence = 0;
    char const* default_name = g_get_application_name();

    for (i = head; i != nullptr && i->next != nullptr; i = i->next)
    {
        char const* name = i->name != nullptr ? i->name : default_name;

        gtk_list_store_insert_with_values(
            store,
            nullptr,
            0,
            TR_ARG_TUPLE(COL_TR_MSG, i),
            TR_ARG_TUPLE(COL_NAME, name),
            TR_ARG_TUPLE(COL_MESSAGE, i->message),
            TR_ARG_TUPLE(COL_SEQUENCE, ++sequence),
            -1);

        /* if it's an error message, dump it to the terminal too */
        if (i->level == TR_LOG_ERROR)
        {
            GString* gstr = g_string_sized_new(512);
            g_string_append_printf(gstr, "%s:%d %s", i->file, i->line, i->message);

            if (i->name != nullptr)
            {
                g_string_append_printf(gstr, " (%s)", i->name);
            }

            g_warning("%s", gstr->str);
            g_string_free(gstr, TRUE);
        }
    }

    return i; /* tail */
}

static gboolean onRefresh(gpointer gdata)
{
    auto* data = static_cast<MsgData*>(gdata);
    gboolean const pinned_to_new = is_pinned_to_new(data);

    if (!data->isPaused)
    {
        tr_log_message* msgs = tr_logGetQueue();

        if (msgs != nullptr)
        {
            /* add the new messages and append them to the end of
             * our persistent list */
            tr_log_message* tail = addMessages(data->store, msgs);

            if (myTail != nullptr)
            {
                myTail->next = msgs;
            }
            else
            {
                myHead = msgs;
            }

            myTail = tail;
        }

        if (pinned_to_new)
        {
            scroll_to_bottom(data);
        }
    }

    return G_SOURCE_CONTINUE;
}

static GtkWidget* debug_level_combo_new(void)
{
    GtkWidget* w = gtr_combo_box_new_enum(
        TR_ARG_TUPLE(_("Error"), TR_LOG_ERROR),
        TR_ARG_TUPLE(_("Information"), TR_LOG_INFO),
        TR_ARG_TUPLE(_("Debug"), TR_LOG_DEBUG),
        nullptr);
    gtr_combo_box_set_active_enum(GTK_COMBO_BOX(w), gtr_pref_int_get(TR_KEY_message_level));
    return w;
}

/**
***  Public Functions
**/

GtkWidget* gtr_message_log_window_new(GtkWindow* parent, TrCore* core)
{
    GtkWidget* win;
    GtkWidget* vbox;
    GtkWidget* toolbar;
    GtkWidget* w;
    GtkWidget* view;
    GtkToolItem* item;
    struct MsgData* data;

    data = g_new0(struct MsgData, 1);
    data->core = core;

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    gtk_window_set_title(GTK_WINDOW(win), _("Message Log"));
    gtk_window_set_default_size(GTK_WINDOW(win), 560, 350);
    gtk_window_set_role(GTK_WINDOW(win), "message-log");
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /**
    ***  toolbar
    **/

    toolbar = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), GTK_TOOLBAR_BOTH_HORIZ);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), GTK_STYLE_CLASS_PRIMARY_TOOLBAR);

    item = gtk_tool_button_new(nullptr, nullptr);
    g_object_set(
        item,
        TR_ARG_TUPLE("icon-name", "document-save-as"),
        TR_ARG_TUPLE("is-important", TRUE),
        TR_ARG_TUPLE("label", _("Save _As")),
        TR_ARG_TUPLE("use-underline", TRUE),
        nullptr);
    g_signal_connect(item, "clicked", G_CALLBACK(onSaveRequest), data);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);

    item = gtk_tool_button_new(nullptr, nullptr);
    g_object_set(
        item,
        TR_ARG_TUPLE("icon-name", "edit-clear"),
        TR_ARG_TUPLE("is-important", TRUE),
        TR_ARG_TUPLE("label", _("Clear")),
        TR_ARG_TUPLE("use-underline", TRUE),
        nullptr);
    g_signal_connect(item, "clicked", G_CALLBACK(onClearRequest), data);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);

    item = gtk_separator_tool_item_new();
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);

    item = gtk_toggle_tool_button_new();
    g_object_set(
        G_OBJECT(item),
        TR_ARG_TUPLE("icon-name", "media-playback-pause"),
        TR_ARG_TUPLE("is-important", TRUE),
        TR_ARG_TUPLE("label", _("P_ause")),
        TR_ARG_TUPLE("use-underline", TRUE),
        nullptr);
    g_signal_connect(item, "toggled", G_CALLBACK(onPauseToggled), data);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);

    item = gtk_separator_tool_item_new();
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);

    w = gtk_label_new(_("Level"));
    g_object_set(w, "margin", GUI_PAD, nullptr);
    item = gtk_tool_item_new();
    gtk_container_add(GTK_CONTAINER(item), w);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);

    w = debug_level_combo_new();
    g_signal_connect(w, "changed", G_CALLBACK(level_combo_changed_cb), data);
    item = gtk_tool_item_new();
    gtk_container_add(GTK_CONTAINER(item), w);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);

    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    /**
    ***  messages
    **/

    data->store = gtk_list_store_new(
        N_COLUMNS,
        G_TYPE_UINT, /* sequence */
        G_TYPE_POINTER, /* category */
        G_TYPE_POINTER, /* message */
        G_TYPE_POINTER); /* struct tr_log_message */

    addMessages(data->store, myHead);
    onRefresh(data); /* much faster to populate *before* it has listeners */

    data->filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(data->store), nullptr);
    data->sort = gtk_tree_model_sort_new_with_model(data->filter);
    g_object_unref(data->filter);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(data->sort), COL_SEQUENCE, GTK_SORT_ASCENDING);
    data->maxLevel = static_cast<tr_log_level>(gtr_pref_int_get(TR_KEY_message_level));
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(data->filter), isRowVisible, data, nullptr);

    view = gtk_tree_view_new_with_model(data->sort);
    g_object_unref(data->sort);
    g_signal_connect(view, "button-release-event", G_CALLBACK(on_tree_view_button_released), nullptr);
    data->view = GTK_TREE_VIEW(view);
    appendColumn(data->view, COL_SEQUENCE);
    appendColumn(data->view, COL_NAME);
    appendColumn(data->view, COL_MESSAGE);
    w = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(w), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(w), view);
    gtk_box_pack_start(GTK_BOX(vbox), w, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    data->refresh_tag = gdk_threads_add_timeout_seconds(SECONDARY_WINDOW_REFRESH_INTERVAL_SECONDS, onRefresh, data);
    g_object_weak_ref(G_OBJECT(win), onWindowDestroyed, data);

    scroll_to_bottom(data);
    gtk_widget_show_all(win);
    return win;
}
