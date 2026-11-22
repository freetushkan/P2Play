#pragma once

// The web interface: the Silo plugin's pages (plugins/mediamesh), built into the core
// (cmake/embed-ui.cmake) and served at /ui/. One source: the plugin and the core host the same files.

#include <cstddef>
#include <string>

namespace p2p {

    struct UiFile {
        const char *path;   // "/index.html"
        const char *type;
        const unsigned char *data;
        size_t size;
    };

    // nullptr: no such file
    const UiFile *uiFile(const std::string &path);

}
