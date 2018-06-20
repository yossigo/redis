#ifndef _EREDIS_H
#define _EREDIS_H

typedef struct eredis_client eredis_client_t;

int eredis_init(void);
eredis_client_t *eredis_create_client(void);
int eredis_prepare_request(eredis_client_t *c, int args_count, const char **args);
const char *eredis_execute(eredis_client_t *c, int *reply_len);


#endif  /* _EREDIS_H */
