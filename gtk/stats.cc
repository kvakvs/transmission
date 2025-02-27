/*
 * This file Copyright (C) 2007-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include "hig.h"
#include "stats.h"
#include "tr-core.h"
#include "tr-prefs.h"
#include "util.h"

enum
{
    TR_RESPONSE_RESET = 1
};

struct stat_ui
{
    GtkLabel* one_up_lb;
    GtkLabel* one_down_lb;
    GtkLabel* one_ratio_lb;
    GtkLabel* one_time_lb;

    GtkLabel* all_up_lb;
    GtkLabel* all_down_lb;
    GtkLabel* all_ratio_lb;
    GtkLabel* all_time_lb;

    GtkLabel* all_sessions_lb;

    TrCore* core;
};

static void setLabel(GtkLabel* l, char const* str)
{
    gtr_label_set_text(l, str);
}

static void setLabelFromRatio(GtkLabel* l, double d)
{
    char buf[128];

    tr_strlratio(buf, d, sizeof(buf));
    setLabel(l, buf);
}

static gboolean updateStats(gpointer gdata)
{
    char buf[128];
    tr_session_stats one;
    tr_session_stats all;
    size_t const buflen = sizeof(buf);
    auto* ui = static_cast<stat_ui*>(gdata);

    tr_sessionGetStats(gtr_core_session(ui->core), &one);
    tr_sessionGetCumulativeStats(gtr_core_session(ui->core), &all);

    setLabel(ui->one_up_lb, tr_strlsize(buf, one.uploadedBytes, buflen));
    setLabel(ui->one_down_lb, tr_strlsize(buf, one.downloadedBytes, buflen));
    setLabel(ui->one_time_lb, tr_strltime(buf, one.secondsActive, buflen));
    setLabelFromRatio(ui->one_ratio_lb, one.ratio);

    char const* const fmt = ngettext("Started %'d time", "Started %'d times", (int)all.sessionCount);
    g_snprintf(buf, buflen, fmt, (int)all.sessionCount);
    setLabel(ui->all_sessions_lb, buf);
    setLabel(ui->all_up_lb, tr_strlsize(buf, all.uploadedBytes, buflen));
    setLabel(ui->all_down_lb, tr_strlsize(buf, all.downloadedBytes, buflen));
    setLabel(ui->all_time_lb, tr_strltime(buf, all.secondsActive, buflen));
    setLabelFromRatio(ui->all_ratio_lb, all.ratio);

    return G_SOURCE_CONTINUE;
}

static void dialogDestroyed(gpointer p, GObject* dialog)
{
    TR_UNUSED(dialog);

    g_source_remove(GPOINTER_TO_UINT(p));
}

static void dialogResponse(GtkDialog* dialog, gint response, gpointer gdata)
{
    auto* ui = static_cast<stat_ui*>(gdata);

    if (response == TR_RESPONSE_RESET)
    {
        char const* primary = _("Reset your statistics?");
        char const* secondary = _(
            "These statistics are for your information only. "
            "Resetting them doesn't affect the statistics logged by your BitTorrent trackers.");
        auto const flags = GtkDialogFlags(GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL);
        GtkWidget* w = gtk_message_dialog_new(GTK_WINDOW(dialog), flags, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", primary);
        gtk_dialog_add_buttons(
            GTK_DIALOG(w),
            TR_ARG_TUPLE(_("_Cancel"), GTK_RESPONSE_CANCEL),
            TR_ARG_TUPLE(_("_Reset"), TR_RESPONSE_RESET),
            nullptr);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(w), "%s", secondary);

        if (gtk_dialog_run(GTK_DIALOG(w)) == TR_RESPONSE_RESET)
        {
            tr_sessionClearStats(gtr_core_session(ui->core));
            updateStats(ui);
        }

        gtk_widget_destroy(w);
    }

    if (response == GTK_RESPONSE_CLOSE)
    {
        gtk_widget_destroy(GTK_WIDGET(dialog));
    }
}

GtkWidget* gtr_stats_dialog_new(GtkWindow* parent, TrCore* core)
{
    guint i;
    GtkWidget* d;
    GtkWidget* t;
    GtkWidget* l;
    guint row = 0;
    struct stat_ui* ui = g_new0(struct stat_ui, 1);

    d = gtk_dialog_new_with_buttons(
        _("Statistics"),
        parent,
        GTK_DIALOG_DESTROY_WITH_PARENT,
        TR_ARG_TUPLE(_("_Reset"), TR_RESPONSE_RESET),
        TR_ARG_TUPLE(_("_Close"), GTK_RESPONSE_CLOSE),
        nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CLOSE);
    t = hig_workarea_create();
    ui->core = core;

    hig_workarea_add_section_title(t, &row, _("Current Session"));
    l = gtk_label_new(nullptr);
    ui->one_up_lb = GTK_LABEL(l);
    gtk_label_set_single_line_mode(ui->one_up_lb, TRUE);
    hig_workarea_add_row(t, &row, _("Uploaded:"), l, nullptr);
    l = gtk_label_new(nullptr);
    ui->one_down_lb = GTK_LABEL(l);
    gtk_label_set_single_line_mode(ui->one_down_lb, TRUE);
    hig_workarea_add_row(t, &row, _("Downloaded:"), l, nullptr);
    l = gtk_label_new(nullptr);
    ui->one_ratio_lb = GTK_LABEL(l);
    gtk_label_set_single_line_mode(ui->one_ratio_lb, TRUE);
    hig_workarea_add_row(t, &row, _("Ratio:"), l, nullptr);
    l = gtk_label_new(nullptr);
    ui->one_time_lb = GTK_LABEL(l);
    gtk_label_set_single_line_mode(ui->one_time_lb, TRUE);
    hig_workarea_add_row(t, &row, _("Duration:"), l, nullptr);

    hig_workarea_add_section_divider(t, &row);

    hig_workarea_add_section_title(t, &row, _("Total"));
    l = gtk_label_new(_("Started %'d time"));
    ui->all_sessions_lb = GTK_LABEL(l);
    gtk_label_set_single_line_mode(ui->all_sessions_lb, TRUE);
    hig_workarea_add_label_w(t, row, l);
    ++row;
    l = gtk_label_new(nullptr);
    ui->all_up_lb = GTK_LABEL(l);
    gtk_label_set_single_line_mode(ui->all_up_lb, TRUE);
    hig_workarea_add_row(t, &row, _("Uploaded:"), l, nullptr);
    l = gtk_label_new(nullptr);
    ui->all_down_lb = GTK_LABEL(l);
    gtk_label_set_single_line_mode(ui->all_down_lb, TRUE);
    hig_workarea_add_row(t, &row, _("Downloaded:"), l, nullptr);
    l = gtk_label_new(nullptr);
    ui->all_ratio_lb = GTK_LABEL(l);
    gtk_label_set_single_line_mode(ui->all_ratio_lb, TRUE);
    hig_workarea_add_row(t, &row, _("Ratio:"), l, nullptr);
    l = gtk_label_new(nullptr);
    ui->all_time_lb = GTK_LABEL(l);
    gtk_label_set_single_line_mode(ui->all_time_lb, TRUE);
    hig_workarea_add_row(t, &row, _("Duration:"), l, nullptr);

    gtr_dialog_set_content(GTK_DIALOG(d), t);

    updateStats(ui);
    g_object_set_data_full(G_OBJECT(d), "data", ui, g_free);
    g_signal_connect(d, "response", G_CALLBACK(dialogResponse), ui);
    i = gdk_threads_add_timeout_seconds(SECONDARY_WINDOW_REFRESH_INTERVAL_SECONDS, updateStats, ui);
    g_object_weak_ref(G_OBJECT(d), dialogDestroyed, GUINT_TO_POINTER(i));
    return d;
}
