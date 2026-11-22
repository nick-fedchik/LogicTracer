# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk

"""A desktop that cached hicolor before installation must find the new launcher icon."""

import configparser
import os
from pathlib import Path
import subprocess
import sys
import tempfile

try:
    import gi
    gi.require_version("Gtk", "3.0")
    from gi.repository import Gtk
except (ImportError, ValueError):
    print("GTK 3 icon-theme introspection unavailable")
    sys.exit(77)


def main(cmake, gui_build, datadir, config):
    with tempfile.TemporaryDirectory() as tmp:
        prefix = Path("/opt/logictracer-test")
        data = Path(tmp) / (prefix / datadir).relative_to("/")
        icons = data / "icons"
        hicolor = icons / "hicolor"
        (hicolor / "scalable/apps").mkdir(parents=True)
        (hicolor / "index.theme").write_text(
            "[Icon Theme]\nName=hicolor\nDirectories=scalable/apps\n"
            "[scalable/apps]\nSize=48\nType=Scalable\nContext=Applications\n"
        )
        os.utime(hicolor, (1, 1))  # GNOME compares theme timestamps in whole seconds.
        theme = Gtk.IconTheme.new()
        theme.set_search_path([str(icons)])
        theme.set_custom_theme("hicolor")
        assert not theme.has_icon("logictracer")

        subprocess.run([cmake, "--install", gui_build, "--prefix", str(prefix), "--config", config],
                       env=dict(os.environ, DESTDIR=tmp), check=True)
        launcher = configparser.ConfigParser(interpolation=None)
        launcher.read(data / "applications/logictracer-gui.desktop")
        theme.rescan_if_needed()
        icon = theme.lookup_icon(launcher["Desktop Entry"]["Icon"], 48, Gtk.IconLookupFlags(0))
        assert icon and icon.get_filename() == str(hicolor / "scalable/apps/logictracer.svg"), \
            "running desktop still shows a fallback icon"


if __name__ == "__main__":
    main(*sys.argv[1:])
