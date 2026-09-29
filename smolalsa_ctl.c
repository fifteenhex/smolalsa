// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * List the things a card lets you change, and change them.
 *
 *	smolalsa_ctl [-d /dev/snd/controlC0] [list | get NAME | set NAME VALUE]
 *
 * With no command it lists. A value is a number, on or off for a switch, or
 * the name of one of the things a list element can be set to:
 *
 *	smolalsa_ctl set 'Headphone Playback Volume' 200
 *	smolalsa_ctl set 'Mic Capture Switch' on
 *	smolalsa_ctl set 'Capture Source' Mic
 */
#include "smolalsa.h"

#ifndef NOLIBC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#endif

/* Nothing allocates, so the listing brings its own room and says if it ran out */
#define ELEMS		512
#define VALUES		16

static struct snd_ctl_elem_id ids[ELEMS];

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s [-d dev] [list | get NAME | set NAME VALUE]\n", prog);
}

static int isbool(const char *value, long *val)
{
	if (!strcmp(value, "on") || !strcmp(value, "true") || !strcmp(value, "yes") ||
	    !strcmp(value, "1")) {
		*val = 1;
		return 1;
	}

	if (!strcmp(value, "off") || !strcmp(value, "false") || !strcmp(value, "no") ||
	    !strcmp(value, "0")) {
		*val = 0;
		return 1;
	}

	return 0;
}

static int isnumber(const char *value)
{
	unsigned int i = 0;

	if (value[0] == '-' || value[0] == '+')
		i++;

	if (!value[i])
		return 0;

	for (; value[i]; i++)
		if (value[i] < '0' || value[i] > '9')
			return 0;

	return 1;
}

/* One line: what the element is, where it is now, and what else it could be */
static void show(struct smolalsa_ctl *ctl, struct smolalsa_ctl_elem *elem)
{
	struct snd_ctl_elem_value value;
	unsigned int i, n;
	char name[64];

	printf("numid %u '%s' index %u %s count %u", elem->id.numid,
	       (const char *)elem->id.name, elem->id.index,
	       smolalsa_ctl_typename(elem->type), elem->count);

	if (smolalsa_ctl_read(ctl, &elem->id, &value) < 0) {
		printf(" = (cannot read it)\n");
		return;
	}

	n = elem->count > VALUES ? VALUES : elem->count;
	printf(" =");

	for (i = 0; i < n; i++) {
		const char *sep = i ? "," : " ";

		switch (elem->type) {
		case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
			printf("%s%s", sep, value.value.integer.value[i] ? "on" : "off");
			break;
		case SNDRV_CTL_ELEM_TYPE_INTEGER:
			printf("%s%ld", sep, value.value.integer.value[i]);
			break;
		case SNDRV_CTL_ELEM_TYPE_INTEGER64:
			printf("%s%ld", sep, (long)value.value.integer64.value[i]);
			break;
		case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
			if (smolalsa_ctl_item(ctl, &elem->id, value.value.enumerated.item[i],
					      name, sizeof(name)) < 0)
				printf("%s%u", sep, value.value.enumerated.item[i]);
			else
				printf("%s'%s'", sep, name);
			break;
		default:
			printf("%s?", sep);
			break;
		}
	}

	if (elem->count > n)
		printf(",...");

	if (elem->type == SNDRV_CTL_ELEM_TYPE_INTEGER)
		printf(" (%ld to %ld, step %ld)", elem->min, elem->max, elem->step);

	if (elem->type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
		printf(" (");
		for (i = 0; i < elem->items; i++) {
			if (smolalsa_ctl_item(ctl, &elem->id, i, name, sizeof(name)) < 0)
				break;
			printf("%s'%s'", i ? " " : "", name);
		}
		printf(")");
	}

	printf("\n");
}

static int list(struct smolalsa_ctl *ctl)
{
	unsigned int used = 0, total = 0, i;
	int ret;

	ret = smolalsa_ctl_list(ctl, ids, ELEMS, &used, &total);
	if (ret < 0) {
		printf("cannot list the elements (%d)\n", ret);
		return 1;
	}

	printf("%u elements", total);
	if (used < total)
		printf(", of which the first %u fit here", used);
	printf("\n");

	for (i = 0; i < used; i++) {
		struct smolalsa_ctl_elem elem;

		ret = smolalsa_ctl_info(ctl, &ids[i], &elem);
		if (ret < 0) {
			printf("numid %u: cannot read what it is (%d)\n", ids[i].numid, ret);
			continue;
		}

		show(ctl, &elem);
	}

	return 0;
}

static int get(struct smolalsa_ctl *ctl, const char *name)
{
	struct smolalsa_ctl_elem elem;
	int ret;

	ret = smolalsa_ctl_find(ctl, name, &elem);
	if (ret < 0) {
		printf("no element called '%s' (%d)\n", name, ret);
		return 1;
	}

	show(ctl, &elem);

	return 0;
}

static int set(struct smolalsa_ctl *ctl, const char *name, const char *value)
{
	struct smolalsa_ctl_elem elem;
	long val = 0;
	int ret;

	ret = smolalsa_ctl_find(ctl, name, &elem);
	if (ret < 0) {
		printf("no element called '%s' (%d)\n", name, ret);
		return 1;
	}

	switch (elem.type) {
	case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
		if (!isbool(value, &val)) {
			printf("'%s' is a switch, so it takes on or off\n", name);
			return 1;
		}
		ret = smolalsa_ctl_set(ctl, name, val);
		break;
	case SNDRV_CTL_ELEM_TYPE_INTEGER:
	case SNDRV_CTL_ELEM_TYPE_INTEGER64:
		if (!isnumber(value)) {
			printf("'%s' takes a number\n", name);
			return 1;
		}
		ret = smolalsa_ctl_set(ctl, name, atol(value));
		break;
	case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
		/* a bare number is which one, anything else is its name */
		if (isnumber(value))
			ret = smolalsa_ctl_set(ctl, name, atol(value));
		else
			ret = smolalsa_ctl_set_item(ctl, name, value);
		break;
	default:
		printf("'%s' is a %s element, which this does not set\n", name,
		       smolalsa_ctl_typename(elem.type));
		return 1;
	}

	if (ret < 0) {
		printf("cannot set '%s' to %s (%d)\n", name, value, ret);
		return 1;
	}

	show(ctl, &elem);

	return 0;
}

int main(int argc, char **argv, char **envp)
{
	const char *dev = NULL, *cmd;
	struct smolalsa_ctl ctl;
	int opt, ret, args;

	(void)envp;

	while ((opt = getopt(argc, argv, "d:h")) != -1) {
		switch (opt) {
		case 'd':
			dev = optarg;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	args = argc - optind;
	cmd = args ? argv[optind] : "list";

	if ((!strcmp(cmd, "list") && args > 1) || (!strcmp(cmd, "get") && args != 2) ||
	    (!strcmp(cmd, "set") && args != 3)) {
		usage(argv[0]);
		return 1;
	}

	ret = smolalsa_ctl_open(&ctl, dev);
	if (ret) {
		printf("no mixer here (%d)\n", ret);
		return 1;
	}

	if (!strcmp(cmd, "list"))
		ret = list(&ctl);
	else if (!strcmp(cmd, "get"))
		ret = get(&ctl, argv[optind + 1]);
	else if (!strcmp(cmd, "set"))
		ret = set(&ctl, argv[optind + 1], argv[optind + 2]);
	else {
		usage(argv[0]);
		ret = 1;
	}

	smolalsa_ctl_close(&ctl);

	return ret;
}
