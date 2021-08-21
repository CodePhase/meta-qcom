#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>

#include "../inc/audio.h"
#include "../inc/devices.h"
#include "../inc/helpers.h"
#include "../inc/logger.h"

struct mixer *mixer;
struct pcm *pcm_tx;
struct pcm *pcm_rx;

/*  Audio runtime state:
 *    current_call_state: IDLE / CIRCUITSWITCH / VOLTE
 *    volte_hd_audio_mode: 8000 / 16000 / 48000
 *    output_device: I2S / USB
 */
struct {
  uint8_t current_call_state;
  uint8_t volte_hd_audio_mode;
  uint8_t output_device;
} audio_runtime_state;

void set_audio_runtime_default() {
  audio_runtime_state.current_call_state = CALL_STATUS_IDLE;
  audio_runtime_state.volte_hd_audio_mode = 0;
  audio_runtime_state.output_device = AUDIO_MODE_I2S;
}

void set_output_device(int device) {
  logger(MSG_DEBUG, "%s: Setting audio output to %i \n", __func__, device);
  audio_runtime_state.output_device = device;
}

uint8_t get_output_device() { return audio_runtime_state.output_device; }

void set_auxpcm_sampling_rate(uint8_t mode) {
  int previous_call_state = audio_runtime_state.current_call_state;
  audio_runtime_state.volte_hd_audio_mode = mode;
  if (mode == 1) {
    if (write_to(sysfs_value_pairs[6].path, "16000", O_RDWR) < 0) {
      logger(MSG_ERROR, "%s: Error setting auxpcm_rate to 16k\n", __func__,
             sysfs_value_pairs[6].path);
    }
  } else if (mode == 2) {
    if (write_to(sysfs_value_pairs[6].path, "48000", O_RDWR) < 0) {
      logger(MSG_ERROR, "%s: Error setting auxpcm_rate to 48k\n", __func__,
             sysfs_value_pairs[6].path);
    }
  } else {
    if (write_to(sysfs_value_pairs[6].path, "8000", O_RDWR) < 0) {
      logger(MSG_ERROR, "%s: Error setting auxpcm_rate to 8k\n", __func__,
             sysfs_value_pairs[6].path);
    }
  }
  // If in call, restart audio
  if (audio_runtime_state.current_call_state != CALL_STATUS_IDLE) {
    stop_audio();
    start_audio(previous_call_state);
  }
}

/* Be careful when logging this as phone numbers will leak if you turn on
   debugging */
void handle_call_pkt(uint8_t *pkt, int from, int sz) {
  bool needs_setting_up_paths = false;
  uint8_t direction, state, type, mode;

    /* What are we looking for? A voice service QMI packet with the following
    params:
    - frame 0x01
    - with flag 0x80
    - of service 0x09 (voice svc)
    - and pkt type 0x04
    - and msg ID 0x2e (call indication)
    */
    if (sz > 25 && pkt[0] == 0x1 && pkt[3] == 0x80 && 
        pkt[4] == 0x09 && pkt[6] == 0x04 && pkt[9] == 0x2e) {

      direction = pkt[20];
      state = pkt[18];
      type = pkt[21];

      if (direction == AUDIO_DIRECTION_OUTGOING) {
        logger(MSG_WARN, "%s: Call direction: outgoing \n", __func__);
      } else if (pkt[20] == AUDIO_DIRECTION_INCOMING) {
        logger(MSG_WARN, "%s: Call direction: incoming \n", __func__);
      } else {
        logger(MSG_ERROR, "%s: Unknown call direction! \n", __func__);
      }

      switch (type) {
        case CALL_TYPE_NO_NETWORK:
        case CALL_TYPE_UNKNOWN:
        case CALL_TYPE_GSM:
        case CALL_TYPE_UMTS:
        case CALL_TYPE_UNKNOWN_ALT:
          mode = CALL_STATUS_CS;
          logger(MSG_INFO, "%s: Call type: Circuit Switch \n", __func__);
          break;
        case CALL_TYPE_VOLTE:
          mode = CALL_STATUS_VOLTE;
          logger(MSG_INFO, "%s: Call type: VoLTE \n", __func__);
          break;
        default:
          logger(MSG_ERROR, "%s: Unknown call type \n", __func__);
          break;
      }

      switch (state) { // Call status
        case AUDIO_CALL_PREPARING:
        case AUDIO_CALL_ATTEMPT:
        case AUDIO_CALL_ORIGINATING:
        case AUDIO_CALL_RINGING:
        case AUDIO_CALL_ESTABLISHED:
        case AUDIO_CALL_UNKNOWN:
          logger(MSG_INFO, "%s: Setting up audio for mode %i \n", __func__, mode);
          start_audio(mode);
          break;
        case AUDIO_CALL_ON_HOLD:
        case AUDIO_CALL_WAITING:
          logger(MSG_INFO, "%s: Skipping audio setting (on hold/waiting) %i \n",
                __func__, mode);
          break;
        case AUTIO_CALL_DISCONNECTING:
        case AUDIO_CALL_HANGUP:
          logger(MSG_INFO, "%s: Stopping audio, mode %i \n", __func__, mode);
          stop_audio();
          break;
        default:
          logger(MSG_ERROR, "%s: Unknown call status \n", __func__);
          break;
      }

      logger(MSG_INFO, "%s: Dir: 0x%.2x Sta: 0x%.2x Typ: 0x%.2x, Mode: 0x%.2x \n",
             __func__, direction, state, type, mode);
    } // if packet is call indication, and comes from dsp
} // func

int set_mixer_ctl(struct mixer *mixer, char *name, int value) {
  struct mixer_ctl *ctl;
  ctl = get_ctl(mixer, name);
  int r;

  r = mixer_ctl_set_value(ctl, 1, value);
  if (r < 0) {
    logger(MSG_ERROR, "%s: Setting %s to value %i failed \n", __func__, name,
           value);
  }
  return 0;
}

int stop_audio() {
  if (audio_runtime_state.current_call_state == CALL_STATUS_IDLE) {
    logger(MSG_ERROR, "%s: No call in progress \n", __func__);
    return 1;
  }
  if (pcm_tx == NULL || pcm_rx == NULL) {
    logger(MSG_ERROR, "%s: Invalid PCM, did it fail to open?\n", __func__);
  }
  if (pcm_tx->fd >= 0)
    pcm_close(pcm_tx);
  if (pcm_rx->fd >= 0)
    pcm_close(pcm_rx);

  mixer = mixer_open(SND_CTL);
  if (!mixer) {
    logger(MSG_ERROR, "error opening mixer! %s:\n", strerror(errno), __LINE__);
    return 0;
  }

switch (audio_runtime_state.output_device) {
  case AUDIO_MODE_I2S: // I2S Audio
    // We close all the mixers
    if (audio_runtime_state.current_call_state == 1) {
      set_mixer_ctl(mixer, TXCTL_VOICE, 0); // Playback
      set_mixer_ctl(mixer, RXCTL_VOICE, 0); // Capture
    } else if (audio_runtime_state.current_call_state == 2) {
      set_mixer_ctl(mixer, TXCTL_VOLTE, 0); // Playback
      set_mixer_ctl(mixer, RXCTL_VOLTE, 0); // Capture
    }
    break;
  case AUDIO_MODE_USB: // USB Audio
    // We close all the mixers
    if (audio_runtime_state.current_call_state == 1) {
      set_mixer_ctl(mixer, AFETX_VOICE, 0); // Playback
      set_mixer_ctl(mixer, AFERX_VOICE, 0); // Capture
    } else if (audio_runtime_state.current_call_state == 2) {
      set_mixer_ctl(mixer, AFETX_VOLTE, 0); // Playback
      set_mixer_ctl(mixer, AFERX_VOLTE, 0); // Capture
    }
    break;
  }

  mixer_close(mixer);
  audio_runtime_state.current_call_state = CALL_STATUS_IDLE;
  return 1;
}

/*	Setup mixers and open PCM devs
 *		type: 0: CS Voice Call
 *		      1: VoLTE Call
 * If a call wasn't actually in progress the kernel
 * will complain with ADSP_FAILED / EADSP_BUSY
 */
int start_audio(int type) {
  int i;
  char pcm_device[18];
  if (audio_runtime_state.current_call_state != CALL_STATUS_IDLE &&
      type != audio_runtime_state.current_call_state) {
    logger(MSG_WARN,
           "%s: Switching audio profiles: 0x%.2x --> 0x%.2x\n",
           __func__, audio_runtime_state.current_call_state, type);
    stop_audio();
  } else if (audio_runtime_state.current_call_state != CALL_STATUS_IDLE &&
             type == audio_runtime_state.current_call_state) {
    logger(MSG_INFO,
           "%s: Not doing anything, already set.\n",
           __func__);
    return 0;
  }

  mixer = mixer_open(SND_CTL);
  if (!mixer) {
    logger(MSG_ERROR, "%s: Error opening mixer!\n", __func__);
    return 0;
  }
 switch (audio_runtime_state.output_device) {
  case AUDIO_MODE_I2S:
    switch (type) {
    case 1:
      logger(MSG_DEBUG, "Call in progress: Circuit Switch\n");
      set_mixer_ctl(mixer, TXCTL_VOICE, 1); // Playback
      set_mixer_ctl(mixer, RXCTL_VOICE, 1); // Capture
      strncpy(pcm_device, PCM_DEV_VOCS, sizeof(PCM_DEV_VOCS));
      break;
    case 2:
      logger(MSG_DEBUG, "Call in progress: VoLTE\n");
      set_mixer_ctl(mixer, TXCTL_VOLTE, 1); // Playback
      set_mixer_ctl(mixer, RXCTL_VOLTE, 1); // Capture
      strncpy(pcm_device, PCM_DEV_VOLTE, sizeof(PCM_DEV_VOLTE));
      break;
    default:
      logger(MSG_ERROR, "%s: Can't set mixers, unknown call type %i\n",
             __func__, type);
      return -EINVAL;
    }
    break;

  case AUDIO_MODE_USB: // MODE usb
    switch (type) {
    case 1:
      logger(MSG_DEBUG, "Call in progress: Circuit Switch\n");
      set_mixer_ctl(mixer, AFETX_VOICE, 1); // Playback
      set_mixer_ctl(mixer, AFERX_VOICE, 1); // Capture
      strncpy(pcm_device, PCM_DEV_VOCS, sizeof(PCM_DEV_VOCS));
      break;
    case 2:
      logger(MSG_DEBUG, "Call in progress: VoLTE\n");
      set_mixer_ctl(mixer, AFETX_VOLTE, 1); // Playback
      set_mixer_ctl(mixer, AFERX_VOLTE, 1); // Capture
      strncpy(pcm_device, PCM_DEV_VOLTE, sizeof(PCM_DEV_VOLTE));
      break;
    default:
      logger(MSG_ERROR, "%s: Can't set mixers, unknown call type %i\n",
             __func__, type);
      return -EINVAL;
    }
    break;
  }
  mixer_close(mixer);

  pcm_rx = pcm_open((PCM_IN | PCM_MONO), pcm_device);
  pcm_rx->channels = 1;
  pcm_rx->rate = 8000;
  pcm_rx->flags = PCM_IN | PCM_MONO;

  pcm_tx = pcm_open((PCM_OUT | PCM_MONO), pcm_device);
  pcm_tx->channels = 1;
  pcm_tx->rate = 8000;
  pcm_tx->flags = PCM_OUT | PCM_MONO;

  if (audio_runtime_state.volte_hd_audio_mode == 1) {
    pcm_rx->rate = 16000;
    pcm_tx->rate = 16000;
  } else if (audio_runtime_state.volte_hd_audio_mode == 2) {
    pcm_rx->rate = 48000;
    pcm_tx->rate = 48000;
  } else {
    pcm_rx->rate = 8000;
    pcm_tx->rate = 8000;
  }

  if (set_params(pcm_rx, PCM_IN)) {
    logger(MSG_ERROR, "Error setting RX Params\n");
    pcm_close(pcm_rx);
    return -EINVAL;
  }

  if (set_params(pcm_tx, PCM_OUT)) {
    logger(MSG_ERROR, "Error setting TX Params\n");
    pcm_close(pcm_tx);
    return -EINVAL;
  }

  if (ioctl(pcm_rx->fd, SNDRV_PCM_IOCTL_PREPARE)) {
    logger(MSG_ERROR, "Error getting RX PCM ready\n");
    pcm_close(pcm_rx);
    return -EINVAL;
  }

  if (ioctl(pcm_tx->fd, SNDRV_PCM_IOCTL_PREPARE)) {
    logger(MSG_ERROR, "Error getting TX PCM ready\n");
    pcm_close(pcm_tx);
    return -EINVAL;
  }

  if (ioctl(pcm_tx->fd, SNDRV_PCM_IOCTL_START) < 0) {
    logger(MSG_ERROR, "PCM ioctl start failed for TX\n");
    pcm_close(pcm_tx);
    return -EINVAL;
  }

  if (ioctl(pcm_rx->fd, SNDRV_PCM_IOCTL_START) < 0) {
    logger(MSG_ERROR, "PCM ioctl start failed for RX\n");
    pcm_close(pcm_rx);
  }

  if (type == CALL_STATUS_CS || type == CALL_STATUS_VOLTE) {
    audio_runtime_state.current_call_state = type;
  }

  return 0;
}

int dump_audio_mixer() {
  struct mixer *mixer;
  mixer = mixer_open(SND_CTL);
  if (!mixer) {
    logger(MSG_ERROR, "%s: Error opening mixer!\n", __func__);
    return -1;
  }
  mixer_dump(mixer);
  mixer_close(mixer);
  return 0;
}

int set_audio_defaults() {
  int i;
  int ret = 0;
  for (i = 0; i < (sizeof(sysfs_value_pairs) / sizeof(sysfs_value_pairs[0]));
       i++) {
    if (write_to(sysfs_value_pairs[i].path, sysfs_value_pairs[i].value,
                 O_RDWR) < 0) {
      logger(MSG_ERROR, "%s: Error writing to %s\n", __func__,
             sysfs_value_pairs[i].path);
      ret = -EPERM;
    } else {
      logger(MSG_DEBUG, "%s: Written %s to %s \n", __func__,
             sysfs_value_pairs[i].value, sysfs_value_pairs[i].path);
    }
  }
  return ret;
}
