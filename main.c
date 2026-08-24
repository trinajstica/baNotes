/* Application version — update here when releasing */
#define VERZIJA "baNotes v1.03"

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <pango/pango.h>
#include <libayatana-appindicator/app-indicator.h>
#include "include/app.h"


// Single-instance support
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

// Global variables
static GtkWidget *main_window = NULL;
static GtkWidget *search_entry = NULL;
static GtkWidget *clear_btn = NULL;
static GtkWidget *folder_combo = NULL;
static GtkListStore *notes_store = NULL;
static GtkWidget *tree = NULL;
static AppIndicator *indicator = NULL;
static int current_x = -1;
static int current_y = -1;
static char *last_selected_note = NULL;  // RAM-only: last selected note title
static char *current_folder = NULL;      // relative folder path, empty means root
static guint search_reload_timer = 0;

typedef struct {
    char *path;
    gint kind;
} DragSource;
static GList *pending_drag_sources = NULL;

enum NoteRowKind {
    ROW_NOTE = 0,
    ROW_FOLDER = 1,
    ROW_PARENT = 2
};

// Napoved funkcij
static gboolean on_tree_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static void on_quit(GtkMenuItem *item, gpointer user_data);
static void on_show_hide(GtkMenuItem *item, gpointer user_data);
static void on_search_changed(GtkEntry *entry, gpointer user_data);
static void on_clear_clicked(GtkButton *btn, gpointer user_data);
static void on_folder_changed(GtkComboBox *combo, gpointer user_data);
static void on_new_folder_clicked(GtkButton *btn, gpointer user_data);
static gboolean on_tree_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static void on_drag_data_get(GtkWidget *widget, GdkDragContext *context,
    GtkSelectionData *selection_data, guint info, guint time, gpointer user_data);
static void on_drag_end(GtkWidget *widget, GdkDragContext *context, gpointer user_data);
static void on_drag_data_received(GtkWidget *widget, GdkDragContext *context,
    gint x, gint y, GtkSelectionData *selection_data, guint info, guint time, gpointer user_data);
static void on_rename_folder_activate(GtkMenuItem *item, gpointer user_data);
static void on_delete_folder_activate(GtkMenuItem *item, gpointer user_data);
static gboolean on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data);
static gboolean on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data);
static void show_main_window(void);
static GtkWidget* create_main_window(void);
static void create_tray_icon(void);
static void editor_destroy(GtkWidget *w, gpointer user_data);
static void on_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data);
static gboolean on_wrap_label_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
/* forward editor type so prototypes can reference it */
typedef struct EditorData EditorData;
static gboolean on_textview_keypress_rich(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static void on_bold_toggled(GtkToggleButton *btn, gpointer user_data);
static void on_italic_toggled(GtkToggleButton *btn, gpointer user_data);
static void on_underline_toggled(GtkToggleButton *btn, gpointer user_data);
static void on_fg_color_set(GtkColorButton *cbtn, gpointer user_data);
static void on_bg_color_set(GtkColorButton *cbtn, gpointer user_data);
static void on_buffer_mark_set(GtkTextBuffer *buffer, GtkTextIter *location,
    GtkTextMark *mark, gpointer user_data);
static GtkWidget *create_rich_toolbar_for_editor(EditorData *ed);
static void on_insert_text(GtkTextBuffer *buffer, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
static void on_buffer_changed(GtkTextBuffer *buffer, gpointer user_data);
static void apply_app_css(void);
// single-instance socket path
static char socket_path[4096] = {0};
static int server_sock = -1;

// Forward
static void *instance_server_thread(void *arg);
static gboolean bring_main_window(gpointer user_data);
static gboolean reload_search_cb(gpointer user_data);


static void cleanup_socket(void) {
    if (server_sock != -1) close(server_sock);
    if (socket_path[0]) unlink(socket_path);
}
static void editor_autosave(EditorData *ed);
static char *first_nonempty_line(const char *text);
static GtkWidget* create_editor_dialog(const char *window_title, const char *note_title, GtkTextBuffer *existing_buffer);
static void reload_folders(void);
static void select_last_or_first_note(void);
static void navigate_to_folder(const char *folder);
static void folder_text_cell_data_func(GtkTreeViewColumn *column, GtkCellRenderer *renderer,
    GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data);

// Editor data
typedef struct EditorData EditorData;

// Editor data
struct EditorData {
    char *title; // brez .txt
    char *folder; // relative folder for a newly created note
    gboolean is_new_note;
    GtkTextBuffer *buffer;
    GtkWidget *window;
    GtkWidget *tview;
    gboolean bold_mode;
    gboolean italic_mode;
    gboolean underline_mode;
    char *fg_color; /* e.g. "#rrggbb" or NULL */
    char *bg_color;
    GtkWidget *bold_btn;
    GtkWidget *italic_btn;
    GtkWidget *underline_btn;
    GtkWidget *fg_btn;
    GtkWidget *bg_btn;
    GList *undo_stack; // list of char* snapshots (serialized)
    GList *redo_stack; // list of char* snapshots
    int max_history;
    gboolean ignore_changes;
    guint debounce_timer; /* timer id for grouping keypress snapshots */
    guint autosave_timer; /* timer id for autosave debounce */
    gboolean dirty;       /* buffer changed since last successful autosave */
    GList *insert_idle_ids; /* pending tag-application callbacks */
    gboolean destroying;
};

static char *sanitize_title(const char *s) {
    if (!s) return NULL;
    // kopiraj in odstrani prepovedane znake (najprej trim)
    const char *start = s;
    while (*start && g_ascii_isspace(*start)) start++;
    const char *end = s + strlen(s) - 1;
    while (end >= start && g_ascii_isspace(*end)) end--;
    size_t len = (end >= start) ? (size_t)(end - start + 1) : 0;
    char *out = g_malloc(len + 1 + 32);
    size_t j = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = start[i];
        if (c == '/' || c == '\\' || c == '\0' || c == '\n' || c == '\r') {
            out[j++] = '_';
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
    // Če je prazno, ustvari untitled_<ts>
    if (j == 0) {
        time_t t = time(NULL);
        snprintf(out, len + 1 + 32, "untitled_%ld", (long)t);
    }
    return out;
}

static gboolean note_file_exists(const char *title) {
    return title && app_note_exists(title);
}

/* Temporary names for new notes stay hidden from the notes list until the
 * user enters actual content and the note receives its real title. */
static char *make_temporary_note_title(const char *folder) {
    time_t now = time(NULL);
    for (int suffix = 0; ; ++suffix) {
        gchar *name = suffix == 0
            ? g_strdup_printf(".untitled_%ld", (long)now)
            : g_strdup_printf(".untitled_%ld-%d", (long)now, suffix);
        gchar *candidate = (folder && *folder)
            ? g_build_filename(folder, name, NULL) : g_strdup(name);
        g_free(name);
        if (!note_file_exists(candidate)) return candidate;
        g_free(candidate);
    }
}

// Handler: selection changed
static void on_selection_changed(GtkTreeSelection *selection, gpointer data) {
    (void)data;
    GtkTreeModel *model = NULL;
    GList *rows = gtk_tree_selection_get_selected_rows(selection, &model);
    for (GList *item = rows; item; item = item->next) {
        GtkTreePath *path = item->data;
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter(model, &iter, path)) {
            gchar *title = NULL;
            gint row_kind = ROW_NOTE;
            gtk_tree_model_get(model, &iter, 2, &title, 3, &row_kind, -1);
            if (title && row_kind == ROW_NOTE) {
                // Save the first selected note to RAM only.
                g_free(last_selected_note);
                last_selected_note = g_strdup(title);
                g_free(title);
                break;
            }
            g_free(title);
        }
    }
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
}

// Helper: reload notes while blocking selection signal to preserve last_selected_note
static void reload_notes_safely(const char *filter) {
    if (!tree || !notes_store) return;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
    // Block the signal so on_selection_changed isn't triggered by clearing/refilling the list
    g_signal_handlers_block_by_func(sel, G_CALLBACK(on_selection_changed), NULL);
    app_load_notes(notes_store, filter, current_folder);
    g_signal_handlers_unblock_by_func(sel, G_CALLBACK(on_selection_changed), NULL);
}

static void clear_pending_drag_sources(void) {
    for (GList *item = pending_drag_sources; item; item = item->next) {
        DragSource *source = item->data;
        g_free(source->path);
        g_free(source);
    }
    g_list_free(pending_drag_sources);
    pending_drag_sources = NULL;
}

static void capture_pending_drag_sources(GtkTreeView *treeview) {
    clear_pending_drag_sources();
    GtkTreeSelection *selection = gtk_tree_view_get_selection(treeview);
    GtkTreeModel *model = NULL;
    GList *rows = gtk_tree_selection_get_selected_rows(selection, &model);
    for (GList *item = rows; item; item = item->next) {
        GtkTreeIter iter;
        GtkTreePath *path = item->data;
        if (gtk_tree_model_get_iter(model, &iter, path)) {
            gchar *source_path = NULL;
            gint row_kind = ROW_NOTE;
            gtk_tree_model_get(model, &iter, 2, &source_path, 3, &row_kind, -1);
            if (source_path && (row_kind == ROW_NOTE || row_kind == ROW_FOLDER)) {
                DragSource *source = g_new0(DragSource, 1);
                source->path = source_path;
                source->kind = row_kind;
                pending_drag_sources = g_list_append(pending_drag_sources, source);
            } else {
                g_free(source_path);
            }
        }
        gtk_tree_path_free(path);
    }
    g_list_free(rows);
}


// Handler: klik na tree view - preveri, če je klik na stolpcu smeti
static gboolean on_tree_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    if (event->type != GDK_BUTTON_PRESS || (event->button != 1 && event->button != 3)) return FALSE;
    GtkTreeView *treeview = GTK_TREE_VIEW(widget);
    int x = (int)event->x, y = (int)event->y;
    GtkTreePath *path = NULL;
    GtkTreeViewColumn *col = NULL;
    if (gtk_tree_view_get_path_at_pos(treeview, x, y, &path, &col, NULL, NULL)) {
        if (event->button == 3) {
            GtkTreeModel *model = gtk_tree_view_get_model(treeview);
            GtkTreeIter iter;
            if (gtk_tree_model_get_iter(model, &iter, path)) {
                gchar *folder = NULL;
                gint row_kind = ROW_NOTE;
                gtk_tree_model_get(model, &iter, 2, &folder, 3, &row_kind, -1);
                if (folder && row_kind == ROW_FOLDER) {
                    GtkTreeSelection *selection = gtk_tree_view_get_selection(treeview);
                    gtk_tree_selection_unselect_all(selection);
                    gtk_tree_selection_select_path(selection, path);
                    gtk_tree_view_set_cursor(treeview, path, NULL, FALSE);

                    GtkWidget *menu = gtk_menu_new();
                    g_object_set_data_full(G_OBJECT(menu), "folder-path", folder, g_free);
                    GtkWidget *rename_item = gtk_menu_item_new_with_label("Rename folder");
                    GtkWidget *delete_item = gtk_menu_item_new_with_label("Delete folder");
                    gtk_menu_shell_append(GTK_MENU_SHELL(menu), rename_item);
                    gtk_menu_shell_append(GTK_MENU_SHELL(menu), delete_item);
                    g_signal_connect(rename_item, "activate",
                        G_CALLBACK(on_rename_folder_activate), menu);
                    g_signal_connect(delete_item, "activate",
                        G_CALLBACK(on_delete_folder_activate), menu);
                    gtk_widget_show_all(menu);
                    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
                    gtk_tree_path_free(path);
                    return TRUE;
                }
                g_free(folder);
            }
            gtk_tree_path_free(path);
            return TRUE;
        }
        GtkTreeSelection *selection = gtk_tree_view_get_selection(treeview);
        if (gtk_tree_selection_path_is_selected(selection, path) &&
            !(event->state & GDK_SHIFT_MASK)) {
            capture_pending_drag_sources(treeview);
        } else {
            clear_pending_drag_sources();
        }
        // Primerjamo, ali je stolpec za smeti (2. stolpec, indeks 1)
        GtkTreeViewColumn *trash_col = gtk_tree_view_get_column(treeview, 1);
        if (col == trash_col) {
            GtkTreeModel *model = gtk_tree_view_get_model(treeview);
            GtkTreeIter iter;
            if (gtk_tree_model_get_iter(model, &iter, path)) {
                gchar *title = NULL;
                gchar *display_title = NULL;
                gint row_kind = ROW_NOTE;
                gtk_tree_model_get(model, &iter, 2, &title, 0, &display_title, 3, &row_kind, -1);
                if (title && row_kind == ROW_NOTE) {
                    /* Remember selected index before reloading notes, so we can restore
                     * selection after deleting a note. This ensures that deleting the
                     * currently selected note keeps the cursor at the same visual
                     * position (or moves to the new last if the deleted one was
                     * last). If nothing was selected, we leave selection as-is. */
                    /* Save the clicked location (path index) so we can reselect the proper
                     * item after reloading. If the click happened on a path, prefer that
                     * index even if the item wasn't selected. */
                    int saved_index = -1;
                    const gint *clicked_indices = gtk_tree_path_get_indices(path);
                    if (clicked_indices) {
                        saved_index = clicked_indices[0];
                        /* Make the clicked item appear selected in the UI (clicking trash
                         * implies the user's intent for that location). */
                        GtkTreePath *sel_path = gtk_tree_path_new_from_indices(saved_index, -1);
                        if (sel_path) {
                            GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
                            gtk_tree_selection_select_path(sel, sel_path);
                            gtk_tree_view_set_cursor(GTK_TREE_VIEW(tree), sel_path, NULL, FALSE);
                            gtk_tree_path_free(sel_path);
                        }
                    } else {
                        /* Fall back to current selection when clicked path is not available */
                        GtkTreeSelection *cur_sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
                        GtkTreeModel *cur_model = NULL;
                        GList *selected_rows = gtk_tree_selection_get_selected_rows(cur_sel, &cur_model);
                        if (selected_rows) {
                            GtkTreePath *cur_path = selected_rows->data;
                            const gint *indices = gtk_tree_path_get_indices(cur_path);
                            if (indices) saved_index = indices[0];
                        }
                        g_list_free_full(selected_rows, (GDestroyNotify)gtk_tree_path_free);
                    }
                    char *dmsg = g_strdup_printf("Delete note '%s'?", display_title ? display_title : title);
                    GtkWidget *dialog = gtk_dialog_new_with_buttons("Delete note",
                        GTK_WINDOW(main_window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                        "Yes", GTK_RESPONSE_YES,
                        "No", GTK_RESPONSE_NO,
                        NULL);
                    GtkWidget *carea = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
                    /* Add margins to the content area so the dialog looks nicer */
                    gtk_widget_set_margin_start(carea, 12);
                    gtk_widget_set_margin_end(carea, 12);
                    gtk_widget_set_margin_top(carea, 8);
                    gtk_widget_set_margin_bottom(carea, 8);
                    GtkWidget *label = gtk_label_new(dmsg);
                    /* Add a bit of padding around the label as well */
                    gtk_widget_set_margin_start(label, 6);
                    gtk_widget_set_margin_end(label, 6);
                    gtk_widget_set_margin_top(label, 6);
                    gtk_widget_set_margin_bottom(label, 6);
                    gtk_container_add(GTK_CONTAINER(carea), label);
                    gtk_widget_show_all(dialog);
                    int resp = gtk_dialog_run(GTK_DIALOG(dialog));
                    gtk_widget_destroy(dialog);
                    g_free(dmsg);
                    g_free(display_title);
                    if (resp == GTK_RESPONSE_YES) {
                        if (app_delete_note(title)) {
                            // Reload notes
                            reload_notes_safely(gtk_entry_get_text(GTK_ENTRY(search_entry)));
                            // Restore selection if we remembered one and the list is not empty
                            if (saved_index >= 0 && tree && notes_store) {
                                GtkTreeModel *model = GTK_TREE_MODEL(notes_store);
                                gint nrows = gtk_tree_model_iter_n_children(model, NULL);
                                if (nrows > 0) {
                                        gint reselect = saved_index;
                                        /* If the saved index is outside the new bounds (e.g., we
                                         * deleted the last note), prefer selecting the first note
                                         * when possible rather than the last. */
                                        if (reselect >= nrows) reselect = 0;
                                    GtkTreePath *path = gtk_tree_path_new_from_indices(reselect, -1);
                                    GtkTreeSelection *sel2 = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
                                    gtk_tree_selection_select_path(sel2, path);
                                    gtk_tree_view_set_cursor(GTK_TREE_VIEW(tree), path, NULL, FALSE);
                                    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(tree), path, NULL, TRUE, 0.5, 0.0);
                                    gtk_tree_path_free(path);
                                }
                            }
                        } else {
                            GtkWidget *err = gtk_dialog_new_with_buttons("Error",
                                GTK_WINDOW(main_window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                "Ok", GTK_RESPONSE_OK,
                                NULL);
                            GtkWidget *err_area = gtk_dialog_get_content_area(GTK_DIALOG(err));
                            GtkWidget *err_label = gtk_label_new("Error deleting note!");
                            gtk_container_add(GTK_CONTAINER(err_area), err_label);
                            gtk_widget_show_all(err);
                            gtk_dialog_run(GTK_DIALOG(err));
                            gtk_widget_destroy(err);
                        }
                    }
                    g_free(title);
                }
                else {
                    g_free(title);
                    g_free(display_title);
                }
            }
            gtk_tree_path_free(path);
            return TRUE;
        }
        gtk_tree_path_free(path);
    }
    return FALSE;
}

static gboolean on_tree_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    (void)user_data;
    if ((event->state & GDK_CONTROL_MASK) &&
        (event->keyval == GDK_KEY_a || event->keyval == GDK_KEY_A)) {
        gtk_tree_selection_select_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(widget)));
        return TRUE;
    }
    return FALSE;
}

static void folder_text_cell_data_func(GtkTreeViewColumn *column, GtkCellRenderer *renderer,
    GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data) {
    (void)column; (void)user_data;
    gint row_kind = ROW_NOTE;
    gtk_tree_model_get(model, iter, 3, &row_kind, -1);
    if (row_kind == ROW_FOLDER) {
        g_object_set(renderer, "foreground", "#76b7ff", "foreground-set", TRUE, NULL);
    } else if (row_kind == ROW_PARENT) {
        g_object_set(renderer, "foreground", "#aab4c3", "foreground-set", TRUE, NULL);
    } else {
        g_object_set(renderer, "foreground-set", FALSE, NULL);
    }
}

static void append_drag_source_payload(GString *payload, const char *source, gint row_kind) {
    if (!source || (row_kind != ROW_NOTE && row_kind != ROW_FOLDER)) return;
    gchar *encoded = g_base64_encode((const guchar *)source, strlen(source));
    g_string_append_printf(payload, "%d|%s\n", row_kind, encoded);
    g_free(encoded);
}

static void on_drag_data_get(GtkWidget *widget, GdkDragContext *context,
    GtkSelectionData *selection_data, guint info, guint time, gpointer user_data) {
    (void)context; (void)info; (void)time; (void)user_data;
    GString *payload = g_string_new(NULL);

    if (pending_drag_sources) {
        for (GList *item = pending_drag_sources; item; item = item->next) {
            DragSource *source = item->data;
            append_drag_source_payload(payload, source->path, source->kind);
        }
    } else {
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
        GtkTreeModel *model = NULL;
        GList *rows = gtk_tree_selection_get_selected_rows(selection, &model);
        for (GList *item = rows; item; item = item->next) {
            GtkTreeIter iter;
            GtkTreePath *path = item->data;
            if (gtk_tree_model_get_iter(model, &iter, path)) {
                gchar *source = NULL;
                gint row_kind = ROW_NOTE;
                gtk_tree_model_get(model, &iter, 2, &source, 3, &row_kind, -1);
                append_drag_source_payload(payload, source, row_kind);
                g_free(source);
            }
            gtk_tree_path_free(path);
        }
        g_list_free(rows);
    }
    gtk_selection_data_set_text(selection_data, payload->str, payload->len);
    g_string_free(payload, TRUE);
}

static void on_drag_end(GtkWidget *widget, GdkDragContext *context, gpointer user_data) {
    (void)widget; (void)context; (void)user_data;
    clear_pending_drag_sources();
}

static void on_drag_data_received(GtkWidget *widget, GdkDragContext *context,
    gint x, gint y, GtkSelectionData *selection_data, guint info, guint time, gpointer user_data) {
    (void)info; (void)user_data;
    GtkTreeView *tree_view = GTK_TREE_VIEW(widget);
    GtkTreePath *dest_path = NULL;
    GtkTreeViewDropPosition drop_pos;
    gboolean accepted = FALSE;
    gint moved = 0;
    gint attempted = 0;
    gint failed = 0;
    GString *failed_sources = NULL;

    if (!gtk_tree_view_get_dest_row_at_pos(tree_view, x, y, &dest_path, &drop_pos)) {
        gtk_drag_finish(context, FALSE, FALSE, time);
        return;
    }

    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    GtkTreeIter dest_iter;
    if (!gtk_tree_model_get_iter(model, &dest_iter, dest_path)) {
        gtk_tree_path_free(dest_path);
        gtk_drag_finish(context, FALSE, FALSE, time);
        return;
    }

    gchar *destination = NULL;
    gint destination_kind = ROW_NOTE;
    gtk_tree_model_get(model, &dest_iter, 2, &destination, 3, &destination_kind, -1);
    gtk_tree_path_free(dest_path);
    if (!destination || (destination_kind != ROW_FOLDER && destination_kind != ROW_PARENT)) {
        g_free(destination);
        gtk_drag_finish(context, FALSE, FALSE, time);
        return;
    }
    failed_sources = g_string_new(NULL);

    gchar *payload = (gchar *)gtk_selection_data_get_text(selection_data);
    if (payload) {
        gchar **lines = g_strsplit(payload, "\n", -1);
        for (gint i = 0; lines[i]; ++i) {
            if (!*lines[i]) continue;
            gchar **parts = g_strsplit(lines[i], "|", 2);
            if (parts[0] && parts[1]) {
                gint row_kind = atoi(parts[0]);
                gsize decoded_len = 0;
                guchar *decoded = g_base64_decode(parts[1], &decoded_len);
                if (decoded && (row_kind == ROW_NOTE || row_kind == ROW_FOLDER)) {
                    accepted = TRUE;
                    attempted++;
                    if (app_move_entry((const char *)decoded, destination,
                        row_kind == ROW_FOLDER)) {
                        moved++;
                    } else {
                        failed++;
                        g_string_append_printf(failed_sources, "%s\n", (const char *)decoded);
                    }
                }
                g_free(decoded);
            }
            g_strfreev(parts);
        }
        g_strfreev(lines);
        g_free(payload);
    }
    g_free(destination);

    if (moved > 0) {
        reload_folders();
        reload_notes_safely(search_entry ? gtk_entry_get_text(GTK_ENTRY(search_entry)) : "");
        select_last_or_first_note();
    }
    gtk_drag_finish(context, accepted && moved > 0, FALSE, time);
    if (failed > 0) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(main_window),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_WARNING,
            GTK_BUTTONS_OK,
            "Moved %d of %d selected items. These could not be moved:\n%s",
            moved, attempted, failed_sources->str);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }
    g_string_free(failed_sources, TRUE);
}

static gboolean editor_autosave_cb(gpointer user_data) {
    EditorData *ed = (EditorData*)user_data;
    if (!ed) return G_SOURCE_REMOVE;
    ed->autosave_timer = 0;
    editor_autosave(ed);
    return G_SOURCE_REMOVE;
}

static void editor_schedule_autosave(EditorData *ed) {
    if (!ed) return;
    if (ed->autosave_timer) {
        g_source_remove(ed->autosave_timer);
        ed->autosave_timer = 0;
    }
    ed->autosave_timer = g_timeout_add(300, editor_autosave_cb, ed);
}

static void editor_mark_dirty(EditorData *ed) {
    if (!ed) return;
    ed->dirty = TRUE;
    editor_schedule_autosave(ed);
}

static void editor_autosave(EditorData *ed) {
    if (!ed || !ed->buffer) return;

    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(ed->buffer, &start);
    gtk_text_buffer_get_end_iter(ed->buffer, &end);
    gchar *text = gtk_text_buffer_get_text(ed->buffer, &start, &end, FALSE);
    char *first = first_nonempty_line(text);

    /* An untouched new note is only a temporary hidden file.  Keep it while
     * the editor is open, but remove it when the user closes the editor. */
    if (ed->is_new_note && (!first || !*first)) {
        if (ed->destroying && ed->title && note_file_exists(ed->title))
            app_delete_note(ed->title);
        g_free(first);
        g_free(text);
        return;
    }

    char *desired_title = NULL;
    if (first && *first) desired_title = sanitize_title(first);
    else {
        time_t t = time(NULL);
        desired_title = g_strdup_printf("untitled_%ld", (long)t);
    }

    char *desired_path = (ed->folder && *ed->folder)
        ? g_build_filename(ed->folder, desired_title, NULL) : g_strdup(desired_title);

    if (ed->is_new_note) {
        char *unique = g_strdup(desired_path);
        int suffix = 1;
        while (note_file_exists(unique)) {
            g_free(unique);
            char *unique_name = g_strdup_printf("%s-%d", desired_title, suffix++);
            unique = (ed->folder && *ed->folder)
                ? g_build_filename(ed->folder, unique_name, NULL) : unique_name;
            if (ed->folder && *ed->folder) g_free(unique_name);
        }
        if (app_rename_note(ed->title, unique)) {
            g_free(ed->title);
            ed->title = unique;
            ed->is_new_note = FALSE;
            if (last_selected_note) g_free(last_selected_note);
            last_selected_note = g_strdup(ed->title);
        } else {
            g_free(unique);
        }
    } else if (g_strcmp0(desired_path, ed->title) != 0) {
        /* Če cilj že obstaja, obdrži star naslov brez pop-upov med tipkanjem. */
        if (!note_file_exists(desired_path) && app_rename_note(ed->title, desired_path)) {
            g_free(ed->title);
            ed->title = g_strdup(desired_path);
            if (last_selected_note) g_free(last_selected_note);
            last_selected_note = g_strdup(ed->title);
        }
    }

    if (ed->title) {
        char *ser = app_serialize_buffer_rich(ed->buffer);
        if (!ser) ser = g_strdup(text ? text : "");
        if (app_write_note(ed->title, ser)) {
            ed->dirty = FALSE;
            /* Osveži šele po zapisu, sicer se nova nota pri prvem
             * osveževanju še ne more pojaviti v seznamu. */
            reload_notes_safely(search_entry ? gtk_entry_get_text(GTK_ENTRY(search_entry)) : "");
        } else {
            g_printerr("baNotes: could not save note '%s'\n", ed->title);
        }
        g_free(ser);
    }

    if (first) g_free(first);
    g_free(desired_path);
    g_free(desired_title);
    g_free(text);
}

// Push the current buffer snapshot on undo stack
static void editor_push_snapshot(EditorData *ed) {
    if (!ed) return;
    if (ed->ignore_changes) return;
    char *snap = app_serialize_buffer_rich(ed->buffer);
    if (!snap) return;
    // Avoid duplicate snapshots if identical to last
    if (ed->undo_stack && ed->undo_stack->data && g_strcmp0((char*)ed->undo_stack->data, snap) == 0) {
        g_free(snap); return;
    }
    ed->undo_stack = g_list_prepend(ed->undo_stack, snap);
    // Trim history to max
    while (ed->max_history > 0 && g_list_length(ed->undo_stack) > ed->max_history) {
        GList *last = g_list_last(ed->undo_stack);
        if (last) {
            g_free(last->data);
            ed->undo_stack = g_list_delete_link(ed->undo_stack, last);
        }
    }
    // Clearing redo on a new snapshot
    g_list_free_full(ed->redo_stack, g_free);
    ed->redo_stack = NULL;
}

/* Debounce timer callback: push snapshot and clear timer id */
static gboolean editor_debounce_snapshot_cb(gpointer user_data) {
    EditorData *ed = (EditorData*)user_data;
    if (!ed) return G_SOURCE_REMOVE;
    ed->debounce_timer = 0;
    editor_push_snapshot(ed);
    return G_SOURCE_REMOVE;
}

/* Schedule a snapshot push using debounce timer (500ms). If a timer exists, reset it. */
static void editor_schedule_snapshot(EditorData *ed) {
    if (!ed) return;
    /* If a timer is already scheduled, remove it */
    if (ed->debounce_timer) {
        g_source_remove(ed->debounce_timer);
        ed->debounce_timer = 0;
    }
    /* Schedule new timer */
    ed->debounce_timer = g_timeout_add(500, editor_debounce_snapshot_cb, ed);
}

// Apply a snapshot string into editor buffer
static void editor_apply_snapshot(EditorData *ed, const char *snap) {
    if (!ed || !snap) return;
    ed->ignore_changes = TRUE;
    app_parse_rich_string_into_buffer(snap, ed->buffer);
    ed->ignore_changes = FALSE;
}

// Undo: push current state to redo and pop from undo stack
static void editor_undo(EditorData *ed) {
    if (!ed || !ed->undo_stack) return;
    char *cur = app_serialize_buffer_rich(ed->buffer);
    if (cur) ed->redo_stack = g_list_prepend(ed->redo_stack, cur);
    GList *first = ed->undo_stack;
    char *snap = (char*)first->data;
    ed->undo_stack = g_list_delete_link(ed->undo_stack, first);
    editor_apply_snapshot(ed, snap);
    g_free(snap);
}

// Redo: push current state to undo and pop from redo stack
static void editor_redo(EditorData *ed) {
    if (!ed || !ed->redo_stack) return;
    char *cur = app_serialize_buffer_rich(ed->buffer);
    if (cur) ed->undo_stack = g_list_prepend(ed->undo_stack, cur);
    GList *first = ed->redo_stack;
    char *snap = (char*)first->data;
    ed->redo_stack = g_list_delete_link(ed->redo_stack, first);
    editor_apply_snapshot(ed, snap);
    g_free(snap);
}

static void on_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data) {
    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) return;
    gchar *title = NULL;
    gchar *display_title = NULL;
    gint row_kind = ROW_NOTE;
    gtk_tree_model_get(model, &iter, 2, &title, 0, &display_title, 3, &row_kind, -1);
    if (!title) {
        g_free(display_title);
        return;
    }

    if (row_kind == ROW_FOLDER || row_kind == ROW_PARENT) {
        navigate_to_folder(title);
        g_free(display_title);
        g_free(title);
        return;
    }

    /* Create buffer and load note content */
    GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
    app_load_note_into_buffer(title, buffer);

    /* Create editor dialog with loaded content */
    char window_title[512];
    snprintf(window_title, sizeof(window_title), "Edit: %s", display_title ? display_title : title);
    GtkWidget *dlg = create_editor_dialog(window_title, title, buffer);
    
    gtk_widget_show_all(dlg);
    g_free(display_title);
    g_free(title);
}

static void editor_destroy(GtkWidget *w, gpointer user_data) {
    EditorData *ed = (EditorData*)user_data;
    if (!ed) return;
    ed->destroying = TRUE;
    if (ed->debounce_timer) { g_source_remove(ed->debounce_timer); ed->debounce_timer = 0; }
    if (ed->autosave_timer) { g_source_remove(ed->autosave_timer); ed->autosave_timer = 0; }
    for (GList *item = ed->insert_idle_ids; item; item = item->next)
        g_source_remove(GPOINTER_TO_UINT(item->data));
    g_list_free(ed->insert_idle_ids);
    ed->insert_idle_ids = NULL;
    /* Always flush once on close.  This also covers buffer changes for which
     * GTK did not emit the expected changed signal. */
    editor_autosave(ed);
    if (ed->buffer) {
        g_signal_handlers_disconnect_by_func(ed->buffer, G_CALLBACK(on_insert_text), ed);
        g_signal_handlers_disconnect_by_func(ed->buffer, G_CALLBACK(on_buffer_changed), ed);
        g_signal_handlers_disconnect_by_func(ed->buffer, G_CALLBACK(on_buffer_mark_set), ed);
    }
    if (ed->title) g_free(ed->title);
    if (ed->folder) g_free(ed->folder);
    if (ed->fg_color) g_free(ed->fg_color);
    if (ed->bg_color) g_free(ed->bg_color);
    if (ed->undo_stack) g_list_free_full(ed->undo_stack, g_free);
    if (ed->redo_stack) g_list_free_full(ed->redo_stack, g_free);
    g_free(ed);
}

/* Create 'clipboard' operation: copy current selection into a buffer for later paste */
static void editor_copy_to_clipboard(EditorData *ed) {
    if (!ed || !ed->buffer) return;
    GtkTextIter s,e;
    if (!gtk_text_buffer_get_selection_bounds(ed->buffer, &s, &e)) return;
    gchar *selected = gtk_text_buffer_get_text(ed->buffer, &s, &e, FALSE);
    if (!selected) return;
    // store into a per-editor 'clipboard' data stored in editor struct (use fg/bg?)
    // Reuse redo_stack as a clipboard store? Better: add new field; but for quick approach, add clipboard string.
    char *oldcb = (char*)g_object_get_data(G_OBJECT(ed->window), "editor-clipboard");
    if (oldcb) g_free(oldcb);
    g_object_set_data_full(G_OBJECT(ed->window), "editor-clipboard", g_strdup(selected), g_free);
    g_free(selected);
}

/* Paste clipboard contents into buffer at cursor position */
static void editor_paste_from_clipboard(EditorData *ed) {
    if (!ed || !ed->buffer) return;
    char *clip = (char*)g_object_get_data(G_OBJECT(ed->window), "editor-clipboard");
    if (!clip) return;
    GtkTextIter it;
    GtkTextMark *m = gtk_text_buffer_get_insert(ed->buffer);
    gtk_text_buffer_get_iter_at_mark(ed->buffer, &it, m);
    // Save snapshot before pasting. Cancel pending debounced snapshot if any.
    if (ed && ed->debounce_timer) { g_source_remove(ed->debounce_timer); ed->debounce_timer = 0; }
    editor_push_snapshot(ed);
    gtk_text_buffer_insert(ed->buffer, &it, clip, -1);
}

/* UI rich-text helpers and callbacks */
static GtkTextTag* ui_get_or_create_tag(GtkTextBuffer *buffer, const char *name) {
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
    GtkTextTag *tag = NULL;
    if (table) tag = gtk_text_tag_table_lookup(table, name);
    if (tag) return tag;
    if (g_strcmp0(name, "BOLD") == 0) {
        tag = gtk_text_buffer_create_tag(buffer, name, "weight", PANGO_WEIGHT_BOLD, NULL);
        g_object_set_data_full(G_OBJECT(tag), "bn-tag-name", g_strdup(name), g_free);
    } else if (g_strcmp0(name, "ITALIC") == 0) {
        tag = gtk_text_buffer_create_tag(buffer, name, "style", PANGO_STYLE_ITALIC, NULL);
        g_object_set_data_full(G_OBJECT(tag), "bn-tag-name", g_strdup(name), g_free);
    } else if (g_strcmp0(name, "UNDERLINE") == 0) {
        tag = gtk_text_buffer_create_tag(buffer, name, "underline", PANGO_UNDERLINE_SINGLE, NULL);
        g_object_set_data_full(G_OBJECT(tag), "bn-tag-name", g_strdup(name), g_free);
    } else if (g_str_has_prefix(name, "FG:#")) {
        const char *color = name + 4; // FG:#RRGGBB, color starts after FG:#
        char clr[16]; snprintf(clr, sizeof(clr), "#%s", color);
        tag = gtk_text_buffer_create_tag(buffer, name, "foreground", clr, NULL);
        g_object_set_data_full(G_OBJECT(tag), "bn-tag-name", g_strdup(name), g_free);
    } else if (g_str_has_prefix(name, "BG:#")) {
        const char *color = name + 4;
        char clr[16]; snprintf(clr, sizeof(clr), "#%s", color);
        tag = gtk_text_buffer_create_tag(buffer, name, "background", clr, NULL);
        g_object_set_data_full(G_OBJECT(tag), "bn-tag-name", g_strdup(name), g_free);
    } else {
        tag = gtk_text_buffer_create_tag(buffer, name, NULL);
        g_object_set_data_full(G_OBJECT(tag), "bn-tag-name", g_strdup(name), g_free);
    }
    return tag;
}

/* ui_toggle_tag_on_selection_generic removed (unused) */

static void editor_apply_tag_on_selection(EditorData *ed, const char *tagname, gboolean apply) {
    GtkTextIter s, e;
    GtkTextBuffer *buf = ed->buffer;
    if (!gtk_text_buffer_get_selection_bounds(buf, &s, &e)) return;
    GtkTextTag *tag = ui_get_or_create_tag(buf, tagname);
    if (apply) gtk_text_buffer_apply_tag(buf, tag, &s, &e);
    else gtk_text_buffer_remove_tag(buf, tag, &s, &e);
    editor_mark_dirty(ed);
}

static void on_bold_toggled(GtkToggleButton *btn, gpointer user_data) {
    EditorData *ed = (EditorData*)user_data;
    gboolean active = gtk_toggle_button_get_active(btn);
    /* If selection present, apply/remove to selection, but do not persist mode */
    GtkTextIter s,e; gboolean sel = FALSE;
    if (ed && ed->buffer) sel = gtk_text_buffer_get_selection_bounds(ed->buffer, &s, &e);
    if (sel) {
        if (ed && ed->debounce_timer) { g_source_remove(ed->debounce_timer); ed->debounce_timer = 0; }
        editor_push_snapshot(ed);
        editor_apply_tag_on_selection(ed, "BOLD", active);
        /* switch mode off after applying */
        ed->bold_mode = FALSE;
        g_signal_handlers_block_by_func(btn, G_CALLBACK(on_bold_toggled), ed);
        gtk_toggle_button_set_active(btn, FALSE);
        g_signal_handlers_unblock_by_func(btn, G_CALLBACK(on_bold_toggled), ed);
    } else {
        ed->bold_mode = active;
    }
    if (ed && ed->tview) gtk_widget_grab_focus(ed->tview);
}
static void on_italic_toggled(GtkToggleButton *btn, gpointer user_data) {
    EditorData *ed = (EditorData*)user_data;
    gboolean active = gtk_toggle_button_get_active(btn);
    GtkTextIter s,e; gboolean sel = FALSE;
    if (ed && ed->buffer) sel = gtk_text_buffer_get_selection_bounds(ed->buffer, &s, &e);
    if (sel) {
        if (ed && ed->debounce_timer) { g_source_remove(ed->debounce_timer); ed->debounce_timer = 0; }
        editor_push_snapshot(ed);
        editor_apply_tag_on_selection(ed, "ITALIC", active);
        ed->italic_mode = FALSE;
        g_signal_handlers_block_by_func(btn, G_CALLBACK(on_italic_toggled), ed);
        gtk_toggle_button_set_active(btn, FALSE);
        g_signal_handlers_unblock_by_func(btn, G_CALLBACK(on_italic_toggled), ed);
    } else {
        ed->italic_mode = active;
    }
    if (ed && ed->tview) gtk_widget_grab_focus(ed->tview);
}
static void on_underline_toggled(GtkToggleButton *btn, gpointer user_data) {
    EditorData *ed = (EditorData*)user_data;
    gboolean active = gtk_toggle_button_get_active(btn);
    GtkTextIter s,e; gboolean sel = FALSE;
    if (ed && ed->buffer) sel = gtk_text_buffer_get_selection_bounds(ed->buffer, &s, &e);
    if (sel) {
        if (ed && ed->debounce_timer) { g_source_remove(ed->debounce_timer); ed->debounce_timer = 0; }
        editor_push_snapshot(ed);
        editor_apply_tag_on_selection(ed, "UNDERLINE", active);
        ed->underline_mode = FALSE;
        g_signal_handlers_block_by_func(btn, G_CALLBACK(on_underline_toggled), ed);
        gtk_toggle_button_set_active(btn, FALSE);
        g_signal_handlers_unblock_by_func(btn, G_CALLBACK(on_underline_toggled), ed);
    } else {
        ed->underline_mode = active;
    }
    if (ed && ed->tview) gtk_widget_grab_focus(ed->tview);
}

/* ui_apply_color_on_selection removed (unused) */

static char *find_color_at_iter(GtkTextIter iter, const char *prefix) {
    for (int pass = 0; pass < 2; ++pass) {
        GSList *tags = gtk_text_iter_get_tags(&iter);
        char *found = NULL;
        for (GSList *item = tags; item; item = item->next) {
            GtkTextTag *tag = item->data;
            const char *name = g_object_get_data(G_OBJECT(tag), "bn-tag-name");
            if (name && g_str_has_prefix(name, prefix)) {
                found = g_strdup_printf("#%s", name + strlen(prefix));
                break;
            }
        }
        g_slist_free(tags);
        if (found) return found;
        if (pass == 0 && gtk_text_iter_get_offset(&iter) > 0)
            gtk_text_iter_backward_char(&iter);
        else
            break;
    }
    return NULL;
}

static void set_color_button_silently(GtkWidget *button, const char *color,
    GCallback callback, EditorData *ed, const char *fallback) {
    if (!button) return;
    GdkRGBA rgba;
    if (!color || !gdk_rgba_parse(&rgba, color))
        gdk_rgba_parse(&rgba, fallback);
    g_signal_handlers_block_by_func(button, callback, ed);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(button), &rgba);
    g_signal_handlers_unblock_by_func(button, callback, ed);
}

static void editor_update_palette(EditorData *ed) {
    if (!ed || !ed->buffer) return;
    GtkTextIter iter;
    GtkTextIter start, end;
    if (gtk_text_buffer_get_selection_bounds(ed->buffer, &start, &end)) {
        iter = start;
    } else {
        GtkTextMark *insert = gtk_text_buffer_get_insert(ed->buffer);
        gtk_text_buffer_get_iter_at_mark(ed->buffer, &iter, insert);
    }

    char *fg = find_color_at_iter(iter, "FG:#");
    char *bg = find_color_at_iter(iter, "BG:#");
    set_color_button_silently(ed->fg_btn, fg, G_CALLBACK(on_fg_color_set), ed, "#000000");
    set_color_button_silently(ed->bg_btn, bg, G_CALLBACK(on_bg_color_set), ed, "#ffffff");
    g_free(fg);
    g_free(bg);
}

static void on_buffer_mark_set(GtkTextBuffer *buffer, GtkTextIter *location,
    GtkTextMark *mark, gpointer user_data) {
    (void)buffer; (void)location; (void)mark;
    editor_update_palette((EditorData *)user_data);
}

static void on_fg_color_set(GtkColorButton *cbtn, gpointer user_data) {
    EditorData *ed = (EditorData*)user_data;
    GdkRGBA color; gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(cbtn), &color);
    char *hex = g_strdup_printf("#%02x%02x%02x", (int)(color.red*255), (int)(color.green*255), (int)(color.blue*255));
    /* apply to selection if present, otherwise set persistent color mode */
    if (ed && ed->buffer) {
        char name[64]; snprintf(name, sizeof(name), "FG:#%02x%02x%02x", (int)(color.red*255), (int)(color.green*255), (int)(color.blue*255));
        GtkTextIter s,e; if (gtk_text_buffer_get_selection_bounds(ed->buffer, &s, &e)) {
            if (ed && ed->debounce_timer) { g_source_remove(ed->debounce_timer); ed->debounce_timer = 0; }
            editor_push_snapshot(ed);
            GtkTextTag *tag = ui_get_or_create_tag(ed->buffer, name);
            gtk_text_buffer_apply_tag(ed->buffer, tag, &s, &e);
            editor_mark_dirty(ed);
            /* do not persist color if selection was applied */
        } else {
            g_free(ed->fg_color); ed->fg_color = g_strdup(hex);
        }
    }
    g_free(hex);
    if (ed && ed->tview) gtk_widget_grab_focus(ed->tview);
}
static void on_bg_color_set(GtkColorButton *cbtn, gpointer user_data) {
    EditorData *ed = (EditorData*)user_data;
    GdkRGBA color; gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(cbtn), &color);
    char *hex = g_strdup_printf("#%02x%02x%02x", (int)(color.red*255), (int)(color.green*255), (int)(color.blue*255));
    /* apply to selection if present */
    if (ed && ed->buffer) {
        char name[64]; snprintf(name, sizeof(name), "BG:#%02x%02x%02x", (int)(color.red*255), (int)(color.green*255), (int)(color.blue*255));
        GtkTextIter s,e; if (gtk_text_buffer_get_selection_bounds(ed->buffer, &s, &e)) {
            if (ed && ed->debounce_timer) { g_source_remove(ed->debounce_timer); ed->debounce_timer = 0; }
            editor_push_snapshot(ed);
            GtkTextTag *tag = ui_get_or_create_tag(ed->buffer, name);
            gtk_text_buffer_apply_tag(ed->buffer, tag, &s, &e);
            editor_mark_dirty(ed);
            /* do not persist color if selection was applied */
        } else {
            g_free(ed->bg_color); ed->bg_color = g_strdup(hex);
        }
    }
    g_free(hex);
    if (ed && ed->tview) gtk_widget_grab_focus(ed->tview);
}
static void on_copy_clicked(GtkButton *btn, gpointer user_data) { editor_copy_to_clipboard((EditorData*)user_data); }
static void on_paste_clicked(GtkButton *btn, gpointer user_data) { editor_paste_from_clipboard((EditorData*)user_data); }
static void on_undo_clicked(GtkButton *btn, gpointer user_data) { editor_undo((EditorData*)user_data); }
static void on_redo_clicked(GtkButton *btn, gpointer user_data) { editor_redo((EditorData*)user_data); }

static void on_clear_formatting_clicked(GtkButton *btn, gpointer user_data) {
    EditorData *ed = (EditorData*)user_data;
    if (!ed) return;
    if (ed->buffer) {
        GtkTextIter s, e;
        if (gtk_text_buffer_get_selection_bounds(ed->buffer, &s, &e)) {
            if (ed->debounce_timer) { g_source_remove(ed->debounce_timer); ed->debounce_timer = 0; }
            editor_push_snapshot(ed);
            gtk_text_buffer_remove_all_tags(ed->buffer, &s, &e);
            editor_mark_dirty(ed);
        } else {
            gtk_text_buffer_get_start_iter(ed->buffer, &s);
            gtk_text_buffer_get_end_iter(ed->buffer, &e);
            if (!gtk_text_iter_equal(&s, &e)) {
                if (ed->debounce_timer) { g_source_remove(ed->debounce_timer); ed->debounce_timer = 0; }
                editor_push_snapshot(ed);
                gtk_text_buffer_remove_all_tags(ed->buffer, &s, &e);
                editor_mark_dirty(ed);
            }
        }
    }
    /* Turn off all formatting modes */
    ed->bold_mode = FALSE;
    ed->italic_mode = FALSE;
    ed->underline_mode = FALSE;
    if (ed->fg_color) { g_free(ed->fg_color); ed->fg_color = NULL; }
    if (ed->bg_color) { g_free(ed->bg_color); ed->bg_color = NULL; }
    /* Update toggle button states */
    if (ed->bold_btn) {
        g_signal_handlers_block_by_func(ed->bold_btn, G_CALLBACK(on_bold_toggled), ed);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ed->bold_btn), FALSE);
        g_signal_handlers_unblock_by_func(ed->bold_btn, G_CALLBACK(on_bold_toggled), ed);
    }
    if (ed->italic_btn) {
        g_signal_handlers_block_by_func(ed->italic_btn, G_CALLBACK(on_italic_toggled), ed);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ed->italic_btn), FALSE);
        g_signal_handlers_unblock_by_func(ed->italic_btn, G_CALLBACK(on_italic_toggled), ed);
    }
    if (ed->underline_btn) {
        g_signal_handlers_block_by_func(ed->underline_btn, G_CALLBACK(on_underline_toggled), ed);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ed->underline_btn), FALSE);
        g_signal_handlers_unblock_by_func(ed->underline_btn, G_CALLBACK(on_underline_toggled), ed);
    }
    /* Return focus to editor */
    if (ed->tview) gtk_widget_grab_focus(ed->tview);
    editor_update_palette(ed);
}

static void open_url_from_selection(EditorData *ed) {
    if (!ed || !ed->buffer) return;
    GtkTextIter start, end;
    if (gtk_text_buffer_get_selection_bounds(ed->buffer, &start, &end)) {
        char *text = gtk_text_buffer_get_text(ed->buffer, &start, &end, FALSE);
        if (text) {
            // Check if it looks like a URL
            if (g_str_has_prefix(text, "http://") || g_str_has_prefix(text, "https://")) {
                GError *err = NULL;
                gtk_show_uri_on_window(GTK_WINDOW(ed->window), text, GDK_CURRENT_TIME, &err);
                if (err) {
                    g_printerr("Error opening URL: %s\n", err->message);
                    g_error_free(err);
                }
            }
            g_free(text);
        }
    }
}

static gboolean on_textview_keypress_rich(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    /* user_data is EditorData* */
    EditorData *ed = (EditorData*)user_data;
    if ((event->state & GDK_CONTROL_MASK) && (event->keyval == GDK_KEY_b || event->keyval == GDK_KEY_B)) {
        if (ed) {
            ed->bold_mode = !ed->bold_mode;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ed->bold_btn), ed->bold_mode);
        }
        return TRUE;
    }
    if ((event->state & GDK_CONTROL_MASK) && (event->keyval == GDK_KEY_i || event->keyval == GDK_KEY_I)) {
        if (ed) {
            ed->italic_mode = !ed->italic_mode;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ed->italic_btn), ed->italic_mode);
        }
        return TRUE;
    }
    /* if it's a normal key, schedule a snapshot (debounced) before insertion so undo reverts pre-insert state */
    if (!(event->state & GDK_CONTROL_MASK)) {
        gunichar u = gdk_keyval_to_unicode(event->keyval);
        if (u != 0 || event->keyval == GDK_KEY_BackSpace || event->keyval == GDK_KEY_Delete) {
            if (ed) editor_schedule_snapshot(ed);
        }
    }
    if ((event->state & GDK_CONTROL_MASK) && (event->keyval == GDK_KEY_z || event->keyval == GDK_KEY_Z)) {
        if (ed) editor_undo(ed);
        /* Clear any scheduled pending snapshot; undo uses explicit snapshots */
        if (ed && ed->debounce_timer) { g_source_remove(ed->debounce_timer); ed->debounce_timer = 0; }
        return TRUE;
    }
    if ((event->state & GDK_CONTROL_MASK) && (event->keyval == GDK_KEY_y || event->keyval == GDK_KEY_Y)) {
        if (ed) editor_redo(ed);
        if (ed && ed->debounce_timer) { g_source_remove(ed->debounce_timer); ed->debounce_timer = 0; }
        return TRUE;
    }
    if ((event->state & GDK_CONTROL_MASK) && (event->keyval == GDK_KEY_e || event->keyval == GDK_KEY_E)) {
        if (ed) open_url_from_selection(ed);
        return TRUE;
    }
    return FALSE;
}

/* insert-text handler: schedule applying tags to newly inserted text */
typedef struct {
    EditorData *ed;
    int start;
    int len;
    guint source_id;
} InsertCtx;

static gboolean apply_tags_to_range_idle(gpointer user_data) {
    InsertCtx *ctx = (InsertCtx*)user_data;
    if (!ctx) return G_SOURCE_REMOVE;
    EditorData *ed = ctx->ed;
    if (ed) ed->insert_idle_ids = g_list_remove(ed->insert_idle_ids,
        GUINT_TO_POINTER(ctx->source_id));
    if (!ed || ed->destroying || !ed->buffer) { g_free(ctx); return G_SOURCE_REMOVE; }
    GtkTextIter s, e;
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &s, ctx->start);
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &e, ctx->start + ctx->len);
    /* apply tags according to editor flags */
    if (ed->bold_mode) { GtkTextTag *tag = ui_get_or_create_tag(ed->buffer, "BOLD"); gtk_text_buffer_apply_tag(ed->buffer, tag, &s, &e); }
    if (ed->italic_mode) { GtkTextTag *tag = ui_get_or_create_tag(ed->buffer, "ITALIC"); gtk_text_buffer_apply_tag(ed->buffer, tag, &s, &e); }
    if (ed->underline_mode) { GtkTextTag *tag = ui_get_or_create_tag(ed->buffer, "UNDERLINE"); gtk_text_buffer_apply_tag(ed->buffer, tag, &s, &e); }
    if (ed->fg_color) { char name[64]; snprintf(name, sizeof(name), "FG:%s", ed->fg_color); GtkTextTag *tag = ui_get_or_create_tag(ed->buffer, name); gtk_text_buffer_apply_tag(ed->buffer, tag, &s, &e); }
    if (ed->bg_color) { char name[64]; snprintf(name, sizeof(name), "BG:%s", ed->bg_color); GtkTextTag *tag = ui_get_or_create_tag(ed->buffer, name); gtk_text_buffer_apply_tag(ed->buffer, tag, &s, &e); }
    g_free(ctx);
    return FALSE;
}

static void on_insert_text(GtkTextBuffer *buffer, GtkTextIter *location, gchar *text, gint len, gpointer user_data) {
    EditorData *ed = (EditorData*)user_data;
    if (!ed) return;
    /* If no modes active, nothing to do */
    if (!ed->bold_mode && !ed->italic_mode && !ed->underline_mode && !ed->fg_color && !ed->bg_color) return;
    int start = gtk_text_iter_get_offset(location);
    InsertCtx *ctx = g_new0(InsertCtx, 1);
    ctx->ed = ed;
    ctx->start = start;
    ctx->len = len;
    /* Schedule to run after insertion is done (in main loop) */
    ctx->source_id = g_idle_add(apply_tags_to_range_idle, ctx);
    ed->insert_idle_ids = g_list_prepend(ed->insert_idle_ids,
        GUINT_TO_POINTER(ctx->source_id));
}

static void on_buffer_changed(GtkTextBuffer *buffer, gpointer user_data) {
    EditorData *ed = (EditorData*)user_data;
    if (!ed) return;
    ed->dirty = TRUE;
    editor_schedule_autosave(ed);
}

static GtkWidget *create_rich_toolbar_for_editor(EditorData *ed) {
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    
    /* Bold toggle with icon */
    GtkWidget *tb_b = gtk_toggle_button_new();
    GtkWidget *bold_img = gtk_image_new_from_icon_name("format-text-bold", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(tb_b), bold_img);
    gtk_widget_set_tooltip_text(tb_b, "Bold (Ctrl+B)");
    g_signal_connect(tb_b, "toggled", G_CALLBACK(on_bold_toggled), ed);
    gtk_box_pack_start(GTK_BOX(toolbar), tb_b, FALSE, FALSE, 0);
    ed->bold_btn = tb_b;
    
    /* Italic toggle with icon */
    GtkWidget *tb_i = gtk_toggle_button_new();
    GtkWidget *italic_img = gtk_image_new_from_icon_name("format-text-italic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(tb_i), italic_img);
    gtk_widget_set_tooltip_text(tb_i, "Italic (Ctrl+I)");
    g_signal_connect(tb_i, "toggled", G_CALLBACK(on_italic_toggled), ed);
    gtk_box_pack_start(GTK_BOX(toolbar), tb_i, FALSE, FALSE, 0);
    ed->italic_btn = tb_i;
    
    /* Underline toggle with icon */
    GtkWidget *tb_u = gtk_toggle_button_new();
    GtkWidget *underline_img = gtk_image_new_from_icon_name("format-text-underline", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(tb_u), underline_img);
    gtk_widget_set_tooltip_text(tb_u, "Underline");
    g_signal_connect(tb_u, "toggled", G_CALLBACK(on_underline_toggled), ed);
    gtk_box_pack_start(GTK_BOX(toolbar), tb_u, FALSE, FALSE, 0);
    ed->underline_btn = tb_u;
    
    /* Color buttons */
    GtkWidget *tb_fg = gtk_color_button_new();
    gtk_widget_set_tooltip_text(tb_fg, "Text color");
    g_signal_connect(tb_fg, "color-set", G_CALLBACK(on_fg_color_set), ed);
    gtk_box_pack_start(GTK_BOX(toolbar), tb_fg, FALSE, FALSE, 0);
    ed->fg_btn = tb_fg;
    
    GtkWidget *tb_bg = gtk_color_button_new();
    gtk_widget_set_tooltip_text(tb_bg, "Background color");
    g_signal_connect(tb_bg, "color-set", G_CALLBACK(on_bg_color_set), ed);
    gtk_box_pack_start(GTK_BOX(toolbar), tb_bg, FALSE, FALSE, 0);
    ed->bg_btn = tb_bg;
    
    /* Clear formatting button */
    GtkWidget *clear_fmt_btn = gtk_button_new_from_icon_name("edit-clear", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(clear_fmt_btn, "Clear formatting (reset to plain text)");
    g_signal_connect(clear_fmt_btn, "clicked", G_CALLBACK(on_clear_formatting_clicked), ed);
    gtk_box_pack_start(GTK_BOX(toolbar), clear_fmt_btn, FALSE, FALSE, 0);
    
    /* Separator */
    GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(toolbar), sep1, FALSE, FALSE, 2);
    
    /* Clipboard buttons with icons */
    GtkWidget *copy_btn = gtk_button_new_from_icon_name("edit-copy", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(copy_btn, "Copy selection (internal clipboard)");
    gtk_box_pack_start(GTK_BOX(toolbar), copy_btn, FALSE, FALSE, 0);
    
    GtkWidget *paste_btn = gtk_button_new_from_icon_name("edit-paste", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(paste_btn, "Paste from internal clipboard");
    gtk_box_pack_start(GTK_BOX(toolbar), paste_btn, FALSE, FALSE, 0);
    
    /* Separator */
    GtkWidget *sep2 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(toolbar), sep2, FALSE, FALSE, 2);
    
    /* Undo/Redo buttons with icons */
    GtkWidget *undo_btn = gtk_button_new_from_icon_name("edit-undo", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(undo_btn, "Undo (Ctrl+Z)");
    gtk_box_pack_start(GTK_BOX(toolbar), undo_btn, FALSE, FALSE, 0);
    
    GtkWidget *redo_btn = gtk_button_new_from_icon_name("edit-redo", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(redo_btn, "Redo (Ctrl+Y)");
    gtk_box_pack_start(GTK_BOX(toolbar), redo_btn, FALSE, FALSE, 0);
    
    /* Set initial toggle state from EditorData */
    if (ed) {
        if (ed->bold_mode && ed->bold_btn) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ed->bold_btn), TRUE);
        if (ed->italic_mode && ed->italic_btn) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ed->italic_btn), TRUE);
        if (ed->underline_mode && ed->underline_btn) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ed->underline_btn), TRUE);
    }
    
    /* Connect button signals */
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_copy_clicked), ed);
    g_signal_connect(paste_btn, "clicked", G_CALLBACK(on_paste_clicked), ed);
    g_signal_connect(undo_btn, "clicked", G_CALLBACK(on_undo_clicked), ed);
    g_signal_connect(redo_btn, "clicked", G_CALLBACK(on_redo_clicked), ed);
    
    return toolbar;
}

// Click handler for wrap label: double-click toggles wrap setting
static gboolean on_wrap_label_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    (void)widget; (void)user_data;
    if (event->type == GDK_2BUTTON_PRESS && event->button == 1) {
        app_toggle_word_wrap();
        return TRUE;
    }
    return FALSE;
}



// Helper: select the last saved note, or first if not found/empty
static void select_last_or_first_note(void) {
    if (!tree || !notes_store) return;
    
    GtkTreeModel *model = GTK_TREE_MODEL(notes_store);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    GtkTreePath *found_path = NULL;
    
    if (last_selected_note && valid) {
        do {
            char *title = NULL;
            gint row_kind = ROW_NOTE;
            gtk_tree_model_get(model, &iter, 2, &title, 3, &row_kind, -1);
            if (title && row_kind == ROW_NOTE) {
                if (g_strcmp0(title, last_selected_note) == 0) {
                    found_path = gtk_tree_model_get_path(model, &iter);
                    g_free(title);
                    break;
                }
                g_free(title);
            }
        } while (gtk_tree_model_iter_next(model, &iter));
    }
    
    if (!found_path) {
        // Fallback to the first note, keeping folder rows unselected.
        gint row_kind = ROW_FOLDER;
        if (gtk_tree_model_get_iter_first(model, &iter)) {
            do {
                gtk_tree_model_get(model, &iter, 3, &row_kind, -1);
                if (row_kind == ROW_NOTE) {
                    found_path = gtk_tree_model_get_path(model, &iter);
                    break;
                }
            } while (gtk_tree_model_iter_next(model, &iter));
        }
    }
    
    if (found_path) {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
        
        // Block the selection-changed signal to prevent recursive updates
        g_signal_handlers_block_by_func(sel, G_CALLBACK(on_selection_changed), NULL);
        
        gtk_tree_selection_select_path(sel, found_path);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(tree), found_path, NULL, FALSE);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(tree), found_path, NULL, TRUE, 0.5, 0.0);
        
        // Unblock the signal
        g_signal_handlers_unblock_by_func(sel, G_CALLBACK(on_selection_changed), NULL);
        
        gtk_tree_path_free(found_path);
        
        // Give focus to the tree view
        gtk_widget_grab_focus(tree);
    }
}

// Helper to show main window, position it, and ensure it is focused/on-top
static void show_main_window(void) {
    if (!main_window) return;
    if (!gtk_widget_get_visible(main_window)) {
        int x = -1, y = -1;
        if (app_read_window_position(&x, &y) && x >= 0 && y >= 0) {
            gtk_window_move(GTK_WINDOW(main_window), x, y);
        }
        gtk_widget_show_all(main_window);
    }
    
    /* Refresh notes list when showing window */
    if (notes_store && search_entry) {
        reload_notes_safely(gtk_entry_get_text(GTK_ENTRY(search_entry)));
    }
    
    /* Select last note row when window is shown */
    select_last_or_first_note();
    
    gtk_window_present(GTK_WINDOW(main_window));
    gtk_window_set_keep_above(GTK_WINDOW(main_window), TRUE);
    gtk_window_set_focus_on_map(GTK_WINDOW(main_window), TRUE);
    gtk_widget_grab_focus(main_window);
}

// Called in main thread via g_idle_add to bring window to front
static gboolean bring_main_window(gpointer user_data) {
    show_main_window();
    return FALSE; // run once
}

// Server thread: accept simple SHOW commands and schedule bringing main window to front
static void *instance_server_thread(void *arg) {
    (void)arg;
    while (server_sock != -1) {
        int c = accept(server_sock, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            break;
        }
        char buf[16] = {0};
        ssize_t r = read(c, buf, sizeof(buf)-1);
        close(c);
        if (r > 0) {
            if (strncmp(buf, "SHOW", 4) == 0) {
                g_idle_add(bring_main_window, NULL);
            }
        }
    }
    return NULL;
}

static void on_show_hide(GtkMenuItem *item, gpointer user_data) {
    if (gtk_widget_get_visible(main_window)) {
        // Save position before hiding
        if (current_x != -1 && current_y != -1) {
            app_save_window_position(current_x, current_y);
        } else {
            int x, y;
            gtk_window_get_position(GTK_WINDOW(main_window), &x, &y);
            app_save_window_position(x, y);
        }
        gtk_widget_hide(main_window);
    } else {
        /* app_load_notes will be called by focus-in-event handler */
        show_main_window();
    }
}

static void on_quit(GtkMenuItem *item, gpointer user_data) {
    gtk_main_quit();
}

static void on_search_changed(GtkEntry *entry, gpointer user_data) {
    (void)entry; (void)user_data;
    if (search_reload_timer) {
        g_source_remove(search_reload_timer);
        search_reload_timer = 0;
    }
    search_reload_timer = g_timeout_add(180, reload_search_cb, NULL);
}

static gboolean reload_search_cb(gpointer user_data) {
    (void)user_data;
    search_reload_timer = 0;
    if (search_entry)
        reload_notes_safely(gtk_entry_get_text(GTK_ENTRY(search_entry)));
    return G_SOURCE_REMOVE;
}

static void reload_folders(void) {
    if (!folder_combo) return;
    g_signal_handlers_block_by_func(folder_combo, G_CALLBACK(on_folder_changed), NULL);
    app_load_folders(GTK_COMBO_BOX_TEXT(folder_combo));
    if (current_folder && *current_folder &&
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(folder_combo), current_folder)) {
        g_signal_handlers_unblock_by_func(folder_combo, G_CALLBACK(on_folder_changed), NULL);
        return;
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(folder_combo), "__root__");
    g_signal_handlers_unblock_by_func(folder_combo, G_CALLBACK(on_folder_changed), NULL);
}

static void navigate_to_folder(const char *folder) {
    if (!folder_combo) return;
    if (folder && *folder)
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(folder_combo), folder);
    else
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(folder_combo), "__root__");
}

static void on_folder_changed(GtkComboBox *combo, gpointer user_data) {
    (void)user_data;
    if (search_reload_timer) {
        g_source_remove(search_reload_timer);
        search_reload_timer = 0;
    }
    const char *id = gtk_combo_box_get_active_id(combo);
    if (!id) return;
    char *next_folder = g_strcmp0(id, "__root__") == 0 ? g_strdup("") : g_strdup(id);
    if (g_strcmp0(next_folder, current_folder) == 0) {
        g_free(next_folder);
        return;
    }
    g_free(current_folder);
    current_folder = next_folder;
    if (last_selected_note) {
        g_free(last_selected_note);
        last_selected_note = NULL;
    }
    reload_notes_safely(search_entry ? gtk_entry_get_text(GTK_ENTRY(search_entry)) : "");
    select_last_or_first_note();
}

static void on_new_folder_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "New folder", GTK_WINDOW(main_window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Create", GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL, NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Folder name");
    gtk_widget_set_margin_start(area, 14);
    gtk_widget_set_margin_end(area, 14);
    gtk_widget_set_margin_top(area, 12);
    gtk_widget_set_margin_bottom(area, 12);
    gtk_box_pack_start(GTK_BOX(area), entry, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    const char *name = gtk_entry_get_text(GTK_ENTRY(entry));
    char *new_folder = NULL;
    if (response == GTK_RESPONSE_OK && name && *name &&
        app_create_folder(current_folder, name)) {
        new_folder = (current_folder && *current_folder)
            ? g_build_filename(current_folder, name, NULL) : g_strdup(name);
    }
    gtk_widget_destroy(dialog);
    if (new_folder) {
        reload_folders();
        reload_notes_safely(search_entry ? gtk_entry_get_text(GTK_ENTRY(search_entry)) : "");
        select_last_or_first_note();
        g_free(new_folder);
    } else if (response == GTK_RESPONSE_OK) {
        GtkWidget *error = gtk_message_dialog_new(GTK_WINDOW(main_window),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK, "Could not create folder. Check the name or whether it already exists.");
        gtk_dialog_run(GTK_DIALOG(error));
        gtk_widget_destroy(error);
    }
}

static void refresh_folder_view(void) {
    reload_folders();
    reload_notes_safely(search_entry ? gtk_entry_get_text(GTK_ENTRY(search_entry)) : "");
    select_last_or_first_note();
}

static void on_rename_folder_activate(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *menu = GTK_WIDGET(user_data);
    const char *stored_folder = g_object_get_data(G_OBJECT(menu), "folder-path");
    char *folder = g_strdup(stored_folder ? stored_folder : "");
    char *old_name = g_path_get_basename(folder);

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Rename folder", GTK_WINDOW(main_window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Rename", GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL, NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), old_name);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
    gtk_widget_set_margin_start(area, 14);
    gtk_widget_set_margin_end(area, 14);
    gtk_widget_set_margin_top(area, 12);
    gtk_widget_set_margin_bottom(area, 12);
    gtk_box_pack_start(GTK_BOX(area), entry, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    char *new_name = NULL;
    if (response == GTK_RESPONSE_OK)
        new_name = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
    gtk_widget_destroy(dialog);

    if (new_name && !app_rename_folder(folder, new_name)) {
        GtkWidget *error = gtk_message_dialog_new(GTK_WINDOW(main_window),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK, "Could not rename the folder. The name may already exist or be invalid.");
        gtk_dialog_run(GTK_DIALOG(error));
        gtk_widget_destroy(error);
    } else if (new_name) {
        refresh_folder_view();
    }
    g_free(new_name);
    g_free(old_name);
    g_free(folder);
    gtk_widget_destroy(menu);
}

static void on_delete_folder_activate(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *menu = GTK_WIDGET(user_data);
    const char *stored_folder = g_object_get_data(G_OBJECT(menu), "folder-path");
    char *folder = g_strdup(stored_folder ? stored_folder : "");
    char *name = g_path_get_basename(folder);
    int empty = app_folder_is_empty(folder);

    if (empty < 0) {
        GtkWidget *error = gtk_message_dialog_new(GTK_WINDOW(main_window),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK, "Could not inspect the folder.");
        gtk_dialog_run(GTK_DIALOG(error));
        gtk_widget_destroy(error);
    } else {
        GtkWidget *dialog;
        if (empty) {
            dialog = gtk_message_dialog_new(GTK_WINDOW(main_window),
                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
                GTK_BUTTONS_NONE, "Delete empty folder '%s'?", name);
        } else {
            dialog = gtk_message_dialog_new(GTK_WINDOW(main_window),
                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_WARNING,
                GTK_BUTTONS_NONE,
                "Folder '%s' is not empty. Deleting it will permanently delete all notes and subfolders inside it. Continue?",
                name);
        }
        gtk_dialog_add_button(GTK_DIALOG(dialog), "Delete", GTK_RESPONSE_YES);
        gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_NO);
        int response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response == GTK_RESPONSE_YES && !app_delete_folder(folder)) {
            GtkWidget *error = gtk_message_dialog_new(GTK_WINDOW(main_window),
                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
                GTK_BUTTONS_OK, "Could not delete the folder.");
            gtk_dialog_run(GTK_DIALOG(error));
            gtk_widget_destroy(error);
        } else if (response == GTK_RESPONSE_YES) {
            refresh_folder_view();
        }
    }
    g_free(name);
    g_free(folder);
    gtk_widget_destroy(menu);
}

static void on_clear_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    if (search_reload_timer) {
        g_source_remove(search_reload_timer);
        search_reload_timer = 0;
    }
    gtk_entry_set_text(GTK_ENTRY(search_entry), "");
    reload_notes_safely("");
    select_last_or_first_note();
}

// Helper: find the first non-empty line (trim) in text, returns a newly allocated string
static char *first_nonempty_line(const char *text) {
    if (!text) return NULL;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        size_t start = 0, end = len;
        while (start < len && g_ascii_isspace((gchar)p[start])) start++;
        while (end > start && g_ascii_isspace((gchar)p[end-1])) end--;
        if (end > start) {
            return g_strndup(p + start, end - start);
        }
        if (!e) break;
        p = e + 1;
    }
    return NULL;
}

/* 
 * Create a shared editor dialog window for both add and edit operations.
 * Returns the dialog window. EditorData is attached to window as "editor-data".
 */
static GtkWidget* create_editor_dialog(const char *window_title, const char *note_title, GtkTextBuffer *existing_buffer) {
    GtkWidget *dlg = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(dlg), window_title);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 600, 400);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(main_window));
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER_ON_PARENT);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(dlg), vbox);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
    GtkWidget *tview = gtk_text_view_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(tview), "editor-text");
    gtk_container_add(GTK_CONTAINER(scroll), tview);
    
    GtkWidget *wrap_label = gtk_label_new(app_get_word_wrap() ? "Wrap: ON" : "Wrap: OFF");
    gtk_widget_set_margin_end(wrap_label, 5);
    GtkWidget *wrap_eventbox = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(wrap_eventbox), FALSE);
    gtk_container_add(GTK_CONTAINER(wrap_eventbox), wrap_label);
    gtk_widget_add_events(wrap_eventbox, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(wrap_eventbox, "button-press-event", G_CALLBACK(on_wrap_label_button_press), NULL);
    app_register_textview(tview, wrap_label);
    
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tview), app_get_word_wrap() ? GTK_WRAP_WORD : GTK_WRAP_NONE);
    gtk_widget_add_events(tview, GDK_KEY_PRESS_MASK);
    g_signal_connect(tview, "key-press-event", G_CALLBACK(app_textview_keypress), NULL);

    GtkTextBuffer *buffer = existing_buffer ? existing_buffer : gtk_text_view_get_buffer(GTK_TEXT_VIEW(tview));
    if (existing_buffer) {
        gtk_text_view_set_buffer(GTK_TEXT_VIEW(tview), existing_buffer);
    }
    
    EditorData *ed = g_new0(EditorData, 1);
    if (note_title) {
        gchar *dirname = g_path_get_dirname(note_title);
        ed->folder = (g_strcmp0(dirname, ".") == 0) ? g_strdup("") : g_strdup(dirname);
        g_free(dirname);
    } else {
        ed->folder = g_strdup(current_folder ? current_folder : "");
    }
    ed->is_new_note = note_title == NULL;
    ed->title = note_title ? g_strdup(note_title) : make_temporary_note_title(ed->folder);
    ed->buffer = buffer;
    ed->window = dlg;
    ed->tview = tview;
    ed->bold_mode = ed->italic_mode = ed->underline_mode = FALSE;
    ed->fg_color = NULL; ed->bg_color = NULL;
    ed->undo_stack = NULL; ed->redo_stack = NULL; ed->max_history = 128; ed->ignore_changes = FALSE;
    ed->debounce_timer = 0;
    ed->autosave_timer = 0;
    ed->dirty = FALSE;
    g_object_set_data(G_OBJECT(dlg), "editor-data", ed);
    if (ed->is_new_note)
        app_write_note(ed->title, "");
    editor_push_snapshot(ed);

    GtkWidget *toolbar = create_rich_toolbar_for_editor(ed);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);
    g_signal_connect(ed->buffer, "mark-set", G_CALLBACK(on_buffer_mark_set), ed);
    editor_update_palette(ed);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    
    GtkWidget *spacer = gtk_label_new(NULL);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), spacer, TRUE, TRUE, 0);
    gtk_widget_set_halign(wrap_eventbox, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(hbox), wrap_eventbox, FALSE, FALSE, 0);

    g_signal_connect(tview, "key-press-event", G_CALLBACK(on_textview_keypress_rich), ed);
    g_signal_connect(dlg, "destroy", G_CALLBACK(editor_destroy), ed);
    g_signal_connect(ed->buffer, "insert-text", G_CALLBACK(on_insert_text), ed);
    g_signal_connect(ed->buffer, "changed", G_CALLBACK(on_buffer_changed), ed);

    return dlg;
}


// Handler: '+' button for new note
static void on_add_clicked(GtkButton *btn, gpointer user_data) {
    /* Create editor dialog for new note (no title, empty buffer) */
    GtkWidget *dlg = create_editor_dialog("New note", NULL, NULL);

    gtk_widget_show_all(dlg);
}

static gboolean on_configure_event(GtkWidget *widget, GdkEventConfigure *event, gpointer user_data) {
    if (gtk_widget_get_visible(widget)) {
        gtk_window_get_position(GTK_WINDOW(widget), &current_x, &current_y);
    }
    return FALSE;
}

static gboolean on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    if (current_x != -1 && current_y != -1) {
        app_save_window_position(current_x, current_y);
    } else {
        int x, y;
        gtk_window_get_position(GTK_WINDOW(widget), &x, &y);
        app_save_window_position(x, y);
    }
    gtk_widget_hide(widget);
    return TRUE; // prepreči uničenje
}

static gboolean on_window_focus_in(GtkWidget *widget, GdkEventFocus *event, gpointer user_data) {
    /* Just restore the last selected note, don't reload the list */
    /* (reloading would re-sort and change positions) */
    if (notes_store && tree) {
        select_last_or_first_note();
    }
    return FALSE; /* propagate event */
}

static GtkWidget* create_main_window(void) {
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "baNotes");
    gtk_window_set_default_size(GTK_WINDOW(win), 500, 600);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_NONE);
    g_signal_connect(win, "configure-event", G_CALLBACK(on_configure_event), NULL);
    g_signal_connect(win, "delete-event", G_CALLBACK(on_window_delete), NULL);
    g_signal_connect(win, "focus-in-event", G_CALLBACK(on_window_focus_in), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(win), vbox);
    gtk_widget_set_name(vbox, "main-root");
    gtk_widget_set_margin_start(vbox, 14);
    gtk_widget_set_margin_end(vbox, 14);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(hbox), "topbar");
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    folder_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(folder_combo, 150, -1);
    gtk_widget_set_tooltip_text(folder_combo, "Current folder");
    g_signal_connect(folder_combo, "changed", G_CALLBACK(on_folder_changed), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), folder_combo, FALSE, FALSE, 0);

    GtkWidget *new_folder_btn = gtk_button_new_from_icon_name("folder-new", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(new_folder_btn, "New folder");
    gtk_style_context_add_class(gtk_widget_get_style_context(new_folder_btn), "btn-flat");
    g_signal_connect(new_folder_btn, "clicked", G_CALLBACK(on_new_folder_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), new_folder_btn, FALSE, FALSE, 0);

    search_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), "Search notes...");
    gtk_style_context_add_class(gtk_widget_get_style_context(search_entry), "search-entry");
    g_signal_connect(search_entry, "changed", G_CALLBACK(on_search_changed), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), search_entry, TRUE, TRUE, 0);

    GtkWidget *add_btn = gtk_button_new_from_icon_name("list-add", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(add_btn, "New note");
    gtk_style_context_add_class(gtk_widget_get_style_context(add_btn), "btn-accent");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(hbox), add_btn, FALSE, FALSE, 0);

    clear_btn = gtk_button_new_from_icon_name("edit-clear-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(clear_btn, "Clear search");
    gtk_style_context_add_class(gtk_widget_get_style_context(clear_btn), "btn-flat");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(hbox), clear_btn, FALSE, FALSE, 0);

    // Model: 0=display title, 1=folder/trash icon, 2=relative path, 3=row kind
    notes_store = gtk_list_store_new(4, G_TYPE_STRING, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_INT);
    tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(notes_store));
    gtk_style_context_add_class(gtk_widget_get_style_context(tree), "notes-tree");
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), FALSE);
    gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(tree), FALSE);
    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(tree), FALSE);
    gtk_tree_view_set_reorderable(GTK_TREE_VIEW(tree), FALSE);
    gtk_widget_add_events(tree, GDK_KEY_PRESS_MASK);
    g_signal_connect(tree, "key-press-event", G_CALLBACK(on_tree_key_press), NULL);

    GtkTargetEntry drag_targets[] = {
        { "text/plain", GTK_TARGET_SAME_APP, 0 }
    };
    gtk_tree_view_enable_model_drag_source(GTK_TREE_VIEW(tree), GDK_BUTTON1_MASK,
        drag_targets, 1, GDK_ACTION_MOVE);
    gtk_tree_view_enable_model_drag_dest(GTK_TREE_VIEW(tree), drag_targets, 1,
        GDK_ACTION_MOVE);
    g_signal_connect(tree, "drag-data-get", G_CALLBACK(on_drag_data_get), NULL);
    g_signal_connect(tree, "drag-end", G_CALLBACK(on_drag_end), NULL);
    g_signal_connect(tree, "drag-data-received", G_CALLBACK(on_drag_data_received), NULL);

    // Title (expandable)
    GtkCellRenderer *renderer_text = gtk_cell_renderer_text_new();
    g_object_set(renderer_text, "ypad", 8, "xpad", 6, "size-points", 12.0, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes("Note title", renderer_text, "text", 0, NULL);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_column_set_cell_data_func(col, renderer_text,
        folder_text_cell_data_func, NULL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

    // Trash icon (fixed width, right)
    GtkCellRenderer *renderer_trash = gtk_cell_renderer_pixbuf_new();
    /* Add horizontal padding so the icon is not placed flush against the column
     * right edge; this helps avoid overlaying with the vertical scrollbar. */
    gtk_cell_renderer_set_padding(renderer_trash, 8, 2);
    GtkTreeViewColumn *col_trash = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col_trash, "");
    gtk_tree_view_column_pack_start(col_trash, renderer_trash, FALSE);
    gtk_tree_view_column_add_attribute(col_trash, renderer_trash, "pixbuf", 1);
    gtk_tree_view_column_set_sizing(col_trash, GTK_TREE_VIEW_COLUMN_FIXED);
    /* Fixed width: original 40px + additional space to fit padding and avoid
     * overlap with overlay scrollbars. */
    gtk_tree_view_column_set_fixed_width(col_trash, 56);
    gtk_tree_view_column_set_alignment(col_trash, 1.0);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col_trash);

    /* Keep a small fixed inset on the far right.  This prevents the delete
     * icon from sitting underneath an overlay scrollbar or the tree border
     * when the window is resized, while the title column remains expandable. */
    GtkTreeViewColumn *col_right_inset = gtk_tree_view_column_new();
    GtkCellRenderer *renderer_right_inset = gtk_cell_renderer_text_new();
    g_object_set(renderer_right_inset, "text", "", "xpad", 0, "ypad", 0, NULL);
    gtk_tree_view_column_pack_start(col_right_inset, renderer_right_inset, FALSE);
    gtk_tree_view_column_set_sizing(col_right_inset, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(col_right_inset, 12);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col_right_inset);

    // Connect click on cell (only for trash) and double-click (row-activated) for editing
    g_signal_connect(tree, "button-press-event", G_CALLBACK(on_tree_button_press), NULL);
    g_signal_connect(tree, "row-activated", G_CALLBACK(on_row_activated), NULL);

    // Connect selection changed to save last note immediately
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
    g_signal_connect(sel, "changed", G_CALLBACK(on_selection_changed), NULL);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(scroll), "notes-scroll");
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_NONE);
    gtk_container_add(GTK_CONTAINER(scroll), tree);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    /* Make sure first row is selected when the main window appears */
    if (!current_folder) current_folder = g_strdup("");
    reload_folders();
    reload_notes_safely("");
    select_last_or_first_note();

    return win;
}

static void apply_app_css(void) {
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "window { background-color: @theme_bg_color; }\n"
        "#main-root { border-radius: 14px; background-color: alpha(@theme_base_color, 0.55); }\n"
        ".topbar button { min-height: 34px; min-width: 34px; border-radius: 10px; }\n"
        ".search-entry { min-height: 36px; border-radius: 10px; padding: 0 10px; }\n"
        ".search-entry:focus { border-color: @theme_selected_bg_color; box-shadow: 0 0 0 2px alpha(@theme_selected_bg_color, 0.25); }\n"
        ".editor-text { font-size: 12pt; }\n"
        ".btn-accent { background-color: alpha(@theme_selected_bg_color, 0.9); color: @theme_selected_fg_color; }\n"
        ".btn-accent:hover { background-color: @theme_selected_bg_color; }\n"
        ".btn-flat { background-color: alpha(@theme_fg_color, 0.08); }\n"
        ".btn-flat:hover { background-color: alpha(@theme_fg_color, 0.14); }\n"
        ".notes-scroll { border-radius: 12px; border: 1px solid alpha(@theme_fg_color, 0.16); background-color: @theme_base_color; }\n"
        ".notes-tree { background: transparent; }\n"
        ".notes-tree:selected, .notes-tree row:selected { background: alpha(@theme_selected_bg_color, 0.35); color: @theme_text_color; }\n"
        ".notes-tree row:hover { background: alpha(@theme_selected_bg_color, 0.14); }\n",
        -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static void create_tray_icon(void) {
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *show_item = gtk_menu_item_new_with_label("Show/Hide");
    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
    GtkWidget *ver_item = gtk_menu_item_new_with_label(VERZIJA);
    GtkWidget *sep1 = gtk_separator_menu_item_new();
    GtkWidget *sep2 = gtk_separator_menu_item_new();
    g_signal_connect(show_item, "activate", G_CALLBACK(on_show_hide), NULL);
    g_signal_connect(quit_item, "activate", G_CALLBACK(on_quit), NULL);
    /* Version entry first (non-interactive) */
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), ver_item);
    gtk_widget_set_sensitive(ver_item, FALSE);
    /* separator under version */
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep1);
    /* then normal items */
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), show_item);
    /* separator before Quit */
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep2);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);
    gtk_widget_show_all(menu);

    indicator = app_indicator_new(
        "baNotes-indicator",
        "document-open",
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS
    );
    /* Prefer the themed icon name `baNotes` (installed via hicolor theme). */
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    gboolean have_theme_icon = FALSE;
    if (theme) have_theme_icon = gtk_icon_theme_has_icon(theme, "baNotes");

    if (have_theme_icon) {
        /* use icon name from the theme */
        app_indicator_set_icon_full(indicator, "baNotes", "baNotes");
    } else if (g_file_test("/usr/share/icons/hicolor/scalable/apps/baNotes.svg", G_FILE_TEST_EXISTS)) {
        /* fallback to system scalable icon */
        app_indicator_set_icon_full(indicator, "/usr/share/icons/hicolor/scalable/apps/baNotes.svg", "baNotes");
    } else if (g_file_test("baNotes.svg", G_FILE_TEST_EXISTS)) {
        /* developer repo-local icon (SVG) */
        gchar *cwd_path = g_canonicalize_filename("baNotes.svg", NULL);
        app_indicator_set_icon_full(indicator, cwd_path, "baNotes");
        g_free(cwd_path);
    } else {
        app_indicator_set_icon_full(indicator, "document-open", "baNotes");
    }
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_menu(indicator, GTK_MENU(menu));
}

int main(int argc, char *argv[]) {
    // Ensure config dir exists and prepare socket path
    app_init_config_dirs();
    // Initialize word-wrap state from saved settings (default enabled)
    int _wrap_state = 1;
    if (app_read_word_wrap(&_wrap_state)) {
        // loaded into internal state by app_read_word_wrap
    } else {
        app_save_word_wrap(_wrap_state);
    }
    const char *home = getenv("HOME");
    if (home) {
        gchar *tmp = g_build_filename(home, CONFIG_DIR, "baNotes.socket", NULL);
        strncpy(socket_path, tmp, sizeof(socket_path)-1);
        socket_path[sizeof(socket_path)-1] = '\0';
        g_free(tmp);
    }

    // Try to notify existing instance: connect to socket and send SHOW command
    if (socket_path[0]) {
        int cli = socket(AF_UNIX, SOCK_STREAM, 0);
        if (cli >= 0) {
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path)-1);
            if (connect(cli, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                // Another instance is running — ask it to show the main window.
                (void)write(cli, "SHOW", 4);
                close(cli);
                return 0;
            }
            close(cli);
        }
    }

    // No existing instance; create server socket to accept future requests
    if (socket_path[0]) {
        server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_sock >= 0) {
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path)-1);
            unlink(socket_path); // remove stale
            if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                if (listen(server_sock, 5) == 0) {
                    pthread_t tid;
                    pthread_create(&tid, NULL, instance_server_thread, NULL);
                    pthread_detach(tid);
                    atexit(cleanup_socket);
                } else {
                    close(server_sock); server_sock = -1;
                }
            } else {
                close(server_sock); server_sock = -1;
            }
        }
    }

    // Force X11 backend to allow window positioning (not supported on Wayland)
    gdk_set_allowed_backends("x11");

    gtk_init(&argc, &argv);
    /* Ensure WM_CLASS matches desktop file (StartupWMClass) */
    gdk_set_program_class("si.generacija.banotes");
    apply_app_css();

    app_init_config_dirs();
    main_window = create_main_window();
    gtk_widget_hide(main_window);
    create_tray_icon();
    gtk_main();
    return 0;
}
