/*
   drbdadm_postparse.c actions to do after config parsing

   This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

   Copyright (C) 2012, LINBIT Information Technologies GmbH

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "drbdtool_common.h"
#include "drbdadm.h"

static void inherit_volumes(struct volumes *from, struct d_host_info *host);
static void check_volume_sets_equal(struct d_resource *, struct d_host_info *, struct d_host_info *);

static void append_names(struct names *head, struct names *to_copy)
{
	struct d_name *new, *copy;

	STAILQ_FOREACH(copy, to_copy, link) {
		new = malloc(sizeof(struct d_name));
		new->name = strdup(copy->name);
		insert_tail(head, new);
	}
}

void set_on_hosts_in_res(struct d_resource *res)
{
	struct d_resource *l_res;
	struct d_host_info *host, *host2;
	struct d_name *h;

	for_each_host(host, &res->all_hosts) {
		if (host->lower_name) {
			for_each_resource(l_res, &config) {
				if (!strcmp(l_res->name, host->lower_name))
					break;
			}

			if (l_res == NULL) {
				fprintf(stderr, "%s:%d: in resource %s, "
					"referenced resource '%s' not defined.\n",
					res->config_file, res->start_line, res->name,
					host->lower_name);
				config_valid = 0;
				continue;
			}

			/* Simple: host->on_hosts = concat_names(l_res->me->on_hosts, l_res->peer->on_hosts); */
			for_each_host(host2, &l_res->all_hosts)
				if (!host2->lower_name)
					append_names(&host->on_hosts, &host2->on_hosts);

			host->lower = l_res;

			/* */
			if (!strcmp(host->address.addr, "127.0.0.1") || !strcmp(host->address.addr, "::1"))
				STAILQ_FOREACH(h, &host->on_hosts, link)
					check_uniq("IP", "%s:%s:%s", h->name, host->address.addr, host->address.port);

		}
	}
}

void set_me_in_resource(struct d_resource* res, int match_on_proxy)
{
	struct d_host_info *host;

	/* Determine the local host section */
	for_each_host(host, &res->all_hosts) {
		/* do we match  this host? */
		if (match_on_proxy) {
		       if (!host->proxy || !name_in_names(nodeinfo.nodename, &host->proxy->on_hosts))
			       continue;
		} else if (host->by_address) {
			if (!have_ip(host->address.af, host->address.addr) &&
				/* for debugging only, e.g. __DRBD_NODE__=10.0.0.1 */
			    strcmp(nodeinfo.nodename, host->address.addr))
				continue;
		} else if (host->lower) {
			if (!host->lower->me)
				continue;
		} else if (STAILQ_EMPTY(&host->on_hosts)) {
			/* huh? a resource without hosts to run on?! */
			continue;
		} else {
			if (!name_in_names(nodeinfo.nodename, &host->on_hosts) &&
			    strcmp("_this_host", STAILQ_FIRST(&host->on_hosts)->name))
				continue;
		}
		/* we matched. */
		if (res->ignore) {
			config_valid = 0;
			fprintf(stderr,
				"%s:%d: in resource %s, %s %s { ... }:\n"
				"\tYou cannot ignore and define at the same time.\n",
				res->config_file, host->config_line, res->name,
				host->lower ? "stacked-on-top-of" : "on",
				host->lower ? host->lower->name : names_to_str(&host->on_hosts));
		}
		if (res->me) {
			config_valid = 0;
			fprintf(stderr,
				"%s:%d: in resource %s, %s %s { ... } ... %s %s { ... }:\n"
				"\tThere are multiple host sections for this node.\n",
				res->config_file, host->config_line, res->name,
				res->me->lower ? "stacked-on-top-of" : "on",
				res->me->lower ? res->me->lower->name : names_to_str(&res->me->on_hosts),
				host->lower ? "stacked-on-top-of" : "on",
				host->lower ? host->lower->name : names_to_str(&host->on_hosts));
		}
		res->me = host;
		host->used_as_me = 1;
		if (host->lower)
			res->stacked = 1;
	}

	/* If there is no me, implicitly ignore that resource */
	if (!res->me) {
		res->ignore = 1;
		return;
	}
}


void set_peer_in_resource(struct d_resource* res, int peer_required)
{
	struct d_host_info *host = NULL, *candidate = NULL;
	int nr_hosts = 0, candidates = 0;

	if (res->ignore)
		return;

	/* me must be already set */
	if (!res->me) {
		/* should have been implicitly ignored. */
		fprintf(stderr, "%s:%d: in resource %s:\n"
				"\tcannot determine the peer, don't even know myself!\n",
				res->config_file, res->start_line, res->name);
		exit(E_THINKO);
	}

	for_each_host(host, &res->all_hosts) {
		nr_hosts++;
		if (!host->used_as_me) {
			candidates++;
			candidate = host;
		}
	}

	if (nr_hosts == 1) {
		if (peer_required) {
			fprintf(stderr,
				"%s:%d: in resource %s:\n"
				"\tMissing section 'on <PEER> { ... }'.\n",
				res->config_file, res->start_line, res->name);
			config_valid = 0;
		}
		return;
	}

	/* short cut for exactly two host sections.
	 * silently ignore any --peer connect_to_host option. */
	if (candidates == 1 && nr_hosts == 2) {
		res->peer = candidate;
		res->connect_to = candidate->proxy ? &candidate->proxy->inside : &candidate->address;
		if (dry_run > 1 && connect_to_host)
			fprintf(stderr,
				"%s:%d: in resource %s:\n"
				"\tIgnoring --peer '%s': there are only two host sections.\n",
				res->config_file, res->start_line, res->name, connect_to_host);
		return;
	}

	/* Multiple peer hosts to choose from.
	 * we need some help! */
	if (!connect_to_host) {
		if (peer_required) {
			fprintf(stderr,
				"%s:%d: in resource %s:\n"
				"\tThere are multiple host sections for the peer node.\n"
				"\tUse the --peer option to select which peer section to use.\n",
				res->config_file, res->start_line, res->name);
			config_valid = 0;
		}
		return;
	}

	for_each_host(host, &res->all_hosts) {
		if (host->by_address && strcmp(connect_to_host, host->address.addr))
			continue;
		if (host->proxy && !name_in_names(nodeinfo.nodename, &host->proxy->on_hosts))
			continue;
		if (!name_in_names(connect_to_host, &host->on_hosts))
			continue;

		if (host == res->me) {
			fprintf(stderr,
				"%s:%d: in resource %s\n"
				"\tInvoked with --peer '%s', but that matches myself!\n",
				res->config_file, res->start_line, res->name, connect_to_host);
			res->peer = NULL;
			res->connect_to = NULL;
			break;
		}

		if (res->peer) {
			fprintf(stderr,
				"%s:%d: in resource %s:\n"
				"\tInvoked with --peer '%s', but that matches multiple times!\n",
				res->config_file, res->start_line, res->name, connect_to_host);
			res->peer = NULL;
			res->connect_to = NULL;
			break;
		}
		res->peer = host;
		res->connect_to = host->proxy ? &host->proxy->inside : &host->address;
	}

	if (peer_required && !res->peer) {
		config_valid = 0;
		if (!host)
			fprintf(stderr,
				"%s:%d: in resource %s:\n"
				"\tNo host ('on' or 'floating') section matches --peer '%s'\n",
				res->config_file, res->start_line, res->name, connect_to_host);
	}
}


void set_disk_in_res(struct d_resource *res)
{
	struct d_host_info *host;
	struct d_volume *a, *b;

	if (res->ignore)
		return;

	for_each_host(host, &res->all_hosts) {
		if (!host->lower)
			continue;

		if (res->stacked && host->lower->stacked) {
			fprintf(stderr,
				"%s:%d: in resource %s, stacked-on-top-of %s { ... }:\n"
				"\tFIXME. I won't stack stacked resources.\n",
				res->config_file, res->start_line, res->name, host->lower_name);
			config_valid = 0;
		}

		if (host->lower->ignore)
			continue;

		check_volume_sets_equal(res, host, host->lower->me);
		if (!config_valid)
			/* don't even bother for broken config. */
			continue;

		/* volume lists are sorted on vnr */
		a = STAILQ_FIRST(&host->volumes);
		b = STAILQ_FIRST(&host->lower->me->volumes);
		while (a) {
			while (b && a->vnr > b->vnr) {
				/* Lower resource has more volumes.
				 * Probably unusual, but we decided
				 * that it should be legal.
				 * Skip those that do not match */
				b = STAILQ_NEXT(b, link);
			}
			if (a && b && a->vnr == b->vnr) {
				if (b->device)
					m_asprintf(&a->disk, "%s", b->device);
				else
					m_asprintf(&a->disk, "/dev/drbd%u", b->device_minor);
				/* stacked implicit volumes need internal meta data, too */
				if (!a->meta_disk)
					m_asprintf(&a->meta_disk, "internal");
				if (!a->meta_index)
					m_asprintf(&a->meta_index, "internal");
				a = STAILQ_NEXT(a, link);
				b = STAILQ_NEXT(b, link);
			} else {
				/* config_invalid should have been set
				 * by check_volume_sets_equal */
				assert(0);
			}
		}
	}
}

static struct d_volume *find_volume(struct volumes *volumes, int vnr)
{
	struct d_volume *vol;

	for_each_volume(vol, volumes)
		if (vol->vnr == vnr)
			return vol;

	return NULL;
}

static void derror(struct d_host_info *host, struct d_resource *res, char *text)
{
	config_valid = 0;
	fprintf(stderr, "%s:%d: in resource %s, on %s { ... }:"
		" '%s' keyword missing.\n",
		res->config_file, host->config_line, res->name, names_to_str(&host->on_hosts), text);
}

static void inherit_volumes(struct volumes *from, struct d_host_info *host)
{
	struct d_volume *s, *t;
	struct d_name *h;

	for_each_volume(s, from) {
		t = find_volume(&host->volumes, s->vnr);
		if (!t) {
			t = calloc(1, sizeof(struct d_volume));
			t->device_minor = -1;
			t->vnr = s->vnr;
			insert_volume(&host->volumes, t);
		}
		if (!t->disk && s->disk) {
			t->disk = strdup(s->disk);
			STAILQ_FOREACH(h, &host->on_hosts, link)
				check_uniq("disk", "disk:%s:%s", h->name, t->disk);
		}
		if (!t->device && s->device)
			t->device = strdup(s->device);
		if (t->device_minor == -1U && s->device_minor != -1U) {
			t->device_minor = s->device_minor;
			STAILQ_FOREACH(h, &host->on_hosts, link)
				check_uniq("device-minor", "device-minor:%s:%d", h->name, t->device_minor);
		}
		if (!t->meta_disk && s->meta_disk) {
			t->meta_disk = strdup(s->meta_disk);
			if (s->meta_index)
				t->meta_index = strdup(s->meta_index);
		}
	}
}

static void check_volume_complete(struct d_resource *res, struct d_host_info *host, struct d_volume *vol)
{
	if (!vol->device && vol->device_minor == -1U)
		derror(host, res, "device");
	if (!vol->disk)
		derror(host, res, "disk");
	if (!vol->meta_disk)
		derror(host, res, "meta-disk");
	if (!vol->meta_index)
		derror(host, res, "meta-index");
}

static void check_volumes_complete(struct d_resource *res, struct d_host_info *host)
{
	struct d_volume *vol;
	unsigned vnr = -1U;

	for_each_volume(vol, &host->volumes) {
		if (vnr == -1U || vnr < vol->vnr)
			vnr = vol->vnr;
		else
			fprintf(stderr,
				"internal error: in %s: unsorted volumes list\n",
				res->name);
		check_volume_complete(res, host, vol);
	}
}

static void check_meta_disk(struct d_volume *vol, struct d_host_info *host)
{
	struct d_name *h;
	/* when parsing "drbdsetup show[-all]" output,
	 * a detached volume will only have device/minor,
	 * but no disk or meta disk. */
	if (vol->meta_disk == NULL)
		return;
	if (strcmp(vol->meta_disk, "internal") != 0) {
		/* index either some number, or "flexible" */
		STAILQ_FOREACH(h, &host->on_hosts, link)
			check_uniq("meta-disk", "%s:%s[%s]", h->name, vol->meta_disk, vol->meta_index);
	}
}

static void check_volume_sets_equal(struct d_resource *res, struct d_host_info *host1, struct d_host_info *host2)
{
	struct d_volume *a, *b;

	/* change the error output, if we have been called to
	 * compare stacked with lower resource volumes */
	int compare_stacked = host1->lower && host1->lower->me == host2;

	if (host1 == host2)
		return;

	a = STAILQ_FIRST(&host1->volumes);
	b = STAILQ_FIRST(&host2->volumes);

	/* volume lists are supposed to be sorted on vnr */
	while (a || b) {
		while (a && (!b || a->vnr < b->vnr)) {
			fprintf(stderr,
				"%s:%d: in resource %s, on %s { ... }: "
				"volume %d not defined on %s\n",
				config_file, line, res->name,
				names_to_str(&host1->on_hosts),
				a->vnr,
				compare_stacked ? host1->lower->name
					: names_to_str(&host2->on_hosts));
			a = STAILQ_NEXT(a, link);
			config_valid = 0;
		}
		while (b && (!a || a->vnr > b->vnr)) {
			/* Though unusual, it is "legal" for a lower resource
			 * to have more volumes than the resource stacked on
			 * top of it.  Warn (if we have a terminal),
			 * but consider it as valid. */
			if (!(compare_stacked && no_tty))
				fprintf(stderr,
					"%s:%d: in resource %s, on %s { ... }: "
					"volume %d missing (present on %s)\n",
					config_file, line, res->name,
					names_to_str(&host1->on_hosts),
					b->vnr,
					compare_stacked ? host1->lower->name
						: names_to_str(&host2->on_hosts));
			if (!compare_stacked)
				config_valid = 0;
			b = STAILQ_NEXT(b, link);
		}
		if (a && b && a->vnr == b->vnr) {
			a = STAILQ_NEXT(a, link);
			b = STAILQ_NEXT(b, link);
		}
	}
}

/* Ensure that in all host sections the same volumes are defined */
static void check_volumes_hosts(struct d_resource *res)
{
	struct d_host_info *host1, *host2;

	host1 = STAILQ_FIRST(&res->all_hosts);

	if (!host1)
		return;

	for_each_host(host2, &res->all_hosts)
		check_volume_sets_equal(res, host1, host2);
}

void post_parse(enum pp_flags flags)
{
	struct d_resource *res;

	/* inherit volumes from resource level into the d_host_info objects */
	for_each_resource(res, &config) {
		struct d_host_info *host;
		for_each_host(host, &res->all_hosts) {
			struct d_volume *vol;
			inherit_volumes(&res->volumes, host);

			for_each_volume(vol, &host->volumes)
				check_meta_disk(vol, host);

			if (host->require_all) {
				if (!host->address.addr)
					derror(host, res, "address");
				check_volumes_complete(res, host);
			}
		}

		check_volumes_hosts(res);
	}

	for_each_resource(res, &config)
		if (res->stacked_on_one)
			set_on_hosts_in_res(res); /* sets on_hosts and host->lower */

	/* Needs "on_hosts" and host->lower already set */
	for_each_resource(res, &config)
		if (!res->stacked_on_one)
			set_me_in_resource(res, flags & MATCH_ON_PROXY);

	/* Needs host->lower->me already set */
	for_each_resource(res, &config)
		if (res->stacked_on_one)
			set_me_in_resource(res, flags & MATCH_ON_PROXY);

	// Needs "me" set already
	for_each_resource(res, &config)
		if (res->stacked_on_one)
			set_disk_in_res(res);
}

static void expand_opts(struct options *common, struct options *options)
{
	struct d_option *option, *new_option;

	STAILQ_FOREACH(option, common, link) {
		if (!find_opt(options, option->name)) {
			new_option = new_opt(strdup(option->name),
					     option->value ? strdup(option->value) : NULL);
			insert_head(options, new_option);
		}
	}
}

void expand_common(void)
{
	struct d_resource *res;
	struct d_volume *vol, *host_vol;
	struct d_host_info *h;

	/* make sure vol->device is non-NULL */
	for_each_resource(res, &config) {
		for_each_host(h, &res->all_hosts) {
			for_each_volume(vol, &h->volumes) {
				if (!vol->device)
					m_asprintf(&vol->device, "/dev/drbd%u",
						   vol->device_minor);
			}
		}
	}

	for_each_resource(res, &config) {
		if (!common)
			break;

		expand_opts(&common->net_options, &res->net_options);
		expand_opts(&common->disk_options, &res->disk_options);
		expand_opts(&common->startup_options, &res->startup_options);
		expand_opts(&common->proxy_options, &res->proxy_options);
		expand_opts(&common->handlers, &res->handlers);
		expand_opts(&common->res_options, &res->res_options);

		if (common->stacked_timeouts)
			res->stacked_timeouts = 1;

		if (STAILQ_EMPTY(&res->become_primary_on))
			res->become_primary_on = common->become_primary_on;

		expand_opts(&common->proxy_plugins, &res->proxy_plugins);

	}

	/* now that common disk options (if any) have been propagated to the
	 * resource level, further propagate them to the volume level. */
	for_each_resource(res, &config) {
		for_each_host(h, &res->all_hosts) {
			for_each_volume(vol, &h->volumes) {
				expand_opts(&res->disk_options, &vol->disk_options);
			}
		}
	}

	/* now from all volume/disk-options on resource level to host level */
	for_each_resource(res, &config) {
		for_each_volume(vol, &res->volumes) {
			for_each_host(h, &res->all_hosts) {
				host_vol = volume_by_vnr(&h->volumes, vol->vnr);
				expand_opts(&vol->disk_options, &host_vol->disk_options);
			}
		}
	}
}

static struct d_resource *res_by_name(const char *name)
{
	struct d_resource *res;

	for_each_resource(res, &config) {
		if (strcmp(name, res->name) == 0)
			return res;
	}
	return NULL;
}

static int sanity_check_abs_cmd(char *cmd_name)
{
	struct stat sb;

	if (stat(cmd_name, &sb)) {
		/* If stat fails, just ignore this sanity check,
		 * we are still iterating over $PATH probably. */
		return 0;
	}

	if (!(sb.st_mode & S_ISUID) || sb.st_mode & S_IXOTH || sb.st_gid == 0) {
		static int did_header = 0;
		if (!did_header)
			fprintf(stderr,
				"WARN:\n"
				"  You are using the 'drbd-peer-outdater' as fence-peer program.\n"
				"  If you use that mechanism the dopd heartbeat plugin program needs\n"
				"  to be able to call drbdsetup and drbdmeta with root privileges.\n\n"
				"  You need to fix this with these commands:\n");
		did_header = 1;
		fprintf(stderr,
			"  chgrp haclient %s\n"
			"  chmod o-x %s\n"
			"  chmod u+s %s\n\n", cmd_name, cmd_name, cmd_name);
	}
	return 1;
}

static void sanity_check_cmd(char *cmd_name)
{
	char *path, *pp, *c;
	char abs_path[100];

	if (strchr(cmd_name, '/')) {
		sanity_check_abs_cmd(cmd_name);
	} else {
		path = pp = c = strdup(getenv("PATH"));

		while (1) {
			c = strchr(pp, ':');
			if (c)
				*c = 0;
			snprintf(abs_path, 100, "%s/%s", pp, cmd_name);
			if (sanity_check_abs_cmd(abs_path))
				break;
			if (!c)
				break;
			c++;
			if (!*c)
				break;
			pp = c;
		}
		free(path);
	}
}

/* if the config file is not readable by haclient,
 * dopd cannot work.
 * NOTE: we assume that any gid != 0 will be the group dopd will run as,
 * typically haclient. */
static void sanity_check_conf(char *c)
{
	struct stat sb;

	/* if we cannot stat the config file,
	 * we have other things to worry about. */
	if (stat(c, &sb))
		return;

	/* permissions are funny: if it is world readable,
	 * but not group readable, and it belongs to my group,
	 * I am denied access.
	 * For the file to be readable by dopd (hacluster:haclient),
	 * it is not enough to be world readable. */

	/* ok if world readable, and NOT group haclient (see NOTE above) */
	if (sb.st_mode & S_IROTH && sb.st_gid == 0)
		return;

	/* ok if group readable, and group haclient (see NOTE above) */
	if (sb.st_mode & S_IRGRP && sb.st_gid != 0)
		return;

	fprintf(stderr,
		"WARN:\n"
		"  You are using the 'drbd-peer-outdater' as fence-peer program.\n"
		"  If you use that mechanism the dopd heartbeat plugin program needs\n"
		"  to be able to read the drbd.config file.\n\n"
		"  You need to fix this with these commands:\n"
		"  chgrp haclient %s\n" "  chmod g+r %s\n\n", c, c);
}

static void sanity_check_perm()
{
	static int checked = 0;
	if (checked)
		return;

	sanity_check_cmd(drbdsetup);
	sanity_check_cmd(drbdmeta);
	sanity_check_conf(config_file);
	checked = 1;
}

static void validate_resource(struct d_resource *res)
{
	struct d_option *opt, *next;
	struct d_name *bpo;

	/* there may be more than one "resync-after" statement,
	 * see commit 89cd0585 */
	STAILQ_FOREACH(opt, &res->disk_options, link) {
	  struct d_resource *rs_after_res;
		if (strcmp(opt->name, "resync-after"))
			continue;
		rs_after_res = res_by_name(opt->value);
		if (rs_after_res == NULL || rs_after_res->ignore) {
			fprintf(stderr,
				"%s:%d: in resource %s:\n\tresource '%s' mentioned in "
				"'resync-after' option is not known%s.\n",
				res->config_file, res->start_line, res->name,
				opt->value,
				rs_after_res ? " on this host" : "");
			/* Non-fatal if run from some script.
			 * When deleting resources, it is an easily made
			 * oversight to leave references to the deleted
			 * resources in resync-after statements.  Don't fail on
			 * every pacemaker-induced action, as it would
			 * ultimately lead to all nodes committing suicide. */
			if (no_tty) {
				next = opt;
				STAILQ_REMOVE(&res->disk_options, opt, d_option, link);
				free_opt(opt);
				opt = next;
			} else
				config_valid = 0;
		}
	}
	if (res->ignore)
		return;
	if (!res->me) {
		fprintf(stderr,
			"%s:%d: in resource %s:\n\tmissing section 'on %s { ... }'.\n",
			res->config_file, res->start_line, res->name,
			nodeinfo.nodename);
		config_valid = 0;
	}
	// need to verify that in the discard-node-nodename options only known
	// nodenames are mentioned.
	if ((opt = find_opt(&res->net_options, "after-sb-0pri"))) {
		if (!strncmp(opt->value, "discard-node-", 13)) {
			if (res->peer &&
			    !name_in_names(opt->value + 13, &res->peer->on_hosts)
			    && !name_in_names(opt->value + 13,
					      &res->me->on_hosts)) {
				fprintf(stderr,
					"%s:%d: in resource %s:\n\t"
					"the nodename in the '%s' option is "
					"not known.\n\t"
					"valid nodenames are: '%s %s'.\n",
					res->config_file, res->start_line,
					res->name, opt->value,
					names_to_str(&res->me->on_hosts),
					names_to_str(&res->peer->on_hosts));
				config_valid = 0;
			}
		}
	}

	if ((opt = find_opt(&res->handlers, "fence-peer"))) {
		if (strstr(opt->value, "drbd-peer-outdater"))
			sanity_check_perm();
	}

	opt = find_opt(&res->net_options, "allow-two-primaries");
	if (name_in_names("both", &res->become_primary_on) && opt == NULL) {
		fprintf(stderr,
			"%s:%d: in resource %s:\n"
			"become-primary-on is set to both, but allow-two-primaries "
			"is not set.\n", res->config_file, res->start_line,
			res->name);
		config_valid = 0;
	}

	if (!res->peer)
		set_peer_in_resource(res, 0);

	if (res->peer
	    && ((res->me->proxy == NULL) != (res->peer->proxy == NULL))) {
		fprintf(stderr,
			"%s:%d: in resource %s:\n\t"
			"Either both 'on' sections must contain a proxy subsection, or none.\n",
			res->config_file, res->start_line, res->name);
		config_valid = 0;
	}

	STAILQ_FOREACH(bpo, &res->become_primary_on, link) {
		if (res->peer &&
		    !name_in_names(bpo->name, &res->me->on_hosts) &&
		    !name_in_names(bpo->name, &res->peer->on_hosts) &&
		    strcmp(bpo->name, "both")) {
			fprintf(stderr,
				"%s:%d: in resource %s:\n\t"
				"become-primary-on contains '%s', which is not named with the 'on' sections.\n",
				res->config_file, res->start_line, res->name,
				bpo->name);
			config_valid = 0;
		}
	}
}

static int ctx_set_implicit_volume(struct cfg_ctx *ctx)
{
	struct d_volume *vol, *v;
	int volumes = 0;

	if (ctx->vol || !ctx->res)
		return 0;

	if (!ctx->res->me) {
		return 0;
	}

	for_each_volume(vol, &ctx->res->me->volumes) {
		volumes++;
		v = vol;
	}

	if (volumes == 1)
		ctx->vol = v;

	return volumes;
}

// Need to convert after from resourcename to minor_number.
static void _convert_after_option(struct d_resource *res, struct d_volume *vol)
{
	struct d_option *opt, *next;
	struct cfg_ctx depends_on_ctx = { };
	int volumes;

	if (res == NULL)
		return;

	STAILQ_FOREACH(opt, &vol->disk_options, link) {
		if (strcpy(opt->name, "resync-after"))
			continue;
		ctx_by_name(&depends_on_ctx, opt->value);
		volumes = ctx_set_implicit_volume(&depends_on_ctx);
		if (volumes > 1) {
			fprintf(stderr,
				"%s:%d: in resource %s:\n\t"
				"resync-after contains '%s', which is ambiguous, since it contains %d volumes\n",
				res->config_file, res->start_line, res->name,
				opt->value, volumes);
			config_valid = 0;
			return;
		}

		if (!depends_on_ctx.res || depends_on_ctx.res->ignore) {
			next = STAILQ_NEXT(opt, link);
			STAILQ_REMOVE(&vol->disk_options, opt, d_option, link);
			free_opt(opt);
			opt = next;
		} else {
			free(opt->value);
			m_asprintf(&opt->value, "%d", depends_on_ctx.vol->device_minor);
		}
	}
}

// Need to convert after from resourcename/volume to minor_number.
static void convert_after_option(struct d_resource *res)
{
	struct d_volume *vol;
	struct d_host_info *h;

	for_each_host(h, &res->all_hosts)
		for_each_volume(vol, &h->volumes)
			_convert_after_option(res, vol);
}

// need to convert discard-node-nodename to discard-local or discard-remote.
static void convert_discard_opt(struct d_resource *res)
{
	struct d_option *opt;

	if (res == NULL)
		return;

	if ((opt = find_opt(&res->net_options, "after-sb-0pri"))) {
		if (!strncmp(opt->value, "discard-node-", 13)) {
			if (!strcmp(nodeinfo.nodename, opt->value + 13)) {
				free(opt->value);
				opt->value = strdup("discard-local");
			} else {
				free(opt->value);
				opt->value = strdup("discard-remote");
			}
		}
	}
}

void global_validate_maybe_expand_die_if_invalid(int expand)
{
	struct d_resource *res;
	for_each_resource(res, &config) {
		validate_resource(res);
		if (!config_valid)
			exit(E_CONFIG_INVALID);
		if (expand) {
			convert_after_option(res);
			convert_discard_opt(res);
		}
		if (!config_valid)
			exit(E_CONFIG_INVALID);
	}
}
