# baNotes

Lightweight GTK note-taking application. Stores notes as text files in a user configuration directory and exposes a tray indicator for quick access.

Build and install

Dependencies (typical on Debian/Ubuntu):

- `libgtk-3-dev`
- `libayatana-appindicator3-dev` (or distribution-specific AppIndicator dev package)
- `pkg-config`

To build and install:

```bash
make
sudo make install
```

Usage

- Run `baNotes` after installation or start from your desktop environment. The app stores notes as simple `.txt` files.

License

This project is licensed under the MIT License. See `LICENSE` for details.
