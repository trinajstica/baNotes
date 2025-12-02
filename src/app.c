#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>

// Return pixbuf for the eye icon
GdkPixbuf *app_get_eye_icon(void) {
    static GdkPixbuf *eye = NULL;
    if (!eye) {
        GtkIconTheme *theme = gtk_icon_theme_get_default();
        eye = gtk_icon_theme_load_icon(theme, "view-preview-symbolic", 16, 0, NULL);
        if (!eye) eye = gtk_icon_theme_load_icon(theme, "view-visible-symbolic", 16, 0, NULL);
        if (!eye) eye = gtk_icon_theme_load_icon(theme, "document-preview", 16, 0, NULL);
    }
    return eye;
}

// Return pixbuf for the trash icon
GdkPixbuf *app_get_trash_icon(void) {
    static GdkPixbuf *trash = NULL;
    if (!trash) {
        GtkIconTheme *theme = gtk_icon_theme_get_default();
        trash = gtk_icon_theme_load_icon(theme, "user-trash-symbolic", 16, 0, NULL);
        if (!trash) trash = gtk_icon_theme_load_icon(theme, "edit-delete-symbolic", 16, 0, NULL);
        if (!trash) trash = gtk_icon_theme_load_icon(theme, "user-trash", 16, 0, NULL);
    }
    return trash;
}
#include "../include/app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <gdk/gdkkeysyms.h>

/* settings.conf removed: window position persistence disabled */

static char *get_notes_dir(void) {
    const char *home = getenv("HOME");
    if (!home) return NULL;
    static char path[4096];
    gchar *tmp = g_build_filename(home, CONFIG_DIR, NOTES_SUBDIR, NULL);
    strncpy(path, tmp, sizeof(path)-1);
    path[sizeof(path)-1] = '\0';
    g_free(tmp);
    return path;
}

static char *get_settings_path(void) {
    const char *home = getenv("HOME");
    if (!home) return NULL;
    gchar *tmp = g_build_filename(home, CONFIG_DIR, "settings.conf", NULL);
    char *path = g_strdup(tmp);
    g_free(tmp);
    return path;
}

// default enabled
static int app_word_wrap_enabled = 1;

int app_read_word_wrap(int *enabled) {
    if (!enabled) return 0;
    char *path = get_settings_path();
    if (!path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) { g_free(path); return 0; }
    char buf[256];
    int found = 0;
    while (fgets(buf, sizeof(buf), f)) {
        size_t l = strlen(buf);
        while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r')) buf[--l] = '\0';
        char *s = buf;
        while (*s && isspace((unsigned char)*s)) s++;
        if (strncmp(s, "wrap=", 5) == 0) {
            char *v = s + 5;
            if (*v == '1') { *enabled = 1; found = 1; break; }
            else { *enabled = 0; found = 1; break; }
        }
    }
    fclose(f);
    g_free(path);
    if (found) app_word_wrap_enabled = *enabled;
    return found;
}

void app_save_word_wrap(int enabled) {
    char *path = get_settings_path();
    if (!path) return;
    FILE *f = fopen(path, "w");
    if (!f) { g_free(path); return; }
    fprintf(f, "wrap=%d\n", enabled ? 1 : 0);
    fclose(f);
    g_free(path);
    app_word_wrap_enabled = enabled ? 1 : 0;
}

int app_get_word_wrap(void) { return app_word_wrap_enabled; }

gboolean app_textview_keypress(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    (void)user_data;
    if ((event->state & GDK_CONTROL_MASK) && (event->keyval == GDK_KEY_w || event->keyval == GDK_KEY_W)) {
        app_toggle_word_wrap();
        return TRUE;
    }
    return FALSE;
}

// Global textview registry to allow updating all open textviews and their labels
struct tv_entry {
    GtkWidget *tview;
    GtkWidget *label; // optional status label
};
static GSList *tv_list = NULL;

int app_register_textview(GtkWidget *tview, GtkWidget *label) {
    if (!tview) return 0;
    struct tv_entry *e = g_new0(struct tv_entry, 1);
    e->tview = tview;
    e->label = label;
    tv_list = g_slist_append(tv_list, e);
    // Ensure we clean up when the widget is destroyed
    g_signal_connect_swapped(tview, "destroy", G_CALLBACK(app_unregister_textview), tview);
    // Apply current wrap immediately
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tview), app_word_wrap_enabled ? GTK_WRAP_WORD : GTK_WRAP_NONE);
    if (label) {
        gtk_label_set_text(GTK_LABEL(label), app_word_wrap_enabled ? "Wrap: ON" : "Wrap: OFF");
    }
    return 1;
}

void app_unregister_textview(GtkWidget *tview) {
    if (!tview) return;
    GSList *p = tv_list;
    while (p) {
        struct tv_entry *e = (struct tv_entry*)p->data;
        if (e->tview == tview) {
            tv_list = g_slist_delete_link(tv_list, p);
            g_free(e);
            return;
        }
        p = p->next;
    }
}

// Update wrap mode for all registered tviews
void app_update_all_textviews_wrap(void) {
    GSList *p = tv_list;
    while (p) {
        struct tv_entry *e = (struct tv_entry*)p->data;
        if (GTK_IS_TEXT_VIEW(e->tview)) {
            gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(e->tview), app_word_wrap_enabled ? GTK_WRAP_WORD : GTK_WRAP_NONE);
        }
        if (e->label && GTK_IS_LABEL(e->label)) {
            gtk_label_set_text(GTK_LABEL(e->label), app_word_wrap_enabled ? "Wrap: ON" : "Wrap: OFF");
        }
        p = p->next;
    }
}

void app_toggle_word_wrap(void) {
    app_word_wrap_enabled = !app_word_wrap_enabled;
    app_save_word_wrap(app_word_wrap_enabled);
    app_update_all_textviews_wrap();
}
// End of wrap functions

void app_init_config_dirs(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    gchar *tmp = g_build_filename(home, CONFIG_DIR, NULL);
    char config_path[4096];
    strncpy(config_path, tmp, sizeof(config_path)-1);
    config_path[sizeof(config_path)-1] = '\0';
    g_free(tmp);
    mkdir(config_path, 0700);
    gchar *tmp2 = g_build_filename(config_path, NOTES_SUBDIR, NULL);
    char notes_path[4096];
    strncpy(notes_path, tmp2, sizeof(notes_path)-1);
    notes_path[sizeof(notes_path)-1] = '\0';
    g_free(tmp2);
    mkdir(notes_path, 0700);
}

// Helper: get mtime for sorting
static time_t get_file_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return st.st_mtime;
    return 0;
}

// Helper: struct for sorting
struct note_entry {
    char *filename;
    time_t mtime;
};

static int note_cmp(const void *a, const void *b) {
    const struct note_entry *na = a, *nb = b;
    return (nb->mtime - na->mtime);
}

void app_load_notes(GtkListStore *store, const char *filter) {
    gtk_list_store_clear(store);
    char *notes_dir = get_notes_dir();
    DIR *dir = opendir(notes_dir);
    if (!dir) return;
    struct dirent *entry;
    struct note_entry *notes = NULL;
    size_t count = 0;
    while ((entry = readdir(dir))) {
        if (entry->d_type != DT_REG) continue;
        char path[4096];
        gchar *tmp = g_build_filename(notes_dir, entry->d_name, NULL);
        strncpy(path, tmp, sizeof(path)-1);
        path[sizeof(path)-1] = '\0';
        g_free(tmp);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int match = 1;
        if (filter && *filter) {
            char *line = NULL;
            size_t len = 0;
            match = 0;
            while (getline(&line, &len, f) != -1) {
                if (strstr(line, filter)) { match = 1; break; }
            }
            free(line);
        }
        fclose(f);
        if (!match) continue;
        notes = realloc(notes, sizeof(*notes) * (count+1));
        // Store name without .txt extension
        const char *dot = strrchr(entry->d_name, '.');
        size_t base_len = dot && strcmp(dot, ".txt") == 0 ? (size_t)(dot - entry->d_name) : strlen(entry->d_name);
        char *title = strndup(entry->d_name, base_len);
        notes[count].filename = title;
        notes[count].mtime = get_file_mtime(path);
        count++;
    }
    closedir(dir);
    if (count > 1) qsort(notes, count, sizeof(*notes), note_cmp);
    GdkPixbuf *trash = app_get_trash_icon();
    for (size_t i = 0; i < count; ++i) {
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
            0, notes[i].filename,
            1, trash,
            -1);
        free(notes[i].filename);
    }
    free(notes);
}

void app_sort_notes(GtkListStore *store) {
    // Sorting handled in app_load_notes
}

// --- Window position settings ---
int app_read_window_position(int *x, int *y) {
    (void)x; (void)y;
    /* Window position persistence disabled; always report failure. */
    return 0;
}

void app_save_window_position(int x, int y) {
    (void)x; (void)y;
    /* No-op: do not write any settings file. */
}

// Delete a note by title (without .txt). Returns 1 on success, 0 on error.
int app_delete_note(const char *title) {
    char *notes_dir = get_notes_dir();
    if (!notes_dir || !title) return 0;
    char path[4096];
    if (strlen(title) > 4 && strcmp(title + strlen(title) - 4, ".txt") == 0) {
        gchar *tmp = g_build_filename(notes_dir, title, NULL);
        strncpy(path, tmp, sizeof(path)-1);
        path[sizeof(path)-1] = '\0';
        g_free(tmp);
    } else {
        gchar *with_ext = g_strconcat(title, ".txt", NULL);
        gchar *tmp = g_build_filename(notes_dir, with_ext, NULL);
        strncpy(path, tmp, sizeof(path)-1);
        path[sizeof(path)-1] = '\0';
        g_free(tmp);
        g_free(with_ext);
    }
    if (unlink(path) == 0) return 1;
    return 0;
}

int app_read_note(const char *title, char **out) {
    if (!title || !out) return 0;
    char *notes_dir = get_notes_dir();
    if (!notes_dir) return 0;
    char path[4096];
    if (strlen(title) > 4 && strcmp(title + strlen(title) - 4, ".txt") == 0) {
        gchar *tmp = g_build_filename(notes_dir, title, NULL);
        strncpy(path, tmp, sizeof(path)-1);
        path[sizeof(path)-1] = '\0';
        g_free(tmp);
    } else {
        gchar *with_ext = g_strconcat(title, ".txt", NULL);
        gchar *tmp = g_build_filename(notes_dir, with_ext, NULL);
        strncpy(path, tmp, sizeof(path)-1);
        path[sizeof(path)-1] = '\0';
        g_free(tmp);
        g_free(with_ext);
    }
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, sz, f) != (size_t)sz) { free(buf); fclose(f); return 0; }
    buf[sz] = '\0';
    fclose(f);
    *out = buf;
    return 1;
}

int app_write_note(const char *title, const char *content) {
    if (!title) return 0;
    char *notes_dir = get_notes_dir();
    if (!notes_dir) return 0;
    char path[4096];
    if (strlen(title) > 4 && strcmp(title + strlen(title) - 4, ".txt") == 0) {
        snprintf(path, sizeof(path), "%s/%s", notes_dir, title);
    } else {
        snprintf(path, sizeof(path), "%s/%s.txt", notes_dir, title);
    }
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    if (content) fprintf(f, "%s", content);
    fclose(f);
    return 1;
}

int app_rename_note(const char *old_title, const char *new_title) {
    if (!old_title || !new_title) return 0;
    char *notes_dir = get_notes_dir();
    if (!notes_dir) return 0;
    char oldpath[4096], newpath[4096];
    if (strlen(old_title) > 4 && strcmp(old_title + strlen(old_title) - 4, ".txt") == 0) {
        gchar *tmp = g_build_filename(notes_dir, old_title, NULL);
        strncpy(oldpath, tmp, sizeof(oldpath)-1);
        oldpath[sizeof(oldpath)-1] = '\0';
        g_free(tmp);
    } else {
        gchar *with_ext_old = g_strconcat(old_title, ".txt", NULL);
        gchar *tmp = g_build_filename(notes_dir, with_ext_old, NULL);
        strncpy(oldpath, tmp, sizeof(oldpath)-1);
        oldpath[sizeof(oldpath)-1] = '\0';
        g_free(tmp);
        g_free(with_ext_old);
    }
    if (strlen(new_title) > 4 && strcmp(new_title + strlen(new_title) - 4, ".txt") == 0) {
        gchar *tmp2 = g_build_filename(notes_dir, new_title, NULL);
        strncpy(newpath, tmp2, sizeof(newpath)-1);
        newpath[sizeof(newpath)-1] = '\0';
        g_free(tmp2);
    } else {
        gchar *with_ext_new = g_strconcat(new_title, ".txt", NULL);
        gchar *tmp2 = g_build_filename(notes_dir, with_ext_new, NULL);
        strncpy(newpath, tmp2, sizeof(newpath)-1);
        newpath[sizeof(newpath)-1] = '\0';
        g_free(tmp2);
        g_free(with_ext_new);
    }
    if (rename(oldpath, newpath) == 0) return 1;
    return 0;
}
