/* Application version — update here when releasing */
#define VERZIJA "baNotes v1.00"

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
static GtkListStore *notes_store = NULL;
static GtkWidget *tree = NULL;
static AppIndicator *indicator = NULL;

// Napoved funkcij
static gboolean on_tree_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static void on_quit(GtkMenuItem *item, gpointer user_data);
static void on_show_hide(GtkMenuItem *item, gpointer user_data);
static void on_search_changed(GtkEntry *entry, gpointer user_data);
static void on_clear_clicked(GtkButton *btn, gpointer user_data);
static gboolean on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data);
static GtkWidget* create_main_window(void);
static void create_tray_icon(void);
static void editor_destroy(GtkWidget *w, gpointer user_data);
static void on_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data);
static gboolean on_wrap_label_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
// single-instance socket path
static char socket_path[4096] = {0};
static int server_sock = -1;

// Forward
static void *instance_server_thread(void *arg);
static gboolean bring_main_window(gpointer user_data);

static void cleanup_socket(void) {
    if (server_sock != -1) close(server_sock);
    if (socket_path[0]) unlink(socket_path);
}
static void on_editor_ok_clicked(GtkButton *btn, gpointer user_data);
static char *first_nonempty_line(const char *text);

// Editor data
typedef struct {
    char *title; // brez .txt
    GtkTextBuffer *buffer;
    GtkWidget *window;
} EditorData;

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

// Handler: klik na tree view - preveri, če je klik na stolpcu smeti
static gboolean on_tree_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    if (event->type != GDK_BUTTON_PRESS || event->button != 1) return FALSE;
    GtkTreeView *treeview = GTK_TREE_VIEW(widget);
    int x = (int)event->x, y = (int)event->y;
    GtkTreePath *path = NULL;
    GtkTreeViewColumn *col = NULL;
    if (gtk_tree_view_get_path_at_pos(treeview, x, y, &path, &col, NULL, NULL)) {
        // Primerjamo, ali je stolpec za smeti (2. stolpec, indeks 1)
        GtkTreeViewColumn *trash_col = gtk_tree_view_get_column(treeview, 1);
        if (col == trash_col) {
            GtkTreeModel *model = gtk_tree_view_get_model(treeview);
            GtkTreeIter iter;
            if (gtk_tree_model_get_iter(model, &iter, path)) {
                gchar *title = NULL;
                gtk_tree_model_get(model, &iter, 0, &title, -1);
                if (title) {
                    /* Remember selected index before reloading notes, so we can restore
                     * selection after deleting a note. This ensures that deleting the
                     * currently selected note keeps the cursor at the same visual
                     * position (or moves to the new last if the deleted one was
                     * last). If nothing was selected, we leave selection as-is. */
                    int saved_index = -1;
                    GtkTreeSelection *cur_sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
                    GtkTreeModel *cur_model = NULL;
                    GtkTreeIter cur_iter;
                    if (gtk_tree_selection_get_selected(cur_sel, &cur_model, &cur_iter)) {
                        GtkTreePath *cur_path = gtk_tree_model_get_path(cur_model, &cur_iter);
                        const gint *indices = gtk_tree_path_get_indices(cur_path);
                        if (indices) saved_index = indices[0];
                        gtk_tree_path_free(cur_path);
                    }
                    char *dmsg = g_strdup_printf("Delete note '%s'?", title);
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
                    if (resp == GTK_RESPONSE_YES) {
                        if (app_delete_note(title)) {
                            // Reload notes
                            app_load_notes(notes_store, gtk_entry_get_text(GTK_ENTRY(search_entry)));
                            // Restore selection if we remembered one and the list is not empty
                            if (saved_index >= 0 && tree && notes_store) {
                                GtkTreeModel *model = GTK_TREE_MODEL(notes_store);
                                gint nrows = gtk_tree_model_iter_n_children(model, NULL);
                                if (nrows > 0) {
                                    gint reselect = saved_index;
                                    if (reselect >= nrows) reselect = nrows - 1;
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
            }
            gtk_tree_path_free(path);
            return TRUE;
        }
        gtk_tree_path_free(path);
    }
    return FALSE;
}

// Save the edited note when the "OK" button is clicked
static void on_editor_ok_clicked(GtkButton *btn, gpointer user_data) {
    EditorData *ed = (EditorData*)user_data;
    if (!ed) return;
    GtkTextBuffer *buffer = ed->buffer;
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_end_iter(buffer, &end);
    gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    // Najdi prvo ne-prazno vrstico za naslov
    char *first = first_nonempty_line(text);
    char *new_title = NULL;
    if (first && *first) {
        new_title = sanitize_title(first);
    } else {
        time_t t = time(NULL);
        new_title = sanitize_title((char*)g_strdup_printf("untitled_%ld", (long)t));
    }

    if (new_title) {
        // Če se naslov spremeni, poskusi preimenovati; pri urejanju NE ustvarjam avtomatičnega sufiksa
        if (g_strcmp0(new_title, ed->title) != 0) {
            // Preveri, ali ciljna datoteka že obstaja
            const char *home = getenv("HOME");
            char notes_dir[4096] = {0};
            if (home) {
                gchar *tmp = g_build_filename(home, CONFIG_DIR, NOTES_SUBDIR, NULL);
                strncpy(notes_dir, tmp, sizeof(notes_dir)-1);
                notes_dir[sizeof(notes_dir)-1] = '\0';
                g_free(tmp);
            }
            char targetpath[4096];
            if (strlen(new_title) > 4 && strcmp(new_title + strlen(new_title) - 4, ".txt") == 0) {
                gchar *tmp2 = g_build_filename(notes_dir, new_title, NULL);
                strncpy(targetpath, tmp2, sizeof(targetpath)-1);
                targetpath[sizeof(targetpath)-1] = '\0';
                g_free(tmp2);
            } else {
                gchar *with_ext = g_strconcat(new_title, ".txt", NULL);
                gchar *tmp2 = g_build_filename(notes_dir, with_ext, NULL);
                strncpy(targetpath, tmp2, sizeof(targetpath)-1);
                targetpath[sizeof(targetpath)-1] = '\0';
                g_free(tmp2);
                g_free(with_ext);
            }
            if (g_file_test(targetpath, G_FILE_TEST_EXISTS)) {
                char *msg = g_strdup_printf("A file named '%s' already exists. Rename it manually or choose a different title.", new_title);
                    GtkWidget *err = gtk_dialog_new_with_buttons("File exists",
                    GTK_WINDOW(ed->window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                    "Ok", GTK_RESPONSE_OK,
                    NULL);
                GtkWidget *err_area = gtk_dialog_get_content_area(GTK_DIALOG(err));
                GtkWidget *err_label = gtk_label_new(msg);
                gtk_container_add(GTK_CONTAINER(err_area), err_label);
                gtk_widget_show_all(err);
                gtk_dialog_run(GTK_DIALOG(err));
                gtk_widget_destroy(err);
                g_free(msg);
                g_free(new_title);
                if (first) g_free(first);
                g_free(text);
                return;
            }
            // Poskusi preimenovati
            if (!app_rename_note(ed->title, new_title)) {
                char *msg2 = g_strdup_printf("Cannot rename note to '%s'.", new_title);
                GtkWidget *err = gtk_dialog_new_with_buttons("Rename error",
                    GTK_WINDOW(ed->window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                    "Ok", GTK_RESPONSE_OK,
                    NULL);
                GtkWidget *err_area2 = gtk_dialog_get_content_area(GTK_DIALOG(err));
                GtkWidget *err_label2 = gtk_label_new(msg2);
                gtk_container_add(GTK_CONTAINER(err_area2), err_label2);
                gtk_widget_show_all(err);
                gtk_dialog_run(GTK_DIALOG(err));
                gtk_widget_destroy(err);
                g_free(msg2);
                g_free(new_title);
                if (first) g_free(first);
                g_free(text);
                return;
            }
            g_free(ed->title);
            ed->title = g_strdup(new_title);
            // refresh list
            app_load_notes(notes_store, gtk_entry_get_text(GTK_ENTRY(search_entry)));
        }

        // Zapiši vsebino v datoteko (uporabi trenutno ed->title)
        app_write_note(ed->title, text);
        g_free(new_title);
    }

    if (first) g_free(first);
    g_free(text);

    // Close the window (editor_destroy will free EditorData)
    gtk_widget_destroy(ed->window);
}

static void on_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data) {
    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) return;
    gchar *title = NULL;
    gtk_tree_model_get(model, &iter, 0, &title, -1);
    if (!title) return;

    // Create editor window (save on 'OK', 'Cancel' discards changes)
    GtkWidget *ewin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char wtitle[512];
    snprintf(wtitle, sizeof(wtitle), "Edit: %s", title);
    gtk_window_set_title(GTK_WINDOW(ewin), wtitle);
    gtk_window_set_default_size(GTK_WINDOW(ewin), 600, 400);
    gtk_window_set_transient_for(GTK_WINDOW(ewin), GTK_WINDOW(main_window));
    gtk_window_set_position(GTK_WINDOW(ewin), GTK_WIN_POS_CENTER_ON_PARENT);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(ewin), vbox);
    gtk_widget_set_margin_start(vbox, 5);
    gtk_widget_set_margin_end(vbox, 5);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
    GtkWidget *tview = gtk_text_view_new();
    gtk_container_add(GTK_CONTAINER(scroll), tview);
    /* Label will be inserted into the buttons row; create it now but add later */
    GtkWidget *wrap_label = gtk_label_new(app_get_word_wrap() ? "Wrap: ON" : "Wrap: OFF");
    gtk_widget_set_margin_end(wrap_label, 5);
    /* Wrap label clickable: put in eventbox so it receives pointer events */
    GtkWidget *wrap_eventbox = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(wrap_eventbox), wrap_label);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(wrap_eventbox), FALSE);
    gtk_widget_add_events(wrap_eventbox, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(wrap_eventbox, "button-press-event", G_CALLBACK(on_wrap_label_button_press), NULL);
    app_register_textview(tview, wrap_label);
    /* Set word wrap initially from saved settings */
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tview), app_get_word_wrap() ? GTK_WRAP_WORD : GTK_WRAP_NONE);
    gtk_widget_add_events(tview, GDK_KEY_PRESS_MASK);
    g_signal_connect(tview, "key-press-event", G_CALLBACK(app_textview_keypress), NULL);

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tview));
    // Load content
    char *content = NULL;
    if (!app_read_note(title, &content)) content = g_strdup("");
    gtk_text_buffer_set_text(buffer, content, -1);

    EditorData *ed = g_new0(EditorData, 1);
    ed->title = g_strdup(title);
    ed->buffer = buffer;
    ed->window = ewin;

    // Add OK / Cancel buttons
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    GtkWidget *ok = gtk_button_new_with_label("Ok");
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    /* Ensure consistent spacing: OK has 5px bottom + 5px right; Cancel has 5px bottom */
    gtk_widget_set_margin_bottom(ok, 5);
    gtk_widget_set_margin_end(ok, 5);
    gtk_widget_set_margin_bottom(cancel, 5);
    /* Pack buttons on the left */
    gtk_box_pack_start(GTK_BOX(hbox), ok, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), cancel, FALSE, FALSE, 0);
    /* Spacer */
    GtkWidget *spacer = gtk_label_new(NULL);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), spacer, TRUE, TRUE, 0);
    /* Wrap label on the right */
    gtk_widget_set_halign(wrap_eventbox, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(hbox), wrap_eventbox, FALSE, FALSE, 0);

    // Cancel: just close the window (destroy will free EditorData via editor_destroy)
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_widget_destroy), ewin);

    // OK: save (respect existing file names — check collisions)
    g_signal_connect(ok, "clicked", G_CALLBACK(on_editor_ok_clicked), ed);

    g_signal_connect(ewin, "destroy", G_CALLBACK(editor_destroy), ed);

    gtk_widget_show_all(ewin);
    g_free(content);
    g_free(title);
}

static void editor_destroy(GtkWidget *w, gpointer user_data) {
    EditorData *ed = (EditorData*)user_data;
    if (!ed) return;
    if (ed->title) g_free(ed->title);
    g_free(ed);
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

// Called in main thread via g_idle_add to bring window to front
static gboolean bring_main_window(gpointer user_data) {
    if (main_window) {
        if (!gtk_widget_get_visible(main_window)) gtk_widget_show_all(main_window);
        gtk_window_present(GTK_WINDOW(main_window));
    }
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
        gtk_widget_hide(main_window);
    } else {
        int x = -1, y = -1;
        if (app_read_window_position(&x, &y) && x >= 0 && y >= 0) {
            gtk_window_move(GTK_WINDOW(main_window), x, y);
        }
        app_load_notes(notes_store, gtk_entry_get_text(GTK_ENTRY(search_entry)));
        gtk_widget_show_all(main_window);
        // Bring to front and focus
        gtk_window_present(GTK_WINDOW(main_window));
        gtk_widget_grab_focus(main_window);
    }
}

static void on_quit(GtkMenuItem *item, gpointer user_data) {
    gtk_main_quit();
}

static void on_search_changed(GtkEntry *entry, gpointer user_data) {
    const char *text = gtk_entry_get_text(entry);
    app_load_notes(notes_store, text);
}

static void on_clear_clicked(GtkButton *btn, gpointer user_data) {
    gtk_entry_set_text(GTK_ENTRY(search_entry), "");
    app_load_notes(notes_store, "");
        /* Select first note if it exists */
    if (tree && notes_store) {
        GtkTreeModel *model = GTK_TREE_MODEL(notes_store);
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter_first(model, &iter)) {
            GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
            GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
            gtk_tree_selection_select_path(sel, path);
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(tree), path, NULL, FALSE);
            gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(tree), path, NULL, TRUE, 0.5, 0.0);
            gtk_tree_path_free(path);
        }
    }
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

typedef struct {
    GtkWindow *dlg;
    GtkWidget *tview;
} NewNoteData;

static void on_add_ok_clicked(GtkButton *btn, gpointer user_data) {
    NewNoteData *nd = (NewNoteData*)user_data;
    if (!nd) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(nd->tview));
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(buf, &start);
    gtk_text_buffer_get_end_iter(buf, &end);
    gchar *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);

    // Use the first non-empty line as the title
    char *first = first_nonempty_line(text);
    char *title = NULL;
    if (first && *first) {
        title = sanitize_title(first);
    } else {
        time_t t = time(NULL);
        title = sanitize_title((char*)g_strdup_printf("untitled_%ld", (long)t));
    }

    if (title) {
        // Check if a file with the same title already exists; if so, find a unique name by appending -1, -2, ...
        const char *home = getenv("HOME");
        char notes_dir[4096] = {0};
        if (home) {
            gchar *tmp = g_build_filename(home, CONFIG_DIR, NOTES_SUBDIR, NULL);
            strncpy(notes_dir, tmp, sizeof(notes_dir)-1);
            notes_dir[sizeof(notes_dir)-1] = '\0';
            g_free(tmp);
        }
        char path[4096];
        if (strlen(title) > 4 && strcmp(title + strlen(title) - 4, ".txt") == 0) {
            gchar *tmp2 = g_build_filename(notes_dir, title, NULL);
            strncpy(path, tmp2, sizeof(path)-1);
            path[sizeof(path)-1] = '\0';
            g_free(tmp2);
        } else {
            gchar *with_ext = g_strconcat(title, ".txt", NULL);
            gchar *tmp2 = g_build_filename(notes_dir, with_ext, NULL);
            strncpy(path, tmp2, sizeof(path)-1);
            path[sizeof(path)-1] = '\0';
            g_free(tmp2);
            g_free(with_ext);
        }

        char *unique = g_strdup(title);
        int suffix = 1;
        while (g_file_test(path, G_FILE_TEST_EXISTS)) {
            g_free(unique);
            unique = g_strdup_printf("%s-%d", title, suffix++);
            if (strlen(unique) > 4 && strcmp(unique + strlen(unique) - 4, ".txt") == 0) {
                gchar *tmp3 = g_build_filename(notes_dir, unique, NULL);
                strncpy(path, tmp3, sizeof(path)-1);
                path[sizeof(path)-1] = '\0';
                g_free(tmp3);
            } else {
                gchar *u_ext = g_strconcat(unique, ".txt", NULL);
                gchar *tmp3 = g_build_filename(notes_dir, u_ext, NULL);
                strncpy(path, tmp3, sizeof(path)-1);
                path[sizeof(path)-1] = '\0';
                g_free(tmp3);
                g_free(u_ext);
            }
        }

        // Save note with unique name
        app_write_note(unique, text);
        // Refresh list
        app_load_notes(notes_store, gtk_entry_get_text(GTK_ENTRY(search_entry)));
        g_free(unique);
        g_free(title);
    }
    if (first) g_free(first);
    g_free(text);
    gtk_widget_destroy(GTK_WIDGET(nd->dlg));
    g_free(nd);
}

// Handler: '+' button for new note
static void on_add_clicked(GtkButton *btn, gpointer user_data) {
    GtkWidget *dlg = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(dlg), "New note");
    gtk_window_set_default_size(GTK_WINDOW(dlg), 600, 400);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(main_window));
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER_ON_PARENT);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(dlg), vbox);
    gtk_widget_set_margin_start(vbox, 5);
    gtk_widget_set_margin_end(vbox, 5);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
    GtkWidget *tview = gtk_text_view_new();
    gtk_container_add(GTK_CONTAINER(scroll), tview);
    /* Label will be placed into the buttons hbox; create it now for registration */
    GtkWidget *wrap_label = gtk_label_new(app_get_word_wrap() ? "Wrap: ON" : "Wrap: OFF");
    gtk_widget_set_margin_end(wrap_label, 5);
    GtkWidget *wrap_eventbox = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(wrap_eventbox), FALSE);
    gtk_container_add(GTK_CONTAINER(wrap_eventbox), wrap_label);
    gtk_widget_add_events(wrap_eventbox, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(wrap_eventbox, "button-press-event", G_CALLBACK(on_wrap_label_button_press), NULL);
    app_register_textview(tview, wrap_label);
    /* Set word wrap initially from saved settings */
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tview), app_get_word_wrap() ? GTK_WRAP_WORD : GTK_WRAP_NONE);
    gtk_widget_add_events(tview, GDK_KEY_PRESS_MASK);
    g_signal_connect(tview, "key-press-event", G_CALLBACK(app_textview_keypress), NULL);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    GtkWidget *ok = gtk_button_new_with_label("Ok");
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    /* Ensure consistent spacing: OK has 5px bottom + 5px right; Cancel has 5px bottom */
    gtk_widget_set_margin_bottom(ok, 5);
    gtk_widget_set_margin_end(ok, 5);
    gtk_widget_set_margin_bottom(cancel, 5);
    /* Pack buttons on the left */
    gtk_box_pack_start(GTK_BOX(hbox), ok, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), cancel, FALSE, FALSE, 0);
    /* Spacer to push wrap label to the right */
    GtkWidget *spacer = gtk_label_new(NULL);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), spacer, TRUE, TRUE, 0);
    /* Wrap label on the right */
    gtk_widget_set_halign(wrap_eventbox, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(hbox), wrap_eventbox, FALSE, FALSE, 0);

    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_widget_destroy), dlg);

    NewNoteData *nd = g_new0(NewNoteData, 1);
    nd->dlg = GTK_WINDOW(dlg);
    nd->tview = tview;

    g_signal_connect(ok, "clicked", G_CALLBACK(on_add_ok_clicked), nd);

    gtk_widget_show_all(dlg);
}

static gboolean on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    int x, y;
    gtk_window_get_position(GTK_WINDOW(widget), &x, &y);
    app_save_window_position(x, y);
    gtk_widget_hide(widget);
    return TRUE; // prepreči uničenje
}

static GtkWidget* create_main_window(void) {
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "baNotes");
    gtk_window_set_default_size(GTK_WINDOW(win), 400, 600);
    g_signal_connect(win, "delete-event", G_CALLBACK(on_window_delete), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(win), vbox);
    /* Add 5px margin left/right */
    gtk_widget_set_margin_start(vbox, 5);
    gtk_widget_set_margin_end(vbox, 5);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 5);

    search_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), "Search notes...");
    g_signal_connect(search_entry, "changed", G_CALLBACK(on_search_changed), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), search_entry, TRUE, TRUE, 0);

    GtkWidget *add_btn = gtk_button_new_with_label("+");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(hbox), add_btn, FALSE, FALSE, 0);

    clear_btn = gtk_button_new_with_label("Clear");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(hbox), clear_btn, FALSE, FALSE, 0);

    // Model: 0=title, 1=trash icon
    notes_store = gtk_list_store_new(2, G_TYPE_STRING, GDK_TYPE_PIXBUF);
    tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(notes_store));

    // Title (expandable)
    GtkCellRenderer *renderer_text = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes("Note title", renderer_text, "text", 0, NULL);
    gtk_tree_view_column_set_expand(col, TRUE);
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

    // Connect click on cell (only for trash) and double-click (row-activated) for editing
    g_signal_connect(tree, "button-press-event", G_CALLBACK(on_tree_button_press), NULL);
    g_signal_connect(tree, "row-activated", G_CALLBACK(on_row_activated), NULL);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), tree);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    return win;
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
    } else if (g_file_test("/usr/share/pixmaps/baNotes.png", G_FILE_TEST_EXISTS)) {
        /* fallback to system pixmaps */
        app_indicator_set_icon_full(indicator, "/usr/share/pixmaps/baNotes.png", "baNotes");
    } else if (g_file_test("baNotes.png", G_FILE_TEST_EXISTS)) {
        /* developer repo-local icon */
        gchar *cwd_path = g_canonicalize_filename("baNotes.png", NULL);
        app_indicator_set_icon_full(indicator, cwd_path, "baNotes");
        g_free(cwd_path);
    } else {
        app_indicator_set_icon_full(indicator, "document-open", "baNotes");
    }
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_menu(indicator, GTK_MENU(menu));
    /* Set label/title so hover may show application name in some desktop environments */
    app_indicator_set_label(indicator, "baNotes", NULL);

    /* No GtkStatusIcon fallback to avoid deprecated APIs; AppIndicator is primary. */
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
                // Another instance is running — exit silently without showing anything
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

    gtk_init(&argc, &argv);
    /* Ensure WM_CLASS matches desktop file (StartupWMClass) */
    gdk_set_program_class("si.generacija.banotes");
    // Set global font size 16px
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, "* { font-size: 16px; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_unref(css);

    app_init_config_dirs();
    main_window = create_main_window();
    gtk_widget_hide(main_window);
    create_tray_icon();
    gtk_main();
    return 0;
}
