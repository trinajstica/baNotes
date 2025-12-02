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

#endif // APP_H
