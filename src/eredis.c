#include "server.h"
#include "eredis.h"

/* Stuff from server.c */
void initServerConfig(void);
void initServer(void);

int eredis_init(void) {
    char hashseed[16];
    getRandomHexChars(hashseed,sizeof(hashseed));
    dictSetHashFunctionSeed((uint8_t*)hashseed);
    server.sentinel_mode = 0;
    initServerConfig();

    /* Override configuration */
    server.port = 0;            /* no tcp */
    server.unixsocket = NULL;   /* no unix domain */

    moduleInitModulesSystem();
    initServer();
    moduleLoadFromQueue();

    return 0;
}

eredis_client_t *eredis_create_client(void)
{
    client *c = createClient(-1);
    c->flags |= CLIENT_MODULE;      /* So we get replies even with fd == -1 */
    return (eredis_client_t *) c;
}

static void prepare_client(client *c, int args_count, const char **args)
{
    int i;
    if (c->argv) {
        for (i = 0; i < c->argc; i++) decrRefCount(c->argv[i]);
        zfree(c->argv);
    }

    c->argv = zmalloc(sizeof(robj *) * args_count);
    c->argc = args_count;
    for (i = 0; i < c->argc; i++) {
        c->argv[i] = createStringObject(args[i], strlen(args[i]));
    }
    c->bufpos = 0;
}

const char *eredis_execute(eredis_client_t *c, int args_count, const char **args, int *reply_len)
{
    client *rc = (client *) c;
    prepare_client(rc, args_count, args);
    if (processCommand(rc) != C_OK) return NULL;

    *reply_len = rc->bufpos;
    return rc->buf;
}

