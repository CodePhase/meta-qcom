/*
** Copyright 2010, The Android Open-Source Project
** Copyright (c) 2011-2013, 2015 The Linux Foundation. All rights reserved.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

/** Pieces of alsa_mixer.c **/

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sound/asound.h>
#include <sound/tlv.h>
#include <sys/ioctl.h>

#include "../inc/audio.h"
#include "../inc/logger.h"

#define check_range(val, min, max)                                             \
  (((val < min) ? (min) : (val > max) ? (max) : (val)))

/* .5 for rounding before casting to non-decmal value */
/* Should not be used if you need decmal values */
/* or are expecting negitive indexes */
#define percent_to_index(val, min, max)                                        \
  ((val) * ((max) - (min)) * 0.01 + (min) + .5)

#define DEFAULT_TLV_SIZE 4096

static int is_volume(const char *name, enum ctl_type *type) {
  const struct suf *p;
  size_t nlen = strlen(name);
  p = suffixes;
  while (p->suffix) {
    size_t slen = strnlen(p->suffix, 44);
    size_t l;
    if (nlen > slen) {
      l = nlen - slen;
      if (strncmp(name + l, p->suffix, slen) == 0 &&
          (l < 1 || name[l - 1] != '-')) { /* 3D Control - Switch */
        *type = p->type;
        return l;
      }
    }
    p++;
  }
  return 0;
}

static const char *elem_iface_name(snd_ctl_elem_iface_t n) {
  switch (n) {
  case SNDRV_CTL_ELEM_IFACE_CARD:
    return "CARD";
  case SNDRV_CTL_ELEM_IFACE_HWDEP:
    return "HWDEP";
  case SNDRV_CTL_ELEM_IFACE_MIXER:
    return "MIXER";
  case SNDRV_CTL_ELEM_IFACE_PCM:
    return "PCM";
  case SNDRV_CTL_ELEM_IFACE_RAWMIDI:
    return "MIDI";
  case SNDRV_CTL_ELEM_IFACE_TIMER:
    return "TIMER";
  case SNDRV_CTL_ELEM_IFACE_SEQUENCER:
    return "SEQ";
  default:
    return "???";
  }
}

static const char *elem_type_name(snd_ctl_elem_type_t n) {
  switch (n) {
  case SNDRV_CTL_ELEM_TYPE_NONE:
    return "NONE";
  case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
    return "BOOL";
  case SNDRV_CTL_ELEM_TYPE_INTEGER:
    return "INT32";
  case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
    return "ENUM";
  case SNDRV_CTL_ELEM_TYPE_BYTES:
    return "BYTES";
  case SNDRV_CTL_ELEM_TYPE_IEC958:
    return "IEC958";
  case SNDRV_CTL_ELEM_TYPE_INTEGER64:
    return "INT64";
  default:
    return "???";
  }
}
struct mixer *mixer_open(const char *device) {
  struct snd_ctl_elem_list elist;
  struct snd_ctl_elem_info tmp;
  struct snd_ctl_elem_id *eid = NULL;
  struct mixer *mixer = NULL;
  unsigned n, m;
  int fd;

  fd = open(device, O_RDWR);
  if (fd < 0) {
    logger(MSG_WARN, "Control open failed\n");
    return 0;
  }

  memset(&elist, 0, sizeof(elist));
  if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &elist) < 0) {
    logger(MSG_WARN, "SNDRV_CTL_IOCTL_ELEM_LIST failed\n");
    goto fail;
  }

  mixer = calloc(1, sizeof(*mixer));
  if (!mixer)
    goto fail;

  mixer->ctl = calloc(elist.count, sizeof(struct mixer_ctl));
  mixer->info = calloc(elist.count, sizeof(struct snd_ctl_elem_info));
  if (!mixer->ctl || !mixer->info)
    goto fail;

  eid = calloc(elist.count, sizeof(struct snd_ctl_elem_id));
  if (!eid)
    goto fail;

  mixer->count = elist.count;
  mixer->fd = fd;
  elist.space = mixer->count;
  elist.pids = eid;
  if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &elist) < 0)
    goto fail;

  for (n = 0; n < mixer->count; n++) {
    struct snd_ctl_elem_info *ei = mixer->info + n;
    ei->id.numid = eid[n].numid;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, ei) < 0)
      goto fail;
    mixer->ctl[n].info = ei;
    mixer->ctl[n].mixer = mixer;
    if (ei->type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
      char **enames = calloc(ei->value.enumerated.items, sizeof(char *));
      if (!enames)
        goto fail;
      mixer->ctl[n].ename = enames;
      for (m = 0; m < ei->value.enumerated.items; m++) {
        memset(&tmp, 0, sizeof(tmp));
        tmp.id.numid = ei->id.numid;
        tmp.value.enumerated.item = m;
        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &tmp) < 0)
          goto fail;
        enames[m] = strdup(tmp.value.enumerated.name);
        if (!enames[m])
          goto fail;
      }
    }
  }

  free(eid);
  return mixer;

fail:
  if (eid)
    free(eid);
  if (mixer)
    mixer_close(mixer);
  else if (fd >= 0)
    close(fd);
  return 0;
}

void mixer_close(struct mixer *mixer) {
  unsigned n, m;

  if (mixer->fd >= 0)
    close(mixer->fd);

  if (mixer->ctl) {
    for (n = 0; n < mixer->count; n++) {
      if (mixer->ctl[n].ename) {
        unsigned max = mixer->ctl[n].info->value.enumerated.items;
        for (m = 0; m < max; m++)
          free(mixer->ctl[n].ename[m]);
        free(mixer->ctl[n].ename);
      }
    }
    free(mixer->ctl);
  }

  if (mixer->info)
    free(mixer->info);

  free(mixer);
}

void mixer_dump(struct mixer *mixer) {
  unsigned n, m;

  logger(MSG_WARN, "  id iface dev sub idx num perms     type   isvolume  name\n");
  for (n = 0; n < mixer->count; n++) {
    enum ctl_type type;
    struct snd_ctl_elem_info *ei = mixer->info + n;

    logger(MSG_WARN, "%4d %5s %3d %3d %3d %3d %c%c%c%c%c%c%c%c %-6s %8d  %s",
           ei->id.numid, elem_iface_name(ei->id.iface), ei->id.device,
           ei->id.subdevice, ei->id.index, ei->count,
           (ei->access & SNDRV_CTL_ELEM_ACCESS_READ) ? 'r' : ' ',
           (ei->access & SNDRV_CTL_ELEM_ACCESS_WRITE) ? 'w' : ' ',
           (ei->access & SNDRV_CTL_ELEM_ACCESS_VOLATILE) ? 'V' : ' ',
           (ei->access & SNDRV_CTL_ELEM_ACCESS_TLV_READ) ? 'R' : ' ',
           (ei->access & SNDRV_CTL_ELEM_ACCESS_TLV_WRITE) ? 'W' : ' ',
           (ei->access & SNDRV_CTL_ELEM_ACCESS_TLV_COMMAND) ? 'C' : ' ',
           (ei->access & SNDRV_CTL_ELEM_ACCESS_INACTIVE) ? 'I' : ' ',
           (ei->access & SNDRV_CTL_ELEM_ACCESS_LOCK) ? 'L' : ' ',
           elem_type_name(ei->type), (is_volume(ei->id.name, &type)) ? 1 : 0,
           ei->id.name);
    switch (ei->type) {
    case SNDRV_CTL_ELEM_TYPE_INTEGER:
      printf(ei->value.integer.step ? " { %ld-%ld, %ld }\n" : " { %ld-%ld }",
             ei->value.integer.min, ei->value.integer.max,
             ei->value.integer.step);
      break;
    case SNDRV_CTL_ELEM_TYPE_INTEGER64:
      printf(ei->value.integer64.step ? " { %lld-%lld, %lld }\n"
                                      : " { %lld-%lld }",
             ei->value.integer64.min, ei->value.integer64.max,
             ei->value.integer64.step);
      break;
    case SNDRV_CTL_ELEM_TYPE_ENUMERATED: {
      unsigned m;
      logger(MSG_WARN, " { %s=0", mixer->ctl[n].ename[0]);
      for (m = 1; m < ei->value.enumerated.items; m++)
        logger(MSG_WARN, ", %s=%d", mixer->ctl[n].ename[m], m);
      logger(MSG_WARN, " }");
      break;
    }
    }
    logger(MSG_WARN, "\n");
  }
}

struct mixer_ctl *mixer_get_control(struct mixer *mixer, const char *name,
                                    unsigned index) {
  unsigned n;
  for (n = 0; n < mixer->count; n++) {
    if (mixer->info[n].id.index == index) {
      if (!strncmp(name, (char *)mixer->info[n].id.name,
                   sizeof(mixer->info[n].id.name))) {
        return mixer->ctl + n;
      }
    }
  }
  logger(MSG_ERROR, "%s: Mixer control %s not found\n", __func__, name);
  return 0;
}

struct mixer_ctl *mixer_get_nth_control(struct mixer *mixer, unsigned n) {
  if (n < mixer->count)
    return mixer->ctl + n;
  return 0;
}
struct mixer_ctl *get_ctl(struct mixer *mixer, char *name) {
  char *p;
  unsigned idx = 0;
  if (isdigit(name[0]))
    return mixer_get_nth_control(mixer, atoi(name) - 1);

  p = strrchr(name, '#');
  if (p) {
    *p++ = 0;
    idx = atoi(p);
  }

  return mixer_get_control(mixer, name, idx);
}
static void print_dB(long dB) {
  logger(MSG_WARN, "%li.%02lidB", dB / 100, (dB < 0 ? -dB : dB) % 100);
}

static long scale_int(struct snd_ctl_elem_info *ei, unsigned _percent) {
  long percent;

  if (_percent > 100)
    percent = 100;
  else
    percent = (long)_percent;

  return (long)percent_to_index(percent, ei->value.integer.min,
                                ei->value.integer.max);
}

static long long scale_int64(struct snd_ctl_elem_info *ei, unsigned _percent) {
  long long percent;

  if (_percent > 100)
    percent = 100;
  else
    percent = (long)_percent;

  return (long long)percent_to_index(percent, ei->value.integer.min,
                                     ei->value.integer.max);
}

int mixer_ctl_mulvalues(struct mixer_ctl *ctl, int count, int val) {
  struct snd_ctl_elem_value ev;
  unsigned n;
  if (!ctl) {
    logger(MSG_ERROR, "%s: Bailing out, can't find control\n", __func__);
    return -1;
  }
  if (count < ctl->info->count || count > ctl->info->count) {
    logger(MSG_ERROR, "Count is invalid!: ");
    if (count < ctl->info->count)
      logger(MSG_ERROR, "Insufficent number of params \n");
    if (count > ctl->info->count)
      logger(MSG_ERROR, "Too many params\n");

    return -EINVAL;
  }

  memset(&ev, 0, sizeof(ev));
  ev.id.numid = ctl->info->id.numid;
  switch (ctl->info->type) {
  case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
    for (n = 0; n < ctl->info->count; n++) {
      ev.value.integer.value[n] = !!val;
    }
    break;
  case SNDRV_CTL_ELEM_TYPE_INTEGER:
    for (n = 0; n < ctl->info->count; n++) {
      if (count < ctl->info->count && n == 0) {
        ev.value.integer.value[n] = 1;
      } else {
        ev.value.integer.value[n] = val;
      }
    }
    break;
  case SNDRV_CTL_ELEM_TYPE_INTEGER64:
    for (n = 0; n < ctl->info->count; n++) {
      long long value_ll = scale_int64(ctl->info, val);
      ev.value.integer64.value[n] = value_ll;
    }
    break;
  case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
    for (n = 0; n < ctl->info->count; n++) {
      ev.value.enumerated.item[n] = (unsigned int)val;
    }
    break;
  default:
    logger(MSG_WARN, "%s: Unknown element type\n", __func__);
    errno = EINVAL;
    return errno;
  }
  return ioctl(ctl->mixer->fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev);
}

int mixer_ctl_read_tlv(struct mixer_ctl *ctl, unsigned int *tlv, long *min,
                       long *max, unsigned int *tlv_type) {
  unsigned int tlv_size = DEFAULT_TLV_SIZE;
  unsigned int type;
  unsigned int size;

  if (!!(ctl->info->access & SNDRV_CTL_ELEM_ACCESS_TLV_READ)) {
    struct snd_ctl_tlv *xtlv;
    tlv[0] = -1;
    tlv[1] = 0;
    xtlv = calloc(1, sizeof(struct snd_ctl_tlv) + tlv_size);
    if (xtlv == NULL)
      return -ENOMEM;
    xtlv->numid = ctl->info->id.numid;
    xtlv->length = tlv_size;
    memcpy(xtlv->tlv, tlv, tlv_size);
    if (ioctl(ctl->mixer->fd, SNDRV_CTL_IOCTL_TLV_READ, xtlv) < 0) {
      logger(MSG_ERROR, "SNDRV_CTL_IOCTL_TLV_READ failed\n");
      free(xtlv);
      return -errno;
    }
    if (xtlv->tlv[1] + 2 * sizeof(unsigned int) > tlv_size) {
      free(xtlv);
      return -EFAULT;
    }
    memcpy(tlv, xtlv->tlv, xtlv->tlv[1] + 2 * sizeof(unsigned int));
    free(xtlv);

    type = tlv[0];
    *tlv_type = type;
    size = tlv[1];
    switch (type) {
    case SNDRV_CTL_TLVT_DB_SCALE: {
      int idx = 2;
      int step;
      logger(MSG_WARN, "dBscale-");
      if (size != 2 * sizeof(unsigned int)) {
        while (size > 0) {
          logger(MSG_WARN, "0x%08x,", tlv[idx++]);
          size -= sizeof(unsigned int);
        }
      } else {
        logger(MSG_WARN, " min=");
        print_dB((int)tlv[2]);
        *min = (long)tlv[2];
        logger(MSG_WARN, " step=");
        step = (tlv[3] & 0xffff);
        print_dB(tlv[3] & 0xffff);
        logger(MSG_WARN, " max=");
        *max = (ctl->info->value.integer.max);
        print_dB((long)ctl->info->value.integer.max);
        logger(MSG_WARN, " mute=%i\n", (tlv[3] >> 16) & 1);
      }
      break;
    }
    case SNDRV_CTL_TLVT_DB_LINEAR: {
      int idx = 2;
      logger(MSG_WARN, "dBLiner-");
      if (size != 2 * sizeof(unsigned int)) {
        while (size > 0) {
          logger(MSG_WARN, "0x%08x,", tlv[idx++]);
          size -= sizeof(unsigned int);
        }
      } else {
        logger(MSG_WARN, " min=");
        *min = tlv[2];
        print_dB(tlv[2]);
        logger(MSG_WARN, " max=");
        *max = tlv[3];
        print_dB(tlv[3]);
      }
      break;
    }
    default:
      break;
    }
    return 0;
  }
  return -EINVAL;
}

int mixer_ctl_set(struct mixer_ctl *ctl, unsigned percent) {
  struct snd_ctl_elem_value ev;
  unsigned n;
  long min, max;
  unsigned int *tlv = NULL;
  enum ctl_type type;
  int volume = 0;
  unsigned int tlv_type;

  if (!ctl) {
    logger(MSG_ERROR, "can't find control\n");
    return -1;
  }

  if (is_volume(ctl->info->id.name, &type)) {
    tlv = calloc(1, DEFAULT_TLV_SIZE);
    if (tlv == NULL) {
      logger(MSG_WARN, "failed to allocate memory\n");
    } else if (!mixer_ctl_read_tlv(ctl, tlv, &min, &max, &tlv_type)) {
      switch (tlv_type) {
      case SNDRV_CTL_TLVT_DB_LINEAR:
        logger(MSG_WARN, "tlv db linear: b4 %d\n", percent);

        if (min < 0) {
          max = max - min;
          min = 0;
        }
        percent = check_range(percent, min, max);
        logger(MSG_WARN, "tlv db linear: %d %ld %ld\n", percent, min, max);
        volume = 1;
        break;
      default:
        percent = (long)percent_to_index(percent, min, max);
        percent = check_range(percent, min, max);
        volume = 1;
        break;
      }
    } else
      logger(MSG_WARN, "mixer_ctl_read_tlv failed\n");
    free(tlv);
  }
  memset(&ev, 0, sizeof(ev));
  ev.id.numid = ctl->info->id.numid;
  switch (ctl->info->type) {
  case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
    for (n = 0; n < ctl->info->count; n++)
      ev.value.integer.value[n] = !!percent;
    break;
  case SNDRV_CTL_ELEM_TYPE_INTEGER: {
    int value;
    if (!volume)
      value = scale_int(ctl->info, percent);
    else
      value = (int)percent;
    for (n = 0; n < ctl->info->count; n++)
      ev.value.integer.value[n] = value;
    break;
  }
  case SNDRV_CTL_ELEM_TYPE_INTEGER64: {
    long long value;
    if (!volume)
      value = scale_int64(ctl->info, percent);
    else
      value = (long long)percent;
    for (n = 0; n < ctl->info->count; n++)
      ev.value.integer64.value[n] = value;
    break;
  }
  default:
    errno = EINVAL;
    return errno;
  }

  return ioctl(ctl->mixer->fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev);
}

int mixer_ctl_set_value(struct mixer_ctl *ctl, int count, int val) {
  unsigned int size;
  unsigned int *tlv = NULL;
  long min, max;
  enum ctl_type type;
  unsigned int tlv_type;
  int ret;
  if (is_volume(ctl->info->id.name, &type)) {
    logger(MSG_WARN, "Volume levels badly handled\n");
    return mixer_ctl_mulvalues(ctl, count, val);
  } else {
    return mixer_ctl_mulvalues(ctl, count, val);
  }
  return EINVAL;
}
