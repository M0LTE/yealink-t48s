// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48mix: list or set ALSA mixer controls with the kernel's own ioctls (no
 * alsa-lib). For kernel/drivers/t48s_audio.c on the phone, where there is no
 * amixer.
 *
 *   t48mix                    list every control of card 0 with its value
 *   t48mix "NAME" VALUE       set one control (a number; for a switch 0 or 1)
 *
 * It changes only the control named. The output switches, gains and the
 * loudspeaker amplifier make things audible: the driver starts with them all
 * off and low.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <sound/asound.h>

int main(int argc, char **argv)
{
	int fd = open("/dev/snd/controlC0", O_RDWR), found = 0;
	struct snd_ctl_elem_id ids[64];
	struct snd_ctl_elem_list list;
	unsigned i;

	if (fd < 0) { perror("/dev/snd/controlC0"); return 1; }
	if (argc != 1 && argc != 3) { fprintf(stderr, "usage: t48mix [\"NAME\" VALUE]\n"); return 2; }
	memset(&list, 0, sizeof(list));
	list.space = 64;
	list.pids = ids;
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) { perror("ELEM_LIST"); return 1; }

	for (i = 0; i < list.used; i++) {
		struct snd_ctl_elem_info info;
		struct snd_ctl_elem_value val;
		int is_enum;

		memset(&info, 0, sizeof(info));
		memset(&val, 0, sizeof(val));
		info.id = val.id = ids[i];
		if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) { perror("ELEM_INFO"); return 1; }
		is_enum = info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED;
		if (argc == 3) {
			if (strcmp((char *)ids[i].name, argv[1]))
				continue;
			found = 1;
			if (is_enum)
				val.value.enumerated.item[0] = atoi(argv[2]);
			else
				val.value.integer.value[0] = atoi(argv[2]);
			if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) < 0) { perror("ELEM_WRITE"); return 1; }
		}
		if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) < 0) { perror("ELEM_READ"); return 1; }
		if (is_enum) {
			info.value.enumerated.item = val.value.enumerated.item[0];
			ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info);
			printf("%-32s %u (%s)\n", ids[i].name, val.value.enumerated.item[0], info.value.enumerated.name);
		} else {
			printf("%-32s %ld (%ld to %ld)\n", ids[i].name, val.value.integer.value[0],
			       info.value.integer.min, info.value.integer.max);
		}
	}
	if (argc == 3 && !found) { fprintf(stderr, "no control named \"%s\"\n", argv[1]); return 1; }
	return 0;
}
