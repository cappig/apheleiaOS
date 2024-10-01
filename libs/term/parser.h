#pragma once

#include <base/types.h>

#include "term.h"


// The parser is just a state machine that parses the string char by char
// Style information is stroed in the terminal struct
char parse_ansi_char(terminal* term, const char* ch_ptr);

int term_parse(terminal* term, const char* string, usize max_size);
