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

struct eredis_client {
    client *client;
    size_t reply_bytes_read;
    listIter reply_iter;
};

eredis_client_t *eredis_create_client(void)
{
    eredis_client_t *c = zmalloc(sizeof(eredis_client_t));
    c->client = createClient(-1);
    c->client->flags |= CLIENT_MODULE;      /* So we get replies even with fd == -1 */
    return c;
}

void eredis_free_client(eredis_client_t *c)
{
    freeClient(c->client);
    zfree(c);
}


int eredis_prepare_request(eredis_client_t *c, int args_count, const char **args, size_t *arg_lens)
{
    client *rc = c->client;
    int i;
    if (rc->argv) {
        for (i = 0; i < rc->argc; i++) decrRefCount(rc->argv[i]);
        zfree(rc->argv);
    }

    rc->argv = zmalloc(sizeof(robj *) * args_count);
    rc->argc = args_count;
    for (i = 0; i < rc->argc; i++) {
        size_t len = arg_lens ? arg_lens[i] : strlen(args[i]);
        rc->argv[i] = createStringObject(args[i], len);
    }
    rc->bufpos = 0;
    c->reply_bytes_read = 0;

    return 0;
}

int eredis_execute(eredis_client_t *c)
{
    client *rc = c->client;
    if (processCommand(rc) != C_OK) return -1;

    return 0;
}

const char *eredis_read_reply_chunk(eredis_client_t *c, int *chunk_len)
{
    if (!c->reply_bytes_read) {
        listRewind(c->client->reply, &c->reply_iter);
        c->reply_bytes_read = c->client->bufpos;
        *chunk_len = c->client->bufpos;
        return c->client->buf;
    } else {
        listNode *ln = listNext(&c->reply_iter);
        if (ln) {
            sds val = listNodeValue(ln);
            *chunk_len = sdslen(val);
            return val;
        } else {
            return NULL;
        }
    }
}

