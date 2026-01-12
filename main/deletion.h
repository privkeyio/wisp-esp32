#ifndef DELETION_H
#define DELETION_H

#include "nostr_relay_protocol.h"

#define NOSTR_KIND_DELETION 5

typedef struct storage_engine storage_engine_t;

int deletion_process(storage_engine_t *storage, const nostr_event *delete_event);

#endif
