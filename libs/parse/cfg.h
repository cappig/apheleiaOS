#pragma once


typedef void (*cfg_comp_fn)(char* key, char* value, void* data);

void parse_cfg(char* text, cfg_comp_fn comp, void* data);
