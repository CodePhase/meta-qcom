// SPDX-License-Identifier: MIT

#include "../inc/command.h"
#include "../inc/call.h"
#include "../inc/cell.h"
#include "../inc/logger.h"
#include "../inc/proxy.h"
#include "../inc/sms.h"
#include "../inc/tracking.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct {
  bool is_unlocked;
  uint32_t unlock_time;
  uint8_t cmd_history[1024];
  uint16_t cmd_position;
  uint32_t last_cmd_timestamp;
  char user_name[32];
  char bot_name[32];
} cmd_runtime;

char *get_rt_modem_name() { return cmd_runtime.bot_name; }

char *get_rt_user_name() { return cmd_runtime.user_name; }

void add_to_history(uint8_t command_id) {
  if (cmd_runtime.cmd_position >= 1023) {
    cmd_runtime.cmd_position = 0;
  }
  cmd_runtime.cmd_history[cmd_runtime.cmd_position] = command_id;
  cmd_runtime.cmd_position++;
}

uint16_t find_cmd_history_match(uint8_t command_id) {
  uint16_t match = 0;
  uint16_t countdown;
  if (cmd_runtime.cmd_position > 5) {
    countdown = cmd_runtime.cmd_position - 5;
    for (uint16_t i = cmd_runtime.cmd_position; i >= countdown; i--) {
      if (cmd_runtime.cmd_history[i] == command_id) {
        match++;
      }
    }
  }
  return match;
}

void get_names() {
  get_modem_name(cmd_runtime.bot_name);
  get_user_name(cmd_runtime.user_name);
}
void set_cmd_runtime_defaults() {
  cmd_runtime.is_unlocked = false;
  cmd_runtime.unlock_time = 0;
  strncpy(cmd_runtime.user_name, "User",
          32); // FIXME: Allow user to set a custom name
  strncpy(cmd_runtime.bot_name, "Modem",
          32); // FIXME: Allow to change modem name
  get_names();
}

int get_uptime(uint8_t *output) {
  unsigned updays, uphours, upminutes;
  struct sysinfo info;
  struct tm *current_time;
  time_t current_secs;
  int bytes_written = 0;
  time(&current_secs);
  current_time = localtime(&current_secs);

  sysinfo(&info);

  bytes_written = snprintf((char *)output, MAX_MESSAGE_SIZE,
                           "%02u:%02u:%02u up ", current_time->tm_hour,
                           current_time->tm_min, current_time->tm_sec);
  updays = (unsigned)info.uptime / (unsigned)(60 * 60 * 24);
  if (updays)
    bytes_written += snprintf((char *)output + bytes_written,
                              MAX_MESSAGE_SIZE - bytes_written, "%u day%s, ",
                              updays, (updays != 1) ? "s" : "");
  upminutes = (unsigned)info.uptime / (unsigned)60;
  uphours = (upminutes / (unsigned)60) % (unsigned)24;
  upminutes %= 60;
  if (uphours)
    bytes_written += snprintf((char *)output + bytes_written,
                              MAX_MESSAGE_SIZE - bytes_written, "%2u:%02u",
                              uphours, upminutes);
  else
    bytes_written +=
        snprintf((char *)output + bytes_written,
                 MAX_MESSAGE_SIZE - bytes_written, "%u min", upminutes);

  return 0;
}

int get_load_avg(uint8_t *output) {
  int fd;
  fd = open("/proc/loadavg", O_RDONLY);
  if (fd < 0) {
    logger(MSG_ERROR, "%s: Cannot open load average \n", __func__);
    return 0;
  }
  lseek(fd, 0, SEEK_SET);
  if (read(fd, output, 64) <= 0) {
    logger(MSG_ERROR, "%s: Error reading PROCFS entry \n", __func__);
    close(fd);
    return 0;
  }

  close(fd);
  return 0;
}

int get_memory(uint8_t *output) {
  struct sysinfo info;
  sysinfo(&info);

  snprintf((char *)output, MAX_MESSAGE_SIZE,
           "Total:%luM\nFree:%luM\nShared:%luK\nBuffer:%luK\nProcs:%i\n",
           (info.totalram / 1024 / 1024), (info.freeram / 1024 / 1024),
           (info.sharedram / 1024), (info.bufferram / 1024), info.procs);

  return 0;
}
void set_custom_modem_name(uint8_t *command) {
  int strsz = 0;
  uint8_t *offset;
  uint8_t *reply = calloc(256, sizeof(unsigned char));
  char name[32];
  offset = (uint8_t *)strstr((char *)command, partial_commands[0].cmd);
  if (offset == NULL) {
    strsz = snprintf((char *)reply, MAX_MESSAGE_SIZE - strsz,
                     "Error setting my new name\n");
  } else {
    int ofs = (int)(offset - command) + strlen(partial_commands[0].cmd);
    if (strlen((char *)command) > ofs) {
      snprintf(name, 32, "%s", (char *)command + ofs);
      strsz = snprintf((char *)reply, MAX_MESSAGE_SIZE, "My name is now %s\n",
                       name);
      set_modem_name(name);
      get_names();
    }
  }
  add_message_to_queue(reply, strsz);
  free(reply);
  reply = NULL;
}

void set_custom_user_name(uint8_t *command) {
  int strsz = 0;
  uint8_t *offset;
  uint8_t *reply = calloc(256, sizeof(unsigned char));
  char name[32];
  offset = (uint8_t *)strstr((char *)command, partial_commands[1].cmd);
  if (offset == NULL) {
    strsz = snprintf((char *)reply, MAX_MESSAGE_SIZE - strsz,
                     "Error setting your new name\n");
  } else {
    int ofs = (int)(offset - command) + strlen(partial_commands[1].cmd);
    if (strlen((char *)command) > ofs) {
      snprintf(name, 32, "%s", (char *)command + ofs);
      strsz = snprintf((char *)reply, MAX_MESSAGE_SIZE,
                       "I will call you %s from now on\n", name);
      set_user_name(name);
      get_names();
    }
  }
  add_message_to_queue(reply, strsz);
  free(reply);
  reply = NULL;
}

void debug_cb_message(uint8_t *command) {
  int strsz = 0;
  uint8_t *reply = calloc(256, sizeof(unsigned char));
  uint8_t example_pkt[] = {
      0x01, 0x71, 0x00, 0x80, 0x05, 0x01, 0x04, 0x08, 0x00, 0x01, 0x00, 0x65,
      0x00, 0x11, 0x5E, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x56, 0x00,
      0x67, 0x60, 0x11, 0x12, 0x0F, 0x66, 0xF2, 0x37, 0xBD, 0x70, 0x2E, 0xCB,
      0x5D, 0x20, 0xE8, 0xBB, 0x2E, 0x07, 0x95, 0xDD, 0xA0, 0x79, 0xD8, 0xFE,
      0x4E, 0xCB, 0x41, 0x70, 0x76, 0x7D, 0x0E, 0x9A, 0xD7, 0xE5, 0x20, 0x76,
      0x79, 0x0E, 0x6A, 0x97, 0xE7, 0xF3, 0xF0, 0xB9, 0x3C, 0x07, 0x91, 0x4F,
      0x61, 0x76, 0x59, 0x4E, 0x2F, 0xB3, 0x40, 0xF6, 0x72, 0x3D, 0xCD, 0x66,
      0x97, 0xF5, 0xA0, 0xF1, 0xDB, 0x3D, 0xAF, 0xB3, 0xE9, 0x65, 0x39, 0xE8,
      0x7E, 0xBF, 0xBB, 0xCA, 0xEE, 0x30, 0xBB, 0x2C, 0xA7, 0x97, 0x5D, 0xE3,
      0x77, 0xBB, 0x16, 0x01, 0x00, 0x00};

  strsz = snprintf((char *)reply, MAX_MESSAGE_SIZE, "Dummy CB Message parse\n");
  add_message_to_queue(reply, strsz);
  check_cb_message(example_pkt, sizeof(example_pkt), 0, 0);
  free(reply);
  reply = NULL;
}

void dump_signal_report() {
  int strsz = 0;
  uint8_t *reply = calloc(256, sizeof(unsigned char));
  struct cell_report report = get_current_cell_report();
  if (report.net_type < 0 ) {
    strsz = snprintf((char *)reply, MAX_MESSAGE_SIZE, "Serving cell report has not been retrieved yet or is invalid\n");
  } else {
    switch (report.net_type) {
      case 0: // GSM
        strsz = snprintf((char *)reply, MAX_MESSAGE_SIZE, "GSM Report: %i-%i\n", report.mcc, report.mnc);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "Cell: %s\n", report.cell_id);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "lac %s\n", report.gsm.lac);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "arfcn %i\n", report.gsm.arfcn);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "band %i\n", report.gsm.band);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "rxlev %i\n", report.gsm.rxlev);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "txp %i\n", report.gsm.txp);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "rla %i\n", report.gsm.rla);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "drx %i\n", report.gsm.drx);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "c1 %i\n", report.gsm.c1);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "c2 %i\n", report.gsm.c2);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "gprs %i\n", report.gsm.gprs);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "tch %i\n", report.gsm.tch);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "ts %i\n", report.gsm.ts);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "ta %i\n", report.gsm.ta);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "maio %i\n", report.gsm.maio);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "hsn %i\n", report.gsm.hsn);
        add_message_to_queue(reply, strsz);
        memset(reply, 0, MAX_MESSAGE_SIZE);
        strsz = snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "rxlevsub %i\n", report.gsm.rxlevsub);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "rxlevfull %i\n", report.gsm.rxlevfull);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "rxqualsub %i\n", report.gsm.rxqualsub);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "rxqualfull %i\n", report.gsm.rxqualfull);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "voicecodec %i\n", report.gsm.voicecodec);

        break;

      case 1: // WCDMA
        strsz = snprintf((char *)reply, MAX_MESSAGE_SIZE, "WCDMA Report: %i-%i\n", report.mcc, report.mnc);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "Cell: %s\n", report.cell_id);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "lac %s\n", report.wcdma.lac);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "uarfcn %i\n", report.wcdma.uarfcn);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "psc %i\n", report.wcdma.psc);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "rac %i\n", report.wcdma.rac);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "rscp %i\n", report.wcdma.rscp);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "ecio %i\n", report.wcdma.ecio);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "phych %i\n", report.wcdma.phych);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "sf %i\n", report.wcdma.sf);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "slot %i\n", report.wcdma.slot);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "speech codec %i\n", report.wcdma.speech_codec);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "conmod %i\n", report.wcdma.conmod);

        break;

      case 2: // LTE
        strsz = snprintf((char *)reply, MAX_MESSAGE_SIZE, "LTE Report: %i-%i\n", report.mcc, report.mnc);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "Cell: %s\n", report.cell_id);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "is_tdd %i\n", report.lte.is_tdd);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "pcid %i\n", report.lte.pcid);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "earfcn %i\n", report.lte.earfcn);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "freq band ind %i\n", report.lte.freq_band_ind);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "ul bw %i\n", report.lte.ul_bandwidth);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "dl bw %i\n", report.lte.dl_bandwidth);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "tac %i\n", report.lte.tac);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "rsrp %i\n", report.lte.rsrp);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "rsrq %i\n", report.lte.rsrq);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "rssi %i\n", report.lte.rssi);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "sinr %i\n", report.lte.sinr);
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "srxlev %i\n", report.lte.srxlev);
        break;

      default:
        strsz = snprintf((char *)reply, MAX_MESSAGE_SIZE, "Serving cell report has not been retrieved yet or is invalid\n");
        break;
    }

  }
  add_message_to_queue(reply, strsz);

  free(reply);
  reply = NULL;
}

void *delayed_shutdown() {
  sleep(5);
  reboot(0x4321fedc);
  return NULL;
}

void *delayed_reboot() {
  sleep(5);
  reboot(0x01234567);
  return NULL;
}

void *schedule_call(void *cmd) {
  int strsz = 0, ret;
  uint8_t *offset;
  uint8_t *command = (uint8_t *)cmd;
  uint8_t *reply = calloc(256, sizeof(unsigned char));
  pthread_t call_schedule_thread;
  logger(MSG_WARN, "SCH: %s -> %s \n", cmd, command);

  int delaysec;
  char tmpbuf[10];
  char *secoffset;
  offset = (uint8_t *)strstr((char *)command, partial_commands[2].cmd);
  if (offset == NULL) {
    strsz = snprintf((char *)reply, MAX_MESSAGE_SIZE - strsz,
                     "Error reading the command\n");
    add_message_to_queue(reply, strsz);
  } else {
    int ofs = (int)(offset - command) + strlen(partial_commands[2].cmd);
    snprintf(tmpbuf, 10, "%s", (char *)command + ofs);
    delaysec = strtol(tmpbuf, &secoffset, 10);
    if (delaysec > 0) {
      strsz = snprintf((char *)reply, MAX_MESSAGE_SIZE,
                       "I will call you back in %i seconds\n", delaysec);
      add_message_to_queue(reply, strsz);
      sleep(delaysec);
      logger(MSG_INFO, "Calling you now!\n");
      set_pending_call_flag(true);
    } else {
      strsz = snprintf(
          (char *)reply, MAX_MESSAGE_SIZE,
          "Please tell me in how many seconds you want me to call you, %s\n",
          cmd_runtime.user_name);
      add_message_to_queue(reply, strsz);
    }
  }
  free(reply);
  reply = NULL;
  command = NULL;
  return NULL;
}

void render_gsm_signal_data() {
  int strsz = 0;
  struct network_state netstat;
  netstat = get_network_status();
  uint8_t *reply = calloc(256, sizeof(unsigned char));
  strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                    "Network type: ");
  if (get_network_type() >= 0x00 && get_network_type() <= 0x08) {
    strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "%s\n",
                      network_types[get_network_type()]);

  } else {
    strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                      "Unknown (0x%.2x)\n", get_network_type());
  }

  strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                    "Signal strength: %i %% \n", get_signal_strength());

  strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                    "Roaming %i \n", netstat.is_roaming );
  
  strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                    "In call %i \n", netstat.in_call );
  add_message_to_queue(reply, strsz);

  free(reply);
  reply = NULL;
}
uint8_t parse_command(uint8_t *command) {
  int ret = 0;
  uint16_t i, random;
  FILE *fp;
  int cmd_id = -1;
  int strsz = 0;
  struct pkt_stats packet_stats;
  pthread_t disposable_thread;
  struct network_state netstat;
  uint8_t *tmpbuf = calloc(128, sizeof(unsigned char));
  uint8_t *reply = calloc(256, sizeof(unsigned char));
  srand(time(NULL));
  /* Static commands */
  for (i = 0; i < (sizeof(bot_commands) / sizeof(bot_commands[0])); i++) {
    if (strcmp((char *)command, bot_commands[i].cmd) == 0) {
      cmd_id = bot_commands[i].id;
    }
  }
  /* Commands with arguments */
  if (cmd_id == -1) {
    for (i = 0; i < (sizeof(partial_commands) / sizeof(partial_commands[0]));
         i++) {
      if (strstr((char *)command, partial_commands[i].cmd) != NULL) {
        cmd_id = partial_commands[i].id;
        logger(MSG_INFO, "PCMD match! %i -> %s\n", ret, command);
      }
    }
  }
  ret = find_cmd_history_match(cmd_id);
  logger(MSG_INFO, "Repeated cmds %i\n", ret);
  if (ret >= 5) {
    logger(MSG_WARN, "You're pissing me off\n");
    random = rand() % 10;
    strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "%s\n",
                      repeated_cmd[random].answer);
  }
  switch (cmd_id) {
  case -1:
    logger(MSG_INFO, "%s: Nothing to do\n", __func__);
    strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                      "Command not found: %s\n", command);
    add_message_to_queue(reply, strsz);
    break;
  case 0:
    strsz +=
        snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "%s %s\n",
                 bot_commands[cmd_id].cmd_text, cmd_runtime.bot_name);
    add_message_to_queue(reply, strsz);
    break;
  case 1:
    if (get_uptime(tmpbuf) == 0) {
      strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                        "Hi %s, %s:\n %s\n", cmd_runtime.user_name,
                        bot_commands[cmd_id].cmd_text, tmpbuf);
    } else {
      strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                        "Error getting the uptime\n");
    }
    add_message_to_queue(reply, strsz);
    break;
  case 2:
    if (get_load_avg(tmpbuf) == 0) {
      strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                        "Hi %s, %s:\n %s\n", cmd_runtime.user_name,
                        bot_commands[cmd_id].cmd_text, tmpbuf);
    } else {
      strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                        "Error getting laodavg\n");
    }
    add_message_to_queue(reply, strsz);
    break;
  case 3:
    strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                      "I'm at version %s\n", RELEASE_VER);
    add_message_to_queue(reply, strsz);
    break;
  case 4:
    strsz +=
        snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                 "USB Suspend state: %i\n", get_transceiver_suspend_state());
    add_message_to_queue(reply, strsz);
    break;
  case 5:
    if (get_memory(tmpbuf) == 0) {
      strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                        "Memory stats:\n%s\n", tmpbuf);
    } else {
      strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                        "Error getting laodavg\n");
    }
    add_message_to_queue(reply, strsz);
    break;
  case 6:
    packet_stats = get_rmnet_stats();
    strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                      "RMNET IF stats:\nBypassed: "
                      "%i\nEmpty:%i\nDiscarded:%i\nFailed:%i\nAllowed:%i",
                      packet_stats.bypassed, packet_stats.empty,
                      packet_stats.discarded, packet_stats.failed,
                      packet_stats.allowed);
    add_message_to_queue(reply, strsz);
    break;
  case 7:
    packet_stats = get_gps_stats();
    strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                      "GPS IF stats:\nBypassed: "
                      "%i\nEmpty:%i\nDiscarded:%i\nFailed:%i\nAllowed:%"
                      "i\nQMI Location svc.: %i",
                      packet_stats.bypassed, packet_stats.empty,
                      packet_stats.discarded, packet_stats.failed,
                      packet_stats.allowed, packet_stats.other);
    add_message_to_queue(reply, strsz);
    break;
  case 8:
    strsz = 0;
    snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
             "Help: Static commands\n");
    add_message_to_queue(reply, strsz);
    strsz = 0;
    for (i = 0; i < (sizeof(bot_commands) / sizeof(bot_commands[0])); i++) {
      if (strlen(bot_commands[i].cmd) + (3 * sizeof(uint8_t)) +
              strlen(bot_commands[i].help) + strsz >
          MAX_MESSAGE_SIZE) {
        add_message_to_queue(reply, strsz);
        strsz = 0;
      }
      strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                        "%s: %s\n", bot_commands[i].cmd, bot_commands[i].help);
    }
    add_message_to_queue(reply, strsz);
    strsz = 0;
    snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
             "Help: Commands with arguments\n");
    add_message_to_queue(reply, strsz);
    strsz = 0;
    for (i = 0; i < (sizeof(partial_commands) / sizeof(partial_commands[0]));
         i++) {
      if (strlen(partial_commands[i].cmd) + (3 * sizeof(uint8_t)) +
              strlen(partial_commands[i].help) + strsz >
          MAX_MESSAGE_SIZE) {
        add_message_to_queue(reply, strsz);
        strsz = 0;
      }
      strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                        "%s x: %s\n", partial_commands[i].cmd,
                        partial_commands[i].help);
    }
    add_message_to_queue(reply, strsz);
    break;
  case 9:
    strsz += snprintf(
        (char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
        "Blocking USB suspend until reboot or until you tell me otherwise!\n");
    set_suspend_inhibit(false);
    add_message_to_queue(reply, strsz);
    break;
  case 10:
    strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                      "Allowing USB tu suspend again\n");
    set_suspend_inhibit(false);
    add_message_to_queue(reply, strsz);
    break;
  case 11:
    strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                      "Turning ADB *ON*\n");
    store_adb_setting(true);
    restart_usb_stack();
    add_message_to_queue(reply, strsz);
    break;
  case 12:
    strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                      "Turning ADB *OFF*\n");
    store_adb_setting(false);
    restart_usb_stack();
    add_message_to_queue(reply, strsz);
    break;
  case 13:
    for (i = 0; i < cmd_runtime.cmd_position; i++) {
      if (strsz < 160) {
        strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                          "%i ", cmd_runtime.cmd_history[i]);
      }
    }
    add_message_to_queue(reply, strsz);
    break;
  case 14:
    fp = fopen("/var/log/openqti.log", "r");
    if (fp == NULL) {
      logger(MSG_ERROR, "%s: Error opening file \n", __func__);
      strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                        "Error opening file\n");
    } else {
      strsz = snprintf((char *)reply, MAX_MESSAGE_SIZE, "OpenQTI Log\n");
      add_message_to_queue(reply, strsz);
      if (ret > (MAX_MESSAGE_SIZE * QUEUE_SIZE)) {
        fseek(fp, (ret - (MAX_MESSAGE_SIZE * QUEUE_SIZE)), SEEK_SET);
      } else {
        fseek(fp, 0L, SEEK_SET);
      }
      do {
        memset(reply, 0, MAX_MESSAGE_SIZE);
        ret = fread(reply, 1, MAX_MESSAGE_SIZE - 2, fp);
        if (ret > 0) {
          add_message_to_queue(reply, ret);
        }
      } while (ret > 0);
      fclose(fp);
    }
    break;
  case 15:
    fp = fopen("/var/log/messages", "r");
    if (fp == NULL) {
      logger(MSG_ERROR, "%s: Error opening file \n", __func__);
      strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                        "Error opening file\n");
    } else {
      strsz = snprintf((char *)reply, MAX_MESSAGE_SIZE, "DMESG:\n");
      add_message_to_queue(reply, strsz);
      fseek(fp, 0L, SEEK_END);
      ret = ftell(fp);
      if (ret > (MAX_MESSAGE_SIZE * QUEUE_SIZE)) {
        fseek(fp, (ret - (MAX_MESSAGE_SIZE * QUEUE_SIZE)), SEEK_SET);
      } else {
        fseek(fp, 0L, SEEK_SET);
      }
      do {
        memset(reply, 0, MAX_MESSAGE_SIZE);
        ret = fread(reply, 1, MAX_MESSAGE_SIZE - 2, fp);
        if (ret > 0) {
          add_message_to_queue(reply, ret);
        }
      } while (ret > 0);
      fclose(fp);
    }
    break;

  case 16:
    strsz +=
        snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "%s: %i\n",
                 bot_commands[cmd_id].cmd_text, get_dirty_reconnects());
    add_message_to_queue(reply, strsz);
    break;
  case 17:
    strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "%s\n",
                      bot_commands[cmd_id].cmd_text);
    add_message_to_queue(reply, strsz);
    set_pending_call_flag(true);
    break;
  case 18:
    strsz +=
        snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "%s %s\n",
                 bot_commands[cmd_id].cmd_text, cmd_runtime.user_name);
    add_message_to_queue(reply, strsz);
    break;
  case 19:
    pthread_create(&disposable_thread, NULL, &delayed_shutdown, NULL);
    strsz +=
        snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "%s %s!\n",
                 bot_commands[cmd_id].cmd_text, cmd_runtime.user_name);
    add_message_to_queue(reply, strsz);
    break;
  case 20:
    render_gsm_signal_data();
    break;
  case 21:
    pthread_create(&disposable_thread, NULL, &delayed_reboot, NULL);
    strsz +=
        snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz, "%s %s!\n",
                 bot_commands[cmd_id].cmd_text, cmd_runtime.user_name);
    add_message_to_queue(reply, strsz);
    break;
  case 22:
    dump_signal_report();
    break;
  case 23:
    strsz = snprintf((char *)reply, MAX_MESSAGE_SIZE,  "Enable signal tracking\n");
    add_message_to_queue(reply, strsz);
    enable_signal_tracking(true);
    break;
  case 24:
    strsz = snprintf((char *)reply, MAX_MESSAGE_SIZE,  "Disable signal tracking\n");
    add_message_to_queue(reply, strsz);
    enable_signal_tracking(false);
    break;
  case 100:
    set_custom_modem_name(command);
    break;
  case 101:
    set_custom_user_name(command);
    break;
  case 102:
    pthread_create(&disposable_thread, NULL, &schedule_call, command);
    sleep(2); // our string gets wiped out before we have a chance
    break;
  case 103:
    debug_cb_message(command);
    break;
  default:
    strsz += snprintf((char *)reply + strsz, MAX_MESSAGE_SIZE - strsz,
                      "Invalid command id %i\n", cmd_id);
    logger(MSG_INFO, "%s: Unknown command %i\n", __func__, cmd_id);
    break;
  }

  add_to_history(cmd_id);

  free(tmpbuf);
  free(reply);

  tmpbuf = NULL;
  reply = NULL;
  return ret;
}
