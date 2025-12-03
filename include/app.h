#ifndef APP_H
#define APP_H

#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>

#define CONFIG_DIR ".config/baNotes"
#define NOTES_SUBDIR "notes"

void app_init_config_dirs(void);
void app_load_notes(GtkListStore *store, const char *filter);
void app_sort_notes(GtkListStore *store);
GdkPixbuf *app_get_eye_icon(void);
GdkPixbuf *app_get_trash_icon(void);

// Delete a note by title (without .txt). Returns 1 on success, 0 on error.
int app_delete_note(const char *title);

// Note I/O and rename
// Reads the entire note into a malloc'd buffer (sets *out). Returns 1 on success.
int app_read_note(const char *title, char **out);
// Writes content to the note file (creates or overwrites). Returns 1 on success.
int app_write_note(const char *title, const char *content);
// Renames a note from old_title -> new_title (without .txt). Returns 1 on success.
int app_rename_note(const char *old_title, const char *new_title);

// Window position settings
int app_read_window_position(int *x, int *y);
void app_save_window_position(int x, int y);

// Word wrap setting persistence
// Reads the word-wrap setting into *enabled (1 = enabled, 0 = disabled).
// Returns 1 if setting read successfully, 0 otherwise (e.g. file missing).
int app_read_word_wrap(int *enabled);
// Save the word-wrap setting (1 = enabled, 0 = disabled).
void app_save_word_wrap(int enabled);

// Force update of all registered textviews (wrap mode and labels). Can be called after toggling or on load.
void app_update_all_textviews_wrap(void);
// Toggle the global word-wrap setting and update all textviews (also used by UI elements)
void app_toggle_word_wrap(void);

// Hook for keypress events on textviews: Ctrl+W toggles word-wrap
gboolean app_textview_keypress(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
// Return the current word-wrap setting (1 = enabled, 0 = disabled)
int app_get_word_wrap(void);
// Register/unregister textviews for global updates;
// label can be NULL (no status label). Register returns 1 on success.
int app_register_textview(GtkWidget *tview, GtkWidget *label);
void app_unregister_textview(GtkWidget *tview);

// Rich-text helpers: load a note into a GtkTextBuffer (decodes custom format if present)
// and serialize a buffer into the custom format (returns a newly allocated string).
// Returned string must be freed with g_free.
int app_load_note_into_buffer(const char *title, GtkTextBuffer *buffer);
char *app_serialize_buffer_rich(GtkTextBuffer *buffer);
// Parse a serialized rich string (BA-RICH-V1 or plain text) into a text buffer.
// Returns 1 on success, 0 on error.
int app_parse_rich_string_into_buffer(const char *serialized, GtkTextBuffer *buffer);

#endif // APP_H
