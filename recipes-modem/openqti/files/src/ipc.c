// SPDX-License-Identifier: MIT

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../inc/devices.h"
#include "../inc/helpers.h"
#include "../inc/ipc.h"
#include "../inc/logger.h"
struct {
  uint8_t services[32];
  uint8_t last_active;
  uint32_t regtime;
} client_handle_track;

int open_ipc_socket(struct qmi_device *qmisock, uint32_t node, uint32_t port,
                    uint32_t service, uint32_t instance,
                    unsigned char address_type) {
  qmisock->fd = socket(IPC_ROUTER, SOCK_DGRAM, 0);
  qmisock->service = service;
  qmisock->transaction_id = 1;
  qmisock->socket.family = IPC_ROUTER;
  qmisock->socket.address.addrtype = address_type;
  qmisock->socket.address.addr.port_addr.node_id = node;
  qmisock->socket.address.addr.port_addr.port_id = port;
  qmisock->socket.address.addr.port_name.service = service;
  qmisock->socket.address.addr.port_name.instance = instance;

  return qmisock->fd;
}

bool is_server_active(uint32_t service, uint32_t instance) {
  int sock, i;
  bool ret = false;
  struct server_lookup_args *lookup;
  lookup =
      (struct server_lookup_args *)calloc(2, sizeof(struct server_lookup_args));

  sock = socket(IPC_ROUTER, SOCK_DGRAM, 0);
  lookup->port_name.service = service;
  lookup->port_name.instance = instance;
  if (instance == 0) {
    lookup->lookup_mask = 0;
  } else {
    lookup->lookup_mask = 0xFFFFFFFF;
  }
  lookup->num_entries_in_array =
      1; // In reality this is the number of entries to fill
  lookup->num_entries_found = 0;
  if (ioctl(sock, IPC_ROUTER_IOCTL_LOOKUP_SERVER, lookup) >= 0) {
    for (i = 0; i < lookup->num_entries_in_array; i++) {
      if (lookup->srv_info[i].node_id != 41 && i == 0) {
        ret = true;
      }
    }
  }
  close(sock);

  free(lookup);
  lookup = NULL;
  return ret;
}

int find_services() {
  int i, j, k, fd;
  bool name = false;
  uint32_t instance = 1;
  int ret = 0;
  struct server_lookup_args *lookup;
  lookup =
      (struct server_lookup_args *)calloc(2, sizeof(struct server_lookup_args));
  fprintf(stdout, "Service Instance Node    Port \t Name \n");
  fprintf(stdout, "--------------------------------------------\n");
  for (k = 1; k <= 4097; k++) {
    name = false;
    fd = socket(IPC_ROUTER, SOCK_DGRAM, 0);
    if (fd < 0) {
      fprintf(stdout, "Error opening socket\n");
      return -EINVAL;
    }
    lookup->port_name.service = k;
    lookup->port_name.instance = instance;
    lookup->num_entries_in_array = 1;
    lookup->num_entries_found = 0;
    if (instance == 0) {
      lookup->lookup_mask = 0;
    } else {
      lookup->lookup_mask = 0xFFFFFFFF;
    }
    if (ioctl(fd, IPC_ROUTER_IOCTL_LOOKUP_SERVER, lookup) < 0) {

      fprintf(stdout, "ioctl failed\n");
      close(fd);
      free(lookup);
      return -EINVAL;
    }

    for (i = 0; i < lookup->num_entries_in_array; i++) {
      if (lookup->srv_info[i].port_id != 0x0e &&
          lookup->srv_info[i].port_id != 0x0b) {
        fprintf(stdout, "%i \t %i \t 0x%.2x \t 0x%.2x \t", k, instance,
                lookup->srv_info[i].node_id, lookup->srv_info[i].port_id);
        for (j = 0; j < (sizeof(common_names) / sizeof(common_names[0])); j++) {
          if (common_names[j].service == k) {
            fprintf(stdout, " %s\n", common_names[j].name);
            name = true;
          }
        }
        if (!name) {
          fprintf(stdout, " Unknown service \n");
        }
        if (i > 0) {
          fprintf(
              stdout,
              "Hey we have more than one port for the same service or what?\n");
        }
      }
    }
    close(fd);
  }

  free(lookup);
  lookup = NULL;
  return 0;
}

struct msm_ipc_server_info get_node_port(uint32_t service, uint32_t instance) {
  struct server_lookup_args *lookup;

  int sock, i;
  bool ret = false;
  struct msm_ipc_server_info port_combo;

  port_combo.node_id = 0;
  port_combo.port_id = 0;
  sock = socket(IPC_ROUTER, SOCK_DGRAM, 0);
  lookup =
      (struct server_lookup_args *)calloc(2, sizeof(struct server_lookup_args));
  lookup->port_name.service = service;
  lookup->port_name.instance = instance;
  lookup->num_entries_in_array = 1;
  lookup->num_entries_found = 0;
  if (instance == 0) {
    lookup->lookup_mask = 0;
  } else {
    lookup->lookup_mask = 0xFFFFFFFF;
  }

  if (ioctl(sock, IPC_ROUTER_IOCTL_LOOKUP_SERVER, lookup) >= 0) {
    if (lookup->num_entries_in_array > 0) {
      if (lookup->srv_info[0].node_id != 41) {
        port_combo.node_id = lookup->srv_info[0].node_id;
        port_combo.port_id = lookup->srv_info[0].port_id;
        port_combo.service = lookup->srv_info[0].service;
        port_combo.instance = lookup->srv_info[0].instance;

        ret = true;
      }
    }
  }

  free(lookup);
  close(sock);
  lookup = NULL;
  return port_combo;
}

/* Setup initial serivce permissions on the IPC router.
   You need to send it something before you can start
   using it. This sets it up so root and a user with
   UID 54 / GID 54 (modemusr in my custom yocto) can
   access it */
int setup_ipc_security() {
  int fd;
  int i;
  int ret = 0;
  int ipc_categories = 511;
  struct irsc_rule *cur_rule;
  cur_rule = calloc(1, (sizeof(struct irsc_rule) + sizeof(uint32_t)));
  logger(MSG_DEBUG, "%s: Setting up MSM IPC Router security...\n", __func__);
  fd = socket(IPC_ROUTER, SOCK_DGRAM, 0);
  if (!fd) {
    logger(MSG_ERROR, " Error opening socket \n");
    return -1;
  }
  for (i = 0; i < ipc_categories; i++) {
    cur_rule->rl_no = 54;
    cur_rule->service = i;
    cur_rule->instance = IRSC_INSTANCE_ALL; // all instances
    cur_rule->group_id[0] = 54;
    if (ioctl(fd, IOCTL_RULES, cur_rule) < 0) {
      logger(MSG_ERROR, "%s: Error serring rule %i \n", __func__, i);
      ret = 2;
      break;
    }
  }

  close(fd);
  if (ret != 0) {
    logger(MSG_ERROR, "%s: Error uploading rules (%i) \n", __func__, ret);
  } else {
    logger(MSG_DEBUG, "%s: Upload finished. \n", __func__);
  }

  free(cur_rule);
  cur_rule = NULL;
  return ret;
}

/* Connect to DPM and request opening SMD Control port 8 */
int init_port_mapper() {
  int ret, linestate, dpmfd;
  struct timeval tv;
  struct portmapper_open_request dpmreq;
  char buf[MAX_PACKET_SIZE];
  struct qmi_device *qmidev;
  qmidev = calloc(1, sizeof(struct qmi_device));
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  socklen_t addrlen = sizeof(struct sockaddr_msm_ipc);

  ret = open_ipc_socket(qmidev, IPC_HEXAGON_NODE, IPC_HEXAGON_DPM_PORT, 0x2f,
                        0x1, IPC_ROUTER_DPM_ADDRTYPE); // open DPM service
  if (ret < 0) {
    logger(MSG_ERROR, "%s: Error opening IPC Socket!\n", __func__);
    return -EINVAL;
  }

  if (ioctl(qmidev->fd, IOCTL_BIND_TOIPC, 0) < 0) {
    logger(MSG_ERROR, "IOCTL to the IPC1 socket failed \n");
  }

  dpmfd = open(DPM_CTL, O_RDWR);
  if (dpmfd < 0) {
    logger(MSG_ERROR, "Error opening %s \n", DPM_CTL);
    return -EINVAL;
  }
  // Unknown IOCTL, just before line state to rmnet
  if (ioctl(dpmfd, _IOC(_IOC_READ, 0x72, 0x2, 0x4), &ret) < 0) {
    logger(MSG_ERROR, "IOCTL failed: %i\n", ret);
  }

  ret = setsockopt(qmidev->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  if (ret) {
    logger(MSG_ERROR, "Error setting socket options \n");
  }

  /* Create the SMD Open request packet */
  /* Parts of this are probably _very_ wrong*/
  /* Recreate the magic packet that request the port mapper to open smdcntl8 */
  dpmreq.ctlid = 0x00;
  dpmreq.transaction_id = htole16(1);
  dpmreq.msgid = htole16(32);
  dpmreq.length = sizeof(struct portmapper_open_request) -
                  (3 * sizeof(uint16_t)) -
                  sizeof(uint8_t); // Size of the packet minux qmi header
  dpmreq.is_valid_ctl_list = htole16(0x10);
  dpmreq.ctl_list_length =
      0x0b010015; // sizeof(SMDCTLPORTNAME); this has to be wrong...

  strncpy(dpmreq.hw_port_map[0].port_name, SMDCTLPORTNAME,
          sizeof(SMDCTLPORTNAME));
  dpmreq.hw_port_map[0].epinfo.ep_type = htole16(DATA_EP_TYPE_BAM_DMUX);
  dpmreq.hw_port_map[0].epinfo.peripheral_iface_id = 0x08000000;

  dpmreq.is_valid_hw_list = 0x00;
  dpmreq.hw_list_length = 0x11110000;
  dpmreq.hw_epinfo.ph_ep_info.ep_type = DATA_EP_TYPE_RESERVED;
  dpmreq.hw_epinfo.ph_ep_info.peripheral_iface_id = 0x00000501;

  dpmreq.hw_epinfo.ipa_ep_pair.cons_pipe_num =
      0x00000800; // 6 is what the bam responds but packet seems to say 0
  dpmreq.hw_epinfo.ipa_ep_pair.prod_pipe_num = htole32(0);

  dpmreq.is_valid_sw_list = false;
  dpmreq.sw_list_length = 0;

  do {
    sleep(1);
    logger(MSG_WARN,
           "%s: Waiting for the Dynamic port mapper to become ready... \n",
           __func__);
  } while (sendto(qmidev->fd, &dpmreq, sizeof(dpmreq), MSG_DONTWAIT,
                  (void *)&qmidev->socket, sizeof(qmidev->socket)) < 0);
  logger(MSG_DEBUG, "%s: DPM Request completed!\n", __func__);

  close(dpmfd);      // We won't need DPM anymore
  close(qmidev->fd); // We don't need this socket anymore
  free(qmidev);
  // All the rest is moved away from here and into the init
  return 0;
}

void drain_client_tracking() {
  int i;
  client_handle_track.regtime = 0;
  client_handle_track.last_active = 0;
  for (i = 0; i < 32; i++) {
    client_handle_track.services[i] = 0;
  }
}
int track_client_count(uint8_t *pkt, int from, int sz) {
  int msglength, i;
  if (pkt[0] != 0x01 || sz < 12 || pkt[9] != 0x00) {
    return 0;
  }
  switch (pkt[8]) {
  case 0x22:
    logger(MSG_WARN, "%s: QMI Register client request\n", __func__);
    if (client_handle_track.regtime == 0) {
      client_handle_track.regtime = get_curr_timestamp();
      /*
       * We need some headroom for mm/ofono here. Some services are registered
       * only after the user has entered the PIN, so wait at least 240 seconds
       * to trigger a client release. I'll make this better at some point when
       * I find all the services triggered from the PIN entry
       */
    } else if ((get_curr_timestamp() - client_handle_track.regtime) > 240000) {
      // Needs a force reset
      logger(MSG_WARN, "%s: It seems we need a reset \n", __func__);
      return 1;
    } else if (client_handle_track.last_active > 32) {
      // Needs a force reset
      logger(MSG_WARN, "%s: It seems we need a reset, too many clients \n",
             __func__);
      return 1;
    }
    msglength = pkt[10] + 10;
    switch (from) {
    case FROM_DSP:
      logger(MSG_WARN, "%s: Assigned instance ID 0x%.2x to service 0x%.2x \n",
             __func__, pkt[msglength + 1], pkt[msglength]);
      client_handle_track.services[client_handle_track.last_active] =
          pkt[msglength];
      client_handle_track.last_active++;
      break;
    case FROM_HOST:
      if (pkt[10] == 0x04) {
        logger(MSG_WARN, "%s: Request for service 0x%.2x with any instance \n",
               __func__, pkt[15]);

      } else if (pkt[10] == 0x05) {
        logger(MSG_WARN,
               "%s: Request for service 0x%.2x with instance 0x%.2x\n",
               __func__, pkt[15], pkt[16]);
      }
      break;
    }
    break;
  case 0x23:
    switch (from) {
    case FROM_DSP:
      logger(MSG_WARN, "%s: QMI Client Release from DSP\n", __func__);
      break;
    case FROM_HOST:
      /* If the host removes a port free it too */
      logger(MSG_WARN,
             "%s: QMI Client Release from HOST,S:%.2x I:%.2x, AC:%i \n",
             __func__, pkt[15], pkt[16], client_handle_track.last_active);
      for (i = 32; i >= 0; i--) {
        if (client_handle_track.services[i] == pkt[15]) {
          client_handle_track.services[i] = 0;
          client_handle_track.last_active--;
          if (client_handle_track.last_active <= 0) {
            logger(MSG_WARN,
                   "%s: All QMI Clients have been freed from the host\n",
                   __func__);
            client_handle_track.last_active = 0;
            client_handle_track.regtime = 0;
          }
        }
      }
      break;
    }
    break;
  }
  return 0;
}

void force_close_qmi(int fd) {
  int transaction_id = 0;
  int ret, i, instance, service;
  bool nuke_everything = false;
  uint8_t release_prototype[] = {0x01, 0x10, 0x00, 0x00, 0x00, 0x00,
                                 0x00, 0x04, 0x23, 0x00, 0x05, 0x00,
                                 0x01, 0x02, 0x00, 0x1a, 0x01};
  logger(MSG_WARN, "%s: Closing all active QMI connections\n", __func__);
  for (i = 0; i < client_handle_track.last_active; i++) {
    for (instance = 0; instance <= 0x05; instance++) {
      release_prototype[7] = transaction_id;
      transaction_id++;
      release_prototype[15] = client_handle_track.services[i];
      release_prototype[16] = instance;
      ret = write(fd, release_prototype, sizeof(release_prototype));
      logger(MSG_DEBUG,
             "%s: Closing connection to service %.2x, instance %i, bytes "
             "written: %i \n",
             __func__, client_handle_track.services[i], instance, ret);
    }
  }
  for (i = 0; i < 32; i++) {
    if (client_handle_track.services[i] != 0) {
      // There was some mismatch here so let's clean this mess
      // to avoid problems
      nuke_everything = true;
    }
  }
  if (nuke_everything) {
    for (service = 0xff; service >= 0x00; service--) {
          for (instance = 0; instance <= 0x05; instance++) {
      release_prototype[7] = transaction_id;
      transaction_id++;
      release_prototype[15] = service;
      release_prototype[16] = instance;
      ret = write(fd, release_prototype, sizeof(release_prototype));
      logger(MSG_ERROR,
             "%s: Nuke connection to service %.2x, instance %i, bytes "
             "written: %i \n",
             __func__, client_handle_track.services[i], instance, ret);
    }
    }
  }
  drain_client_tracking();
}