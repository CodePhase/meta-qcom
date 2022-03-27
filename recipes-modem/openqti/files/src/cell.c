// SPDX-License-Identifier: MIT

#include "../inc/cell.h"
#include "../inc/devices.h"
#include "../inc/helpers.h"
#include "../inc/logger.h"
#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_RESPONSE_SZ 4096
/*
 * cell data
 *  We're going to exploit Quectel's engineering commands
 *  to track signal status, network mode and servicing and
 *  neighbour cells
 *
 *  If we get a sudden change in neighbour cells, service drop
 *  etc. we should be able to track it down here
 */

struct network_state net_status;
struct cell_report current_report;
/*
Return last reported network type
0x00 -> No service
0x01 -> CDMA
0x02 -> CDMA EVDO
0x03 -> AMPS
0x04 -> GSM
0x05 -> UMTS
0x06 ??
0x07 ??
0x08 LTE
*/

uint8_t get_network_type() { return net_status.network_type; }

/* Returns last reported signal in %, based on signal bars */
uint8_t get_signal_strength() { 
  if (net_status.signal_bars > 0) {
    return net_status.signal_bars * 5 /100; 
  }
  
  return 0;
  }

struct network_state get_network_status() {
  return net_status;
}

struct cell_report get_current_cell_report() {
  return current_report;
}

struct cell_report parse_report_data(char *orig_string) {
  struct cell_report report;
  char delim[]=",";
  char str[MAX_RESPONSE_SZ];
  int ret;
  strcpy(str, (char *)orig_string);
  int init_size = strlen(str);
  int positions[128];
  char slices[64][MAX_RESPONSE_SZ];
  int cur_word = 1;
  int cur_word_id = -1;
  int start;
  int end;
  positions[0] = 0;
  char *ptr = strtok(str, delim);
  while (ptr != NULL) {
    // logger(MSG_INFO, "'%s: %s'\n", __func__, ptr);
    ptr = strtok(NULL, delim);
  }

  for (int i = 0; i < init_size; i++) {
    if (str[i] == 0) {
      positions[cur_word] = i;
      cur_word++;
    }
  }
  logger(MSG_INFO, "Total pieces: %i\n", cur_word);
  for (int i = 0; i < cur_word; i++) {
    start = positions[i];
    if (i + 1 >= cur_word) {
      end = init_size;
    } else {
      end = positions[i + 1];
    }
    if (i > 0) {
      start++;
    }
    memset(slices[i], 0, MAX_RESPONSE_SZ);
    memcpy(slices[i], str + start, (end - start));
    if (strcmp(slices[i], "-") == 0) {
      strncpy(slices[i], "-999", strlen("-999")); 
      // If it is empty set it to -999 so we can use that info later
      // Otherwise strtol will convert "-" to 9
    }

    slices[i][strlen(slices[i])] = '\0';
    logger(MSG_WARN, "%s: Current word position %i: %s\n", __func__, i, slices[i]);
    /*
    Now, go filling the blanks by position */
  }
  report.net_type = -1;
  if (strstr(orig_string, "GSM") != NULL) {
    logger(MSG_INFO, "%s GSM network data report\n", __func__);
    report.net_type = 0;
    report.mcc = strtol(slices[3], NULL, 10);
    report.mnc = strtol(slices[4], NULL, 10);
    strncpy(report.gsm.lac, slices[5], strlen(slices[5]));
    strncpy(report.cell_id, slices[6], strlen(slices[6]));
    report.gsm.bsic = strtol(slices[7], NULL, 10);
    report.gsm.arfcn = strtol(slices[8], NULL, 10);
    report.gsm.band = strtol(slices[9], NULL, 10);
    report.gsm.rxlev = strtol(slices[10], NULL, 10);
    report.gsm.txp = strtol(slices[11], NULL, 10);
    report.gsm.rla = strtol(slices[12], NULL, 10);
    report.gsm.drx = strtol(slices[13], NULL, 10);
    report.gsm.c1 = strtol(slices[14], NULL, 10);
    report.gsm.c2 = strtol(slices[15], NULL, 10);
    report.gsm.gprs = strtol(slices[16], NULL, 10);
    report.gsm.tch = strtol(slices[17], NULL, 10);
    report.gsm.ts = strtol(slices[18], NULL, 10);
    report.gsm.ta = strtol(slices[19], NULL, 10);
    report.gsm.maio = strtol(slices[20], NULL, 10);
    report.gsm.hsn = strtol(slices[21], NULL, 10);
    report.gsm.rxlevsub = strtol(slices[22], NULL, 10);
    report.gsm.rxlevfull = strtol(slices[23], NULL, 10);
    report.gsm.rxqualsub = strtol(slices[24], NULL, 10);
    report.gsm.rxqualfull = strtol(slices[25], NULL, 10);
    report.gsm.voicecodec = strtol(slices[26], NULL, 10);

  } else if (strstr(orig_string, "WCDMA") != NULL) {
    logger(MSG_INFO, "%s WCDMA network data report\n", __func__);
    report.net_type = 1;
    report.mcc = strtol(slices[3], NULL, 10);
    report.mnc = strtol(slices[4], NULL, 10);
    strncpy(report.wcdma.lac, slices[5], strlen(slices[5]));
    strncpy(report.cell_id, slices[6], strlen(slices[6]));
    report.wcdma.uarfcn = strtol(slices[7], NULL, 10);
    report.wcdma.psc = strtol(slices[8], NULL, 10);
    report.wcdma.rac = strtol(slices[9], NULL, 10);
    report.wcdma.rscp = strtol(slices[10], NULL, 10);
    report.wcdma.ecio = strtol(slices[11], NULL, 10);
    report.wcdma.phych = strtol(slices[12], NULL, 10);
    report.wcdma.sf = strtol(slices[13], NULL, 10);
    report.wcdma.slot = strtol(slices[14], NULL, 10);
    report.wcdma.speech_codec = strtol(slices[15], NULL, 10);
    report.wcdma.conmod = strtol(slices[16], NULL, 10);

  } else if (strstr(orig_string, "LTE") != NULL) {
    logger(MSG_INFO, "%s LTE network data report\n", __func__);
    report.net_type = 2;
    report.lte.is_tdd = strtol(slices[3], NULL, 10);
    report.mcc = strtol(slices[4], NULL, 10);
    report.mnc = strtol(slices[5], NULL, 10);
    strncpy(report.cell_id, slices[6], strlen(slices[6]));
    report.lte.pcid = strtol(slices[7], NULL, 10);
    report.lte.earfcn = strtol(slices[8], NULL, 10);
    report.lte.freq_band_ind = strtol(slices[9], NULL, 10);
    report.lte.ul_bandwidth = strtol(slices[10], NULL, 10);
    report.lte.dl_bandwidth = strtol(slices[11], NULL, 10);
    report.lte.tac = strtol(slices[12], NULL, 10);
    report.lte.rsrp = strtol(slices[13], NULL, 10);
    report.lte.rsrq = strtol(slices[14], NULL, 10);
    report.lte.rssi = strtol(slices[15], NULL, 10);
    report.lte.sinr = strtol(slices[16], NULL, 10);
    report.lte.srxlev = strtol(slices[17], NULL, 10);
    
  } else {
    logger(MSG_ERROR, "%s Unknown data: %s\n", __func__, orig_string);

  } 

  return report;
}
/* Connect to the AT port, send a command, and get a response */
int get_data_from_command(char *command, size_t len, char *expected_response,
                          char *response) {
  int fd, ret;
  int fnret;
  fd_set readfds;
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 500000;
  fd = open(SMD_SEC_AT, O_RDWR);
  if (fd < 0) {
    logger(MSG_ERROR, "%s: Cannot open SMD10 entry\n", __func__);
    return -EINVAL;
  }
  ret = write(fd, command, len);
  FD_SET(fd, &readfds);
  ret = select(MAX_FD, &readfds, NULL, NULL, &tv);
  if (FD_ISSET(fd, &readfds)) {
    ret = read(fd, response, MAX_RESPONSE_SZ);
    if (strstr(response, expected_response) != NULL) {
      logger(MSG_INFO, "%s->%s We got a match: %s\n", __func__, command,
             response);
      fnret = 0;
    }
  } else {
    logger(MSG_ERROR, "%s: Missed our window\n", __func__);
    fnret = -EBADMSG;
  }

  close(fd);
  return fnret;
}


void read_serving_cell() {
  int ret = 0; 
  char *pt;
  char *response;
  response = malloc(MAX_RESPONSE_SZ * sizeof(char));
  int command_length = strlen(GET_SERVING_CELL);

  logger(MSG_INFO, "%s: Read serving cell start\n", __func__);
  ret = get_data_from_command(GET_SERVING_CELL, command_length,
                              GET_QENG_RESPONSE_PROTO, response);
  if (ret != 0) {
    logger(MSG_ERROR, "%s: Command %s failed. Response: %s\n", __func__,
           GET_SERVING_CELL, response);
  } else {
    logger(MSG_INFO, "%s: Command %s succeeded! Response: %s\n", __func__,
           GET_SERVING_CELL, response);
    if (strlen(response) > 18) {
      current_report = parse_report_data(response);
    }
  }
  logger(MSG_INFO, "%s: EXIT!\n", __func__);
  free(response);
  response = NULL;
}

void read_at_cind() {
  char *response;
  response = malloc(MAX_RESPONSE_SZ * sizeof(char));
  int command_length = strlen(GET_COMMON_IND);
  int ret = 0;
  logger(MSG_INFO, "%s: Read CIND start\n", __func__);

  ret = get_data_from_command(GET_COMMON_IND, command_length,
                              GET_COMMON_IND_RESPONSE_PROTO, response);

  if (ret != 0) {
    logger(MSG_ERROR, "%s: Command %s failed. Response: %s\n", __func__,
           GET_COMMON_IND, response);
  } else {
    logger(MSG_INFO, "%s: Command %s succeeded! Response: %s\n", __func__,
           GET_COMMON_IND, response);
    if (strlen(response) > 18) {
      net_status.signal_bars = get_int_from_str(response, 11);
      net_status.in_service = get_int_from_str(response, 13);
      net_status.in_call = get_int_from_str(response, 15);
      net_status.is_roaming = get_int_from_str(response, 17);
      net_status.ps_domain = get_int_from_str(response, 21);
    }
  }
  logger(MSG_INFO, "%s: EXIT!\n", __func__);
  free(response);
  response = NULL;
}

void update_network_data(uint8_t network_type, uint8_t signal_level) {
  logger(MSG_INFO, "%s: update network data\n", __func__);
  net_status.network_type = network_type;
  net_status.signal_level = signal_level;
  logger(MSG_INFO, "%s: read cind\n", __func__);
  read_at_cind();
  logger(MSG_INFO, "%s: read serving cell\n", __func__);
  read_serving_cell();
  logger(MSG_INFO, "%s: end\n", __func__);

}