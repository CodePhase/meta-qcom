/* SPDX-License-Identifier: MIT */

#ifndef _HELPERS_H
#define _HELPERS_H

#include <stdint.h>

int write_to(const char *path, const char *val, int flags);

void *gps_proxy();
void *rmnet_proxy(void *node_data);
uint32_t get_curr_timestamp();

#endif