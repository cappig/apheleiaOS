#pragma once

typedef void (*cfg_handler_fn)(char* value, void* data);

typedef struct {
    const char* key;
    cfg_handler_fn handler;
} cfg_entry_t;


void parse_cfg(char* text, const cfg_entry_t* table, void* data);
