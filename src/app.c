#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>
#include <glib.h>

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

// Helper: create or lookup a GtkTextTag for a given tag name
static GtkTextTag* get_or_create_tag_for_name(GtkTextBuffer *buffer, const char *name) {
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
    GtkTextTag *tag = NULL;
    if (table) tag = gtk_text_tag_table_lookup(table, name);
    if (tag) return tag;
    /* Create tag based on name conventions: */
    if (g_strcmp0(name, "BOLD") == 0) {
        tag = gtk_text_buffer_create_tag(buffer, name, "weight", PANGO_WEIGHT_BOLD, NULL);
    } else if (g_strcmp0(name, "ITALIC") == 0) {
        tag = gtk_text_buffer_create_tag(buffer, name, "style", PANGO_STYLE_ITALIC, NULL);
    } else if (g_strcmp0(name, "UNDERLINE") == 0) {
        tag = gtk_text_buffer_create_tag(buffer, name, "underline", PANGO_UNDERLINE_SINGLE, NULL);
    } else if (g_str_has_prefix(name, "FG:#")) {
        const char *color = name + 3; /* FG: is 3 chars, color includes # */
        tag = gtk_text_buffer_create_tag(buffer, name, "foreground", color, NULL);
        g_object_set_data_full(G_OBJECT(tag), "bn-tag-name", g_strdup(name), g_free);
    } else if (g_str_has_prefix(name, "BG:#")) {
        const char *color = name + 3; /* BG: is 3 chars, color includes # */
        tag = gtk_text_buffer_create_tag(buffer, name, "background", color, NULL);
        g_object_set_data_full(G_OBJECT(tag), "bn-tag-name", g_strdup(name), g_free);
    } else {
        /* Generic fallback: create empty tag */
        tag = gtk_text_buffer_create_tag(buffer, name, NULL);
    }
    g_object_set_data_full(G_OBJECT(tag), "bn-tag-name", g_strdup(name), g_free);
    return tag;
}

// Parse a previously serialized rich note format into buffer. Returns 1 on success.
static int parse_rich_content_into_buffer(GtkTextBuffer *buffer, const char *raw) {
    if (!buffer || !raw) return 0;
    const char *p = raw;
    const char *hdr = "BA-RICH-V1\n";
    if (strncmp(p, hdr, strlen(hdr)) != 0) return 0;
    p += strlen(hdr);
    // read base64 line
    const char *nl = strchr(p, '\n');
    if (!nl) return 0;
    size_t base64_len = (size_t)(nl - p);
    char *b64 = g_strndup(p, base64_len);
    gsize decoded_len = 0;
    guchar *decoded = g_base64_decode(b64, &decoded_len);
    g_free(b64);
    // set plain text
    gtk_text_buffer_set_text(buffer, (const char*)decoded, (gssize)decoded_len);
    g_free(decoded);
    // Now parse tags
    const char *lines = nl + 1;
    const char *cur = lines;
    // Each line like: TAG|NAME|START|END\n
    while (*cur) {
        const char *next = strchr(cur, '\n');
        size_t len = next ? (size_t)(next - cur) : strlen(cur);
        if (len > 0) {
            char *line = g_strndup(cur, len);
            // parse line
            if (g_str_has_prefix(line, "TAG|")) {
                // fields separated by '|'
                char **parts = g_strsplit(line + 4, "|", 4);
                // parts[0]=NAME parts[1]=START parts[2]=END
                if (parts && parts[0] && parts[1] && parts[2]) {
                    const char *name = parts[0];
                    int start = atoi(parts[1]);
                    int end = atoi(parts[2]);
                    GtkTextTag *tag = get_or_create_tag_for_name(buffer, name);
                    if (tag) {
                        GtkTextIter s, e;
                        gtk_text_buffer_get_iter_at_offset(buffer, &s, start);
                        gtk_text_buffer_get_iter_at_offset(buffer, &e, end);
                        gtk_text_buffer_apply_tag(buffer, tag, &s, &e);
                    }
                }
                g_strfreev(parts);
            }
            g_free(line);
        }
        if (!next) break;
        cur = next + 1;
    }
    return 1;
}

// Public: load note into buffer either as plain text or parse custom format
int app_load_note_into_buffer(const char *title, GtkTextBuffer *buffer) {
    char *content = NULL;
    if (!app_read_note(title, &content)) return 0;
    if (content && g_str_has_prefix(content, "BA-RICH-V1\n")) {
        // parse into buffer
        int res = parse_rich_content_into_buffer(buffer, content);
        g_free(content);
        return res;
    } else {
        if (content) {
            gtk_text_buffer_set_text(buffer, content, -1);
            g_free(content);
            return 1;
        } else {
            return 0;
        }
    }
}

// Public: serialize buffer into custom format (returns g_malloc'd string)
char *app_serialize_buffer_rich(GtkTextBuffer *buffer) {
    if (!buffer) return NULL;
    // Get whole text
    GtkTextIter s, e;
    gtk_text_buffer_get_start_iter(buffer, &s);
    gtk_text_buffer_get_end_iter(buffer, &e);
    gchar *text = gtk_text_buffer_get_text(buffer, &s, &e, FALSE);
    gsize text_len = (text ? strlen(text) : 0);
    gchar *b64 = g_base64_encode((const guchar*)text, text_len);
    g_free(text);
    GString *out = g_string_new(NULL);
    g_string_append(out, "BA-RICH-V1\n");
    g_string_append(out, b64);
    g_string_append(out, "\n");
    g_free(b64);
    // Iterate tags and write ranges by scanning toggles
    GtkTextIter it; gtk_text_buffer_get_start_iter(buffer, &it);
    GtkTextIter end_it; gtk_text_buffer_get_end_iter(buffer, &end_it);
    GHashTable *active = g_hash_table_new(g_direct_hash, g_direct_equal);
    // active map: key=tag pointer, value=pointer to int start offset
    while (!gtk_text_iter_equal(&it, &end_it)) {
            GSList *tags = gtk_text_iter_get_tags(&it);
        int offset = gtk_text_iter_get_offset(&it);
        // find tags that ended
        GList *p = g_hash_table_get_keys(active);
        for (GList *q = p; q; q = q->next) {
            GtkTextTag *atag = (GtkTextTag*)q->data;
            gboolean still = FALSE;
                for (GSList *r = tags; r; r = r->next) {
                if (r->data == atag) { still = TRUE; break; }
            }
            if (!still) {
                gpointer startp = g_hash_table_lookup(active, atag);
                if (startp) {
                    int start_off = GPOINTER_TO_INT(startp);
                    const char *name = (const char*)g_object_get_data(G_OBJECT(atag), "bn-tag-name");
                    g_string_append_printf(out, "TAG|%s|%d|%d\n", name ? name : "", start_off, offset);
                }
                g_hash_table_remove(active, atag);
            }
        }
        g_list_free(p);
        // find tags that started
            for (GSList *r = tags; r; r = r->next) {
            GtkTextTag *tt = (GtkTextTag*)r->data;
            if (!g_hash_table_contains(active, tt)) {
                g_hash_table_insert(active, tt, GINT_TO_POINTER(offset));
            }
        }
            if (!gtk_text_iter_forward_char(&it)) break;
    }
    // finalize active tags
    int end_offset = gtk_text_iter_get_offset(&end_it);
    GList *keys = g_hash_table_get_keys(active);
    for (GList *q = keys; q; q = q->next) {
        GtkTextTag *atag = (GtkTextTag*)q->data;
        gpointer startp = g_hash_table_lookup(active, atag);
        if (startp) {
            int start_off = GPOINTER_TO_INT(startp);
                const char *name = (const char*)g_object_get_data(G_OBJECT(atag), "bn-tag-name");
            g_string_append_printf(out, "TAG|%s|%d|%d\n", name ? name : "", start_off, end_offset);
        }
    }
    g_list_free(keys);
    g_hash_table_destroy(active);
    char *res = g_strdup(out->str);
    g_string_free(out, TRUE);
    return res;
}

// Public: parse a serialized rich string into the given buffer.
int app_parse_rich_string_into_buffer(const char *serialized, GtkTextBuffer *buffer) {
    if (!serialized || !buffer) return 0;
    // If the string is BA-RICH-V1, parse accordingly
    if (g_str_has_prefix(serialized, "BA-RICH-V1\n")) {
        return parse_rich_content_into_buffer(buffer, serialized);
    }
    // Otherwise treat as plain text
    gtk_text_buffer_set_text(buffer, serialized, -1);
    return 1;
}

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
            // Read entire file and decode if necessary
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *buf = malloc(sz + 1);
            if (!buf) { fclose(f); continue; }
            if (fread(buf, 1, sz, f) != (size_t)sz) { free(buf); fclose(f); continue; }
            buf[sz] = '\0';
            char *plain = buf;
            const char *hdr = "BA-RICH-V1\n";
            if (sz > (long)strlen(hdr) && strncmp(buf, hdr, strlen(hdr)) == 0) {
                const char *p = buf + strlen(hdr);
                const char *nl = strchr(p, '\n');
                if (nl) {
                    size_t b64len = (size_t)(nl - p);
                    char *b64 = g_strndup(p, b64len);
                    gsize dlen = 0;
                    guchar *dec = g_base64_decode(b64, &dlen);
                    free(buf);
                    plain = g_strndup((const char*)dec, dlen);
                    g_free(dec);
                    g_free(b64);
                }
            }
            match = (strstr(plain, filter) != NULL);
            free(plain);
            fclose(f);
            if (!match) continue;
        } else {
            fclose(f);
        }
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
