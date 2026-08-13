#include "config/config.h"
#include "core/server.h"
#include "leme-version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/util/log.h>

static void
leme_usage(FILE *stream)
{
    fputs("usage: leme\n"
        "       leme --config-check [PATH]\n"
        "       leme --help\n"
        "       leme --version\n"
        "       leme -v\n", stream);
}

int
main(int argc, char *argv[])
{
    struct leme_server server = {0};
    int status;

    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            leme_usage(stdout);
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[1], "--version") == 0 ||
                strcmp(argv[1], "-v") == 0) {
            if (argc != 2) {
                leme_usage(stderr);
                return 2;
            }
            if (printf("leme %s (%s)\n", LEME_VERSION,
                    LEME_VCS_TAG) < 0) {
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[1], "--config-check") == 0 ||
                strcmp(argv[1], "-c") == 0) {
            if (argc > 3) {
                leme_usage(stderr);
                return 2;
            }
            /*
             * Um caminho dado é usado tal como vem. Só a ausência dele
             * consulta leme_config_path, para que a verificação incida
             * sobre o ficheiro que a sessão carregaria.
             */
            return leme_config_check(argc == 3 ? argv[2] :
                leme_config_path(), stderr);
        }
        leme_usage(stderr);
        return 2;
    }

    wlr_log_init(WLR_INFO, NULL);

    if (!leme_server_init(&server)) {
        leme_server_finish(&server);
        return EXIT_FAILURE;
    }

    status = leme_server_run(&server);
    leme_server_finish(&server);
    return status;
}
