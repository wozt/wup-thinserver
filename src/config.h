#pragma once
#include "settings.h"

/* Validate an effective settings_t. Returns NULL when valid, otherwise a
   static English error string describing the first problem found. */
const char *config_validate(const settings_t *s);
