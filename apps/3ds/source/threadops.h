#pragma once

#include "client.h"

void client_fork_thread(Client *c, const T3Thread *t, const char *title);
void client_load_launch_refs(Client *c, const Project *p);
void client_load_diff(Client *c, const T3Thread *t);
void client_close_diff(Client *c);

// Shared RPC plumbing stays in client.c; feature responses live here.
int client_rpc_send(Client *c, const char *tag, const char *payload, int kind, const char *done_label);
bool threadops_success(Client *c, const PendingRpc *rpc, JsonCur *value);
bool threadops_failure(Client *c, const PendingRpc *rpc, const char *message);
void threadops_reset(Client *c);
void threadops_connection_lost(Client *c);
