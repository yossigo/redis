#ifndef _EREDIS_H
#define _EREDIS_H

typedef struct eredis_client eredis_client_t;

/* Initialize the eredis library - must be done once per process! */
int eredis_init(void);

/* Client create/free */
eredis_client_t *eredis_create_client(void);
void eredis_free_client(eredis_client_t *c);

/* Prepare a request, before calling execute. If arg_lens is NULL, strings are
 * processed as null terminated. Otherwise lengths are taken from the separate
 * size_t array.
 */
int eredis_prepare_request(eredis_client_t *c, int args_count, const char **args, size_t *arg_lens);

/* Execute request; returns 0 on success. */
int eredis_execute(eredis_client_t *c);

/* Read chunk from reply. Chunking depends on internal Redis representation
 * so make no assumptions.
 *
 * Every call returns a pointer and updates chunk_len. When no more chunks
 * are available, NULL is returned.
 */
const char *eredis_read_reply_chunk(eredis_client_t *c, int *chunk_len);


#endif  /* _EREDIS_H */
