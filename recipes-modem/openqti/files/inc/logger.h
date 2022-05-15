// SPDX-License-Identifier: MIT
#ifndef _LOGGER_H_
#define _LOGGER_H_

#include "../inc/openqti.h"
#include <stdbool.h>
#include <stdio.h>

#define VOLATILE_LOGPATH "/var/log/openqti.log"
#define PERSISTENT_LOGPATH "/persist/openqti.log"

void reset_logtime();
double get_elapsed_time();
void logger(uint8_t level, char *format, ...);
void dump_packet(char *direction, uint8_t *buf, int pktsize);
uint8_t get_log_level();
void set_log_level(uint8_t level);
void set_log_method(bool ttyout);
void dump_pkt_raw(uint8_t *buf, int pktsize);
int mask_phone_number(char *orig, char *dest);
#endif