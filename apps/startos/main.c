// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "installation.h"
#include "local_http.h"
#include "network.h"
#include "platform.h"
#include "profile_store.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

void web_start(void);

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--health")) {
        static const char request[] = "GET /healthz HTTP/1.0\r\nHost: localhost\r\n\r\n";
        return local_http_response("127.0.0.1", 8080, request, sizeof(request) - 1, NULL, 0, false) == 200 ? 0 : 1;
    }
    const bool initialize = argc == 3 && !strcmp(argv[2], "--init");
    char *end = NULL;
    const unsigned long port = argc == 3 && !initialize ? strtoul(argv[2], &end, 10) : 8080;
    if (argc < 2 || argc > 3 || (end && (*end || end == argv[2])) || !port || port > 65535) {
        fputs("Usage: dispatch-preview DATA_DIRECTORY [PORT | --init]\n", stderr);
        return 2;
    }
    umask(0077);
    if (!preview_storage_open(argv[1])) {
        fputs("Could not open or lock the DISPATCH data volume\n", stderr);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    network_start();
    if (initialize) return 0;
    if (!preview_http_port(port)) return 1;
    profile_store_start();
    installation_start();
    web_start();
    const int status = preview_http_run();
    installation_checkpoint();
    return status;
}
