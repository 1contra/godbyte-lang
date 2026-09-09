/**
 * Copyright 2026 1contra
 *
 * Licensed under the GNU General Public License, Version 3
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.gnu.org/licenses/gpl-3.0.html
 */

#include "server.hpp"

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    gbpp::lsp::LSPServer server;
    server.run();
    return 0;
}