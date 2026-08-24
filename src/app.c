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

        GtkStyleContext *ctx = gtk_style_context_new();
        GtkWidgetPath *path = gtk_widget_path_new();
        gtk_widget_path_append_type(path, GTK_TYPE_LABEL);
        gtk_style_context_set_path(ctx, path);
        GdkRGBA fg;
        gtk_style_context_get_color(ctx, GTK_STATE_FLAG_NORMAL, &fg);
        gtk_widget_path_free(path);
        g_object_unref(ctx);

        GtkIconInfo *info = gtk_icon_theme_lookup_icon(theme, "user-trash-symbolic", 16, GTK_ICON_LOOKUP_FORCE_SYMBOLIC);
        if (!info) info = gtk_icon_theme_lookup_icon(theme, "edit-delete-symbolic", 16, GTK_ICON_LOOKUP_FORCE_SYMBOLIC);
        if (info) {
            gboolean was_symbolic = FALSE;
            trash = gtk_icon_info_load_symbolic(info, &fg, NULL, NULL, NULL, &was_symbolic, NULL);
            g_object_unref(info);
        }

        if (!trash) trash = gtk_icon_theme_load_icon(theme, "user-trash-symbolic", 16, GTK_ICON_LOOKUP_FORCE_SYMBOLIC, NULL);
        if (!trash) trash = gtk_icon_theme_load_icon(theme, "edit-delete-symbolic", 16, GTK_ICON_LOOKUP_FORCE_SYMBOLIC, NULL);
        if (!trash) trash = gtk_icon_theme_load_icon(theme, "user-trash", 16, 0, NULL);
    }
    return trash;
}

GdkPixbuf *app_get_folder_icon(void) {
    static GdkPixbuf *folder = NULL;
    if (!folder) {
        GtkIconTheme *theme = gtk_icon_theme_get_default();
        folder = gtk_icon_theme_load_icon(theme, "folder-symbolic", 16, 0, NULL);
        if (!folder) folder = gtk_icon_theme_load_icon(theme, "folder", 16, 0, NULL);
    }
    return folder;
}

GdkPixbuf *app_get_parent_icon(void) {
    static GdkPixbuf *parent = NULL;
    if (!parent) {
        GtkIconTheme *theme = gtk_icon_theme_get_default();
        parent = gtk_icon_theme_load_icon(theme, "go-up-symbolic", 16, 0, NULL);
        if (!parent) parent = gtk_icon_theme_load_icon(theme, "go-up", 16, 0, NULL);
        if (!parent) parent = app_get_folder_icon();
    }
    return parent;
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
    return g_build_filename(home, CONFIG_DIR, NOTES_SUBDIR, NULL);
}

/* All paths exposed to the UI are relative to the notes directory. */
static gboolean valid_relative_path(const char *path) {
    if (!path || !*path || path[0] == '/' || path[0] == '\\') return path && !*path;
    gchar **parts = g_strsplit_set(path, "/\\", -1);
    gboolean valid = TRUE;
    for (gint i = 0; parts && parts[i]; ++i) {
        if (!*parts[i] || g_strcmp0(parts[i], ".") == 0 || g_strcmp0(parts[i], "..") == 0) {
            valid = FALSE;
            break;
        }
    }
    g_strfreev(parts);
    return valid;
}

static gchar *build_note_path(const char *title) {
    if (!title || !valid_relative_path(title)) return NULL;
    char *notes_dir = get_notes_dir();
    if (!notes_dir) return NULL;
    gchar *filename = (strlen(title) > 4 && strcmp(title + strlen(title) - 4, ".txt") == 0)
        ? g_strdup(title) : g_strconcat(title, ".txt", NULL);
    gchar *path = g_build_filename(notes_dir, filename, NULL);
    g_free(notes_dir);
    g_free(filename);
    return path;
}

static gchar *build_folder_path(const char *folder) {
    if (!folder || !*folder) return get_notes_dir();
    if (!valid_relative_path(folder)) return NULL;
    char *notes_dir = get_notes_dir();
    if (!notes_dir) return NULL;
    gchar *path = g_build_filename(notes_dir, folder, NULL);
    g_free(notes_dir);
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
    
    // Read existing window position if any
    int win_x = -1, win_y = -1;
    FILE *rf = fopen(path, "r");
    if (rf) {
        char buf[256];
        while (fgets(buf, sizeof(buf), rf)) {
            size_t l = strlen(buf);
            while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r')) buf[--l] = '\0';
            char *s = buf;
            while (*s && isspace((unsigned char)*s)) s++;
            if (strncmp(s, "window_x=", 9) == 0) {
                win_x = atoi(s + 9);
            } else if (strncmp(s, "window_y=", 9) == 0) {
                win_y = atoi(s + 9);
            }
        }
        fclose(rf);
    }
    
    // Write all settings
    FILE *f = fopen(path, "w");
    if (!f) { g_free(path); return; }
    fprintf(f, "wrap=%d\n", enabled ? 1 : 0);
    if (win_x >= 0 && win_y >= 0) {
        fprintf(f, "window_x=%d\n", win_x);
        fprintf(f, "window_y=%d\n", win_y);
    }
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
                    int start_off = GPOINTER_TO_INT(startp) - 1;
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
                /* Store offset + 1 because GINT_TO_POINTER(0) is NULL and
                 * would make a tag beginning at the first character vanish. */
                g_hash_table_insert(active, tt, GINT_TO_POINTER(offset + 1));
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
            int start_off = GPOINTER_TO_INT(startp) - 1;
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
    g_mkdir_with_parents(config_path, 0700);
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
    if (na->mtime < nb->mtime) return 1;
    if (na->mtime > nb->mtime) return -1;
    return 0;
}

static gboolean is_real_directory(const char *path) {
    return path && g_file_test(path, G_FILE_TEST_IS_DIR) &&
        !g_file_test(path, G_FILE_TEST_IS_SYMLINK);
}

static GList *get_child_folder_names(const char *parent_path) {
    GDir *dir = g_dir_open(parent_path, 0, NULL);
    if (!dir) return NULL;
    GList *names = NULL;
    const gchar *name;
    while ((name = g_dir_read_name(dir))) {
        gchar *child_path = g_build_filename(parent_path, name, NULL);
        if (is_real_directory(child_path))
            names = g_list_insert_sorted(names, g_strdup(name),
                (GCompareFunc)g_ascii_strcasecmp);
        g_free(child_path);
    }
    g_dir_close(dir);
    return names;
}

static gboolean search_text_matches(const char *text, const char *query_folded) {
    if (!text || !query_folded || !*query_folded) return FALSE;
    gchar *folded = g_utf8_casefold(text, -1);
    gboolean matches = folded && g_strstr_len(folded, -1, query_folded) != NULL;
    g_free(folded);
    return matches;
}

static gboolean search_file_content_matches(const char *path, const char *query_folded) {
    gchar *content = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &content, &length, NULL)) return FALSE;
    gboolean matches = FALSE;
    const char *header = "BA-RICH-V1\n";
    if (length > strlen(header) && g_str_has_prefix(content, header)) {
        const char *encoded = content + strlen(header);
        const char *newline = strchr(encoded, '\n');
        if (newline) {
            gchar *base64 = g_strndup(encoded, (gsize)(newline - encoded));
            gsize decoded_length = 0;
            guchar *decoded = g_base64_decode(base64, &decoded_length);
            if (decoded) {
                gchar *plain = g_strndup((const char *)decoded, decoded_length);
                matches = search_text_matches(plain, query_folded);
                g_free(plain);
                g_free(decoded);
            }
            g_free(base64);
        }
    } else {
        matches = search_text_matches(content, query_folded);
    }
    g_free(content);
    return matches;
}

struct search_context {
    const char *query_folded;
    GList *folders;
    struct note_entry *notes;
    size_t note_count;
};

static gboolean collect_search_results(struct search_context *ctx, const char *folder) {
    gchar *folder_path = build_folder_path(folder);
    if (!folder_path) return FALSE;
    GDir *dir = g_dir_open(folder_path, 0, NULL);
    if (!dir) {
        g_free(folder_path);
        return FALSE;
    }

    gboolean has_match = FALSE;
    const gchar *name;
    while ((name = g_dir_read_name(dir))) {
        gchar *child_path = g_build_filename(folder_path, name, NULL);
        if (is_real_directory(child_path)) {
            gchar *child_folder = (folder && *folder)
                ? g_build_filename(folder, name, NULL) : g_strdup(name);
            gboolean folder_name_matches = search_text_matches(name, ctx->query_folded);
            gboolean descendant_matches = collect_search_results(ctx, child_folder);
            if (folder_name_matches || descendant_matches) {
                ctx->folders = g_list_append(ctx->folders, g_strdup(child_folder));
                has_match = TRUE;
            }
            g_free(child_folder);
        } else if (g_file_test(child_path, G_FILE_TEST_IS_REGULAR)) {
            /* Dot-prefixed files are temporary/internal notes (for example
             * .untitled_<timestamp>) and must not appear in search results. */
            if (name[0] == '.') {
                g_free(child_path);
                continue;
            }
            const char *dot = strrchr(name, '.');
            size_t base_len = dot && strcmp(dot, ".txt") == 0
                ? (size_t)(dot - name) : strlen(name);
            gchar *title = g_strndup(name, base_len);
            gchar *relative = (folder && *folder)
                ? g_build_filename(folder, title, NULL) : g_strdup(title);
            gboolean title_matches = search_text_matches(relative, ctx->query_folded);
            if (title_matches || search_file_content_matches(child_path, ctx->query_folded)) {
                struct note_entry *new_notes = realloc(ctx->notes,
                    sizeof(*ctx->notes) * (ctx->note_count + 1));
                if (!new_notes) {
                    g_free(relative);
                    g_free(title);
                    g_free(child_path);
                    g_dir_close(dir);
                    g_free(folder_path);
                    return FALSE;
                }
                ctx->notes = new_notes;
                ctx->notes[ctx->note_count].filename = relative;
                ctx->notes[ctx->note_count].mtime = get_file_mtime(child_path);
                ctx->note_count++;
                has_match = TRUE;
            } else {
                g_free(relative);
            }
            g_free(title);
        }
        g_free(child_path);
    }
    g_dir_close(dir);
    g_free(folder_path);
    return has_match;
}

static int search_folder_cmp(const void *a, const void *b) {
    const char *aa = a;
    const char *bb = b;
    int depth_a = 0, depth_b = 0;
    for (const char *p = aa; *p; ++p) if (*p == '/') depth_a++;
    for (const char *p = bb; *p; ++p) if (*p == '/') depth_b++;
    if (depth_a != depth_b) return depth_a - depth_b;
    return g_ascii_strcasecmp(aa, bb);
}

static gchar *display_path_from_folder(const char *path, const char *base_folder) {
    if (base_folder && *base_folder) {
        gsize base_len = strlen(base_folder);
        if (g_str_has_prefix(path, base_folder) && path[base_len] == '/')
            return g_strdup(path + base_len + 1);
    }
    return g_strdup(path);
}

static void load_search_results(GtkListStore *store, const char *filter, const char *folder) {
    gchar *query_folded = g_utf8_casefold(filter, -1);
    struct search_context ctx = { query_folded, NULL, NULL, 0 };
    collect_search_results(&ctx, folder ? folder : "");
    ctx.folders = g_list_sort(ctx.folders, search_folder_cmp);

    if (folder && *folder) {
        gchar *parent = g_path_get_dirname(folder);
        if (g_strcmp0(parent, ".") == 0) {
            g_free(parent);
            parent = g_strdup("");
        }
        GtkTreeIter parent_iter;
        gtk_list_store_append(store, &parent_iter);
        gtk_list_store_set(store, &parent_iter,
            0, "..", 1, app_get_parent_icon(), 2, parent, 3, 2, -1);
        g_free(parent);
    }

    for (GList *item = ctx.folders; item; item = item->next) {
        char *path = item->data;
        gchar *display = display_path_from_folder(path, folder);
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
            0, display, 1, app_get_folder_icon(), 2, path, 3, 1, -1);
        g_free(display);
    }
    g_list_free_full(ctx.folders, g_free);

    if (ctx.note_count > 1)
        qsort(ctx.notes, ctx.note_count, sizeof(*ctx.notes), note_cmp);
    GdkPixbuf *trash = app_get_trash_icon();
    for (size_t i = 0; i < ctx.note_count; ++i) {
        gchar *display = display_path_from_folder(ctx.notes[i].filename, folder);
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
            0, display, 1, trash, 2, ctx.notes[i].filename, 3, 0, -1);
        g_free(display);
        free(ctx.notes[i].filename);
    }
    free(ctx.notes);
    g_free(query_folded);
}

int app_create_folder(const char *parent, const char *name) {
    if (!name || !*name || strchr(name, '/') || strchr(name, '\\') ||
        g_strcmp0(name, ".") == 0 || g_strcmp0(name, "..") == 0) return 0;
    if (parent && *parent && !valid_relative_path(parent)) return 0;
    gchar *relative = (parent && *parent) ? g_build_filename(parent, name, NULL) : g_strdup(name);
    gchar *path = build_folder_path(relative);
    gboolean ok = path && mkdir(path, 0700) == 0;
    g_free(path);
    g_free(relative);
    return ok ? 1 : 0;
}

static gboolean valid_folder_name(const char *name) {
    return name && *name && !strchr(name, '/') && !strchr(name, '\\') &&
        !strchr(name, '\n') && !strchr(name, '\r') &&
        g_strcmp0(name, ".") != 0 && g_strcmp0(name, "..") != 0;
}

int app_rename_folder(const char *folder, const char *new_name) {
    if (!folder || !*folder || !valid_relative_path(folder) || !valid_folder_name(new_name)) return 0;
    gchar *parent = g_path_get_dirname(folder);
    if (g_strcmp0(parent, ".") == 0) {
        g_free(parent);
        parent = g_strdup("");
    }
    gchar *target = (parent && *parent)
        ? g_build_filename(parent, new_name, NULL) : g_strdup(new_name);
    gchar *source_path = build_folder_path(folder);
    gchar *target_path = build_folder_path(target);
    int result = source_path && target_path &&
        is_real_directory(source_path) &&
        !g_file_test(target_path, G_FILE_TEST_EXISTS) &&
        rename(source_path, target_path) == 0;
    g_free(parent);
    g_free(target);
    g_free(source_path);
    g_free(target_path);
    return result;
}

int app_folder_is_empty(const char *folder) {
    if (!folder || !*folder || !valid_relative_path(folder)) return -1;
    gchar *path = build_folder_path(folder);
    if (!path) return -1;
    GDir *dir = g_dir_open(path, 0, NULL);
    if (!dir) {
        g_free(path);
        return -1;
    }
    const gchar *name = g_dir_read_name(dir);
    g_dir_close(dir);
    g_free(path);
    return name ? 0 : 1;
}

static gboolean delete_folder_contents(const char *path) {
    GDir *dir = g_dir_open(path, 0, NULL);
    if (!dir) return FALSE;
    gboolean ok = TRUE;
    const gchar *name;
    while (ok && (name = g_dir_read_name(dir))) {
        gchar *child = g_build_filename(path, name, NULL);
        gboolean is_real_dir = g_file_test(child, G_FILE_TEST_IS_DIR) &&
            !g_file_test(child, G_FILE_TEST_IS_SYMLINK);
        if (is_real_dir) ok = delete_folder_contents(child);
        else ok = unlink(child) == 0;
        g_free(child);
    }
    g_dir_close(dir);
    return ok && rmdir(path) == 0;
}

int app_delete_folder(const char *folder) {
    if (!folder || !*folder || !valid_relative_path(folder)) return 0;
    gchar *path = build_folder_path(folder);
    if (!path) return 0;
    int result = is_real_directory(path) && delete_folder_contents(path);
    g_free(path);
    return result;
}

void app_load_notes(GtkListStore *store, const char *filter, const char *folder) {
    gtk_list_store_clear(store);
    if (filter && *filter) {
        load_search_results(store, filter, folder);
        return;
    }
    char *notes_dir = build_folder_path(folder);
    if (!notes_dir) return;

    /* Navigation and folders are always shown before notes. */
    if (folder && *folder) {
        gchar *parent = g_path_get_dirname(folder);
        if (g_strcmp0(parent, ".") == 0) {
            g_free(parent);
            parent = g_strdup("");
        }
        GtkTreeIter parent_iter;
        gtk_list_store_append(store, &parent_iter);
        gtk_list_store_set(store, &parent_iter,
            0, "..",
            1, app_get_parent_icon(),
            2, parent,
            3, 2,
            -1);
        g_free(parent);
    }

    GList *child_folders = get_child_folder_names(notes_dir);
    GdkPixbuf *folder_icon = app_get_folder_icon();
    for (GList *item = child_folders; item; item = item->next) {
        const char *folder_name = item->data;
        gchar *relative = (folder && *folder)
            ? g_build_filename(folder, folder_name, NULL) : g_strdup(folder_name);
        GtkTreeIter folder_iter;
        gtk_list_store_append(store, &folder_iter);
        gtk_list_store_set(store, &folder_iter,
            0, folder_name,
            1, folder_icon,
            2, relative,
            3, 1,
            -1);
        g_free(relative);
    }
    g_list_free_full(child_folders, g_free);

    DIR *dir = opendir(notes_dir);
    if (!dir) { g_free(notes_dir); return; }
    struct dirent *entry;
    struct note_entry *notes = NULL;
    size_t count = 0;
    while ((entry = readdir(dir))) {
        gchar *entry_path = g_build_filename(notes_dir, entry->d_name, NULL);
        if (!g_file_test(entry_path, G_FILE_TEST_IS_REGULAR)) {
            g_free(entry_path);
            continue;
        }
        /* New notes are stored as hidden .untitled_* files until they get
         * their first real title. */
        if (entry->d_name[0] == '.') {
            g_free(entry_path);
            continue;
        }
        FILE *f = fopen(entry_path, "r");
        if (!f) {
            g_free(entry_path);
            continue;
        }
        fclose(f);
        struct note_entry *new_notes = realloc(notes, sizeof(*notes) * (count+1));
        if (!new_notes) {
            g_free(entry_path);
            for (size_t i = 0; i < count; ++i) free(notes[i].filename);
            free(notes);
            closedir(dir);
            g_free(notes_dir);
            return;
        }
        notes = new_notes;
        // Store name without .txt extension
        const char *dot = strrchr(entry->d_name, '.');
        size_t base_len = dot && strcmp(dot, ".txt") == 0 ? (size_t)(dot - entry->d_name) : strlen(entry->d_name);
        char *title = strndup(entry->d_name, base_len);
        notes[count].filename = (folder && *folder)
            ? g_build_filename(folder, title, NULL) : g_strdup(title);
        free(title);
        notes[count].mtime = get_file_mtime(entry_path);
        count++;
        g_free(entry_path);
    }
    closedir(dir);
    if (count > 1) qsort(notes, count, sizeof(*notes), note_cmp);
    GdkPixbuf *trash = app_get_trash_icon();
    for (size_t i = 0; i < count; ++i) {
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
            0, strrchr(notes[i].filename, '/') ? strrchr(notes[i].filename, '/') + 1 : notes[i].filename,
            1, trash,
            2, notes[i].filename,
            3, 0,
            -1);
        free(notes[i].filename);
    }
    free(notes);
    g_free(notes_dir);
}

void app_sort_notes(GtkListStore *store) {
    // Sorting handled in app_load_notes
}

// --- Window position settings ---
int app_read_window_position(int *x, int *y) {
    if (!x || !y) return 0;
    char *path = get_settings_path();
    if (!path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) { g_free(path); return 0; }
    char buf[256];
    int found_x = 0, found_y = 0;
    while (fgets(buf, sizeof(buf), f)) {
        size_t l = strlen(buf);
        while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r')) buf[--l] = '\0';
        char *s = buf;
        while (*s && isspace((unsigned char)*s)) s++;
        if (strncmp(s, "window_x=", 9) == 0) {
            *x = atoi(s + 9);
            found_x = 1;
        } else if (strncmp(s, "window_y=", 9) == 0) {
            *y = atoi(s + 9);
            found_y = 1;
        }
    }
    fclose(f);
    g_free(path);
    return (found_x && found_y) ? 1 : 0;
}

void app_save_window_position(int x, int y) {
    char *path = get_settings_path();
    if (!path) return;
    
    // Read existing settings
    int wrap = app_word_wrap_enabled;
    
    // Write all settings
    FILE *f = fopen(path, "w");
    if (!f) { g_free(path); return; }
    fprintf(f, "wrap=%d\n", wrap);
    fprintf(f, "window_x=%d\n", x);
    fprintf(f, "window_y=%d\n", y);
    fclose(f);
    g_free(path);
}



// Delete a note by title (without .txt). Returns 1 on success, 0 on error.
int app_delete_note(const char *title) {
    gchar *path = build_note_path(title);
    if (!path) return 0;
    int result = unlink(path) == 0;
    g_free(path);
    return result;
}

int app_note_exists(const char *title) {
    gchar *path = build_note_path(title);
    if (!path) return 0;
    int result = g_file_test(path, G_FILE_TEST_IS_REGULAR);
    g_free(path);
    return result;
}

int app_move_entry(const char *source, const char *destination_folder, gboolean is_folder) {
    if (!source || !*source || (destination_folder && !valid_relative_path(destination_folder))) return 0;
    if (!is_folder && !valid_relative_path(source)) return 0;
    if (is_folder && !valid_relative_path(source)) return 0;

    const char *destination = destination_folder ? destination_folder : "";
    if (is_folder && (g_strcmp0(source, destination) == 0 ||
        (g_str_has_prefix(destination, source) && destination[strlen(source)] == '/'))) {
        return 0;
    }

    gchar *source_path = is_folder ? build_folder_path(source) : build_note_path(source);
    gchar *base = g_path_get_basename(source);
    gchar *target_name = is_folder ? g_strdup(base) : g_strconcat(base, ".txt", NULL);
    gchar *destination_dir = build_folder_path(destination);
    gchar *destination_path = destination_dir
        ? g_build_filename(destination_dir, target_name, NULL) : NULL;
    int result = 0;

    if (source_path && destination_path && destination_dir &&
        g_file_test(source_path, G_FILE_TEST_EXISTS) &&
        is_real_directory(destination_dir) &&
        !g_file_test(destination_path, G_FILE_TEST_EXISTS) &&
        rename(source_path, destination_path) == 0) {
        result = 1;
    }

    g_free(source_path);
    g_free(base);
    g_free(target_name);
    g_free(destination_dir);
    g_free(destination_path);
    return result;
}

int app_read_note(const char *title, char **out) {
    if (!title || !out) return 0;
    gchar *path = build_note_path(title);
    if (!path) return 0;
    FILE *f = fopen(path, "r");
    g_free(path);
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
    gchar *path = build_note_path(title);
    if (!path) return 0;
    gchar *parent = g_path_get_dirname(path);
    g_mkdir_with_parents(parent, 0700);
    g_free(parent);
    FILE *f = fopen(path, "w");
    g_free(path);
    if (!f) return 0;
    if (content) fprintf(f, "%s", content);
    fclose(f);
    return 1;
}

int app_rename_note(const char *old_title, const char *new_title) {
    if (!old_title || !new_title) return 0;
    gchar *oldpath = build_note_path(old_title);
    gchar *newpath = build_note_path(new_title);
    if (!oldpath || !newpath) {
        g_free(oldpath);
        g_free(newpath);
        return 0;
    }
    int result = rename(oldpath, newpath) == 0;
    g_free(oldpath);
    g_free(newpath);
    return result;
}
