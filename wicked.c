/*
 * No REST for the wicked!
 *
 * This command line utility provides an interface to the network
 * configuration/information facilities.
 *
 * It uses a RESTful interface (even though it's a command line utility).
 * The idea is to make it easier to extend this to some smallish daemon
 * with a AF_LOCAL socket interface.
 *
 * Copyright (C) 2010 Olaf Kirch <okir@suse.de>
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <mcheck.h>
#include <stdlib.h>
#include <getopt.h>

#include <wicked/netinfo.h>
#include <wicked/logging.h>
#include <wicked/wicked.h>
#include <wicked/addrconf.h>
#include <wicked/bonding.h>
#include <wicked/bridge.h>
#include <wicked/xml.h>
#include <wicked/xpath.h>

enum {
	OPT_CONFIGFILE,
	OPT_DEBUG,
	OPT_DRYRUN,
	OPT_ROOTDIR,
};

static struct option	options[] = {
	{ "config",		required_argument,	NULL,	OPT_CONFIGFILE },
	{ "dryrun",		no_argument,		NULL,	OPT_DRYRUN },
	{ "dry-run",		no_argument,		NULL,	OPT_DRYRUN },
	{ "debug",		required_argument,	NULL,	OPT_DEBUG },
	{ "root-directory",	required_argument,	NULL,	OPT_ROOTDIR },

	{ NULL }
};

static int	opt_dryrun = 0;
static char *	opt_rootdir = NULL;

static int	do_rest(const char *, int, char **);
static int	do_xpath(int, char **);
static int	do_ifup(int, char **);
static int	do_ifdown(int, char **);
static void	clear_line(FILE *);

int
main(int argc, char **argv)
{
	char *cmd;
	int c;

	mtrace();
	while ((c = getopt_long(argc, argv, "+", options, NULL)) != EOF) {
		switch (c) {
		default:
		usage:
			fprintf(stderr,
				"./wicked [options] cmd path\n"
				"This command understands the following options\n"
				"  --config filename\n"
				"        Use alternative configuration file.\n"
				"  --dry-run\n"
				"        Do not change the system in any way.\n"
				"  --debug facility\n"
				"        Enable debugging for debug <facility>.\n"
				"\n"
				"Supported commands:\n"
				"  ifup [--boot] [--file xmlspec] ifname\n"
				"  ifdown [--delete] ifname\n"
				"  get /config/interface\n"
				"  get /config/interface/ifname\n"
				"  put /config/interface/ifname < cfg.xml\n"
				"  get /system/interface\n"
				"  get /system/interface/ifname\n"
				"  put /system/interface/ifname < cfg.xml\n"
				"  delete /config/interface/ifname\n"
				"  delete /system/interface/ifname\n"
				"  xpath [options] expr ...\n"
			       );
			return 1;

		case OPT_CONFIGFILE:
			ni_set_global_config_path(optarg);
			break;

		case OPT_DRYRUN:
			opt_dryrun = 1;
			break;

		case OPT_ROOTDIR:
			opt_rootdir = optarg;
			break;

		case OPT_DEBUG:
			if (!strcmp(optarg, "help")) {
				printf("Supported debug facilities:\n");
				ni_debug_help(stdout);
				return 0;
			}
			if (ni_enable_debug(optarg) < 0) {
				fprintf(stderr, "Bad debug facility \"%s\"\n", optarg);
				return 1;
			}
			break;

		}
	}

	if (ni_init() < 0)
		return 1;

	if (optind >= argc) {
		fprintf(stderr, "Missing command\n");
		goto usage;
	}

	cmd = argv[optind++];

	if (!strcmp(cmd, "xpath"))
		return do_xpath(argc - optind + 1, argv + optind - 1);

	if (!strcmp(cmd, "ifup"))
		return do_ifup(argc - optind + 1, argv + optind - 1);

	if (!strcmp(cmd, "ifdown"))
		return do_ifdown(argc - optind + 1, argv + optind - 1);

	if (!strcmp(cmd, "get")
	 || !strcmp(cmd, "put")
	 || !strcmp(cmd, "post")
	 || !strcmp(cmd, "delete"))
		return do_rest(cmd, argc - optind + 1, argv + optind - 1);

	fprintf(stderr, "Unsupported command %s\n", cmd);
	goto usage;
}

static int
__wicked_request(int rest_op, const char *path,
		/* const */ xml_node_t *send_xml,
		xml_node_t **recv_xml)
{
	ni_wicked_request_t req;
	int rv;

	if (opt_dryrun && rest_op != NI_REST_OP_GET) {
		printf("Would send request %s %s\n",
				ni_wicked_rest_op_print(rest_op), path);
		if (send_xml)
			xml_node_print(send_xml, stdout);
		return 0;
	}

	ni_wicked_request_init(&req);
	req.cmd = rest_op;
	req.path = strdup(path);
	req.xml_in = send_xml;

	if (opt_rootdir)
		ni_wicked_request_add_option(&req, "Root", opt_rootdir);

	rv = ni_wicked_call_indirect(&req);
	if (rv < 0) {
		ni_error("%s", req.error_msg);
	} else if (recv_xml) {
		*recv_xml = req.xml_out;
		req.xml_out = NULL;
	}

	ni_wicked_request_destroy(&req);
	return rv;
}

xml_node_t *
wicked_get(const char *path)
{
	xml_node_t *out = NULL;

	if (__wicked_request(NI_REST_OP_GET, path, NULL, &out) < 0)
		return NULL;

	return out;
}

xml_node_t *
wicked_get_interface(const char *base_path, const char *ifname)
{
	char pathbuf[256];

	snprintf(pathbuf, sizeof(pathbuf), "%s/%s", base_path, ifname);
	return wicked_get(pathbuf);
}

int
wicked_put(const char *path, xml_node_t *in)
{
	return __wicked_request(NI_REST_OP_PUT, path, in, NULL);
}

int
wicked_put_interface(const char *base_path, const char *ifname, xml_node_t *in)
{
	xml_node_t *out = NULL;
	char pathbuf[256];

	snprintf(pathbuf, sizeof(pathbuf), "%s/%s", base_path, ifname);
	if (__wicked_request(NI_REST_OP_PUT, pathbuf, in, &out) < 0)
		return -1;

	if (out) {
		xml_node_print(out, stdout);
		xml_node_free(out);
	}

	return 0;
}

int
wicked_delete_interface(const char *base_path, const char *ifname)
{
	char pathbuf[256];

	snprintf(pathbuf, sizeof(pathbuf), "%s/%s", base_path, ifname);
	if (__wicked_request(NI_REST_OP_DELETE, pathbuf, NULL, NULL) < 0)
		return -1;

	return 0;
}

enum {
	OPER_IDLE,
	OPER_CONFIGURE,
	OPER_WAIT
};
enum {
	STATE_UNKNOWN = 0,
	STATE_DEVICE_DOWN,
	STATE_DEVICE_UP,
	STATE_LINK_UP,
	STATE_NETWORK_UP,

	STATE_ERROR,
	STATE_DONE
};

/*
 * Interface state information
 */
typedef struct ni_interface_state ni_interface_state_t;

typedef struct ni_interface_state_array {
	unsigned int		count;
	ni_interface_state_t **	data;
} ni_interface_state_array_t;

struct ni_interface_state {
	unsigned int		refcount;

	int			state;
	char *			ifname;
	ni_interface_t *	config;
	unsigned int		is_slave : 1,
				waiting  : 1;

	int			next_state;
	int			want_state;
	int			have_state;
	unsigned int		timeout;
	ni_ifaction_t		behavior;

	ni_interface_state_t *	parent;
	ni_interface_state_array_t children;
};

static ni_interface_state_t *ni_interface_state_array_find(ni_interface_state_array_t *, const char *);
static void	ni_interface_state_array_append(ni_interface_state_array_t *, ni_interface_state_t *);
static void	ni_interface_state_array_destroy(ni_interface_state_array_t *);

static ni_interface_state_t *
ni_interface_state_new(const char *name, ni_interface_t *dev)
{
	ni_interface_state_t *state;

	state = calloc(1, sizeof(*state));
	ni_string_dup(&state->ifname, name);
	if (dev)
		state->config = ni_interface_get(dev);
	return state;
}

ni_interface_state_t *
ni_interface_state_add_child(ni_interface_state_t *parent, const char *slave_name, ni_interface_t *slave_dev)
{
	ni_interface_state_t *slave_state;

	if ((slave_state = ni_interface_state_array_find(&parent->children, slave_name)) == NULL) {
		slave_state = ni_interface_state_new(slave_name, slave_dev);
		ni_interface_state_array_append(&parent->children, slave_state);
	}

	if (parent->behavior.mandatory)
		slave_state->behavior.mandatory = 1;
	slave_state->is_slave = 1;

	return slave_state;
}

static void
ni_interface_state_free(ni_interface_state_t *state)
{
	ni_string_free(&state->ifname);
	if (state->config)
		ni_interface_put(state->config);
	ni_interface_state_array_destroy(&state->children);
}

static inline void
ni_interface_state_release(ni_interface_state_t *state)
{
	if (--(state->refcount) == 0)
		ni_interface_state_free(state);
}

static void
ni_interface_state_array_append(ni_interface_state_array_t *array, ni_interface_state_t *state)
{
	array->data = realloc(array->data, (array->count + 1) * sizeof(array->data[0]));
	array->data[array->count++] = state;
	state->refcount++;
}

static ni_interface_state_t *
ni_interface_state_array_find(ni_interface_state_array_t *array, const char *ifname)
{
	unsigned int i;

	for (i = 0; i < array->count; ++i) {
		ni_interface_state_t *state = array->data[i];

		if (!strcmp(state->ifname, ifname))
			return state;
	}
	return NULL;
}

static void
ni_interface_state_array_destroy(ni_interface_state_array_t *array)
{
	while (array->count)
		ni_interface_state_release(array->data[--(array->count)]);
	free(array->data);
	array->data = NULL;
}

static int
ni_build_partial_topology2(ni_handle_t *config, int filter, ni_evaction_t ifaction, ni_interface_state_array_t *result)
{
	ni_interface_state_array_t state_array = { 0, NULL };
	ni_interface_t *pos = NULL, *ifp;
	int want_state;

	want_state = (ifaction == NI_INTERFACE_START)? STATE_NETWORK_UP : STATE_DEVICE_DOWN;

	for (ifp = ni_interface_first(config, &pos); ifp; ifp = ni_interface_next(config, &pos)) {
		const ni_ifaction_t *ifa = &ifp->startmode.ifaction[filter];
		ni_interface_state_t *state;

#if 0
		static const unsigned int updownmask = NI_IFF_NETWORK_UP | NI_IFF_LINK_UP | NI_IFF_DEVICE_UP;
		ifp->ifflags &= ~updownmask;
		ifp->ifflags |= flmask;
#endif

		if (ifa->action != ifaction)
			continue;

		state = ni_interface_state_new(ifp->name, ifp);
		state->want_state = want_state;
		state->behavior = *ifa;

		ni_interface_state_array_append(&state_array, state);
	}

	for (ifp = ni_interface_first(config, &pos); ifp; ifp = ni_interface_next(config, &pos)) {
		ni_interface_state_t *master_state, *slave_state;
		ni_interface_t *slave;
		const char *slave_name;
		ni_bonding_t *bond;
		ni_bridge_t *bridge;
		ni_vlan_t *vlan;
		unsigned int i;

		master_state = ni_interface_state_array_find(&state_array, ifp->name);
		assert(master_state->config == ifp);

		switch (ifp->type) {
		case NI_IFTYPE_VLAN:
			if ((vlan = ifp->vlan) == NULL)
				continue;

			slave_name = vlan->interface_name;

			slave = ni_interface_by_name(config, slave_name);
			if (slave != NULL) {
				/* VLANs are special, real device must be an ether device,
				 * and can be referenced by more than one vlan */
				if (slave->type != NI_IFTYPE_ETHERNET) {
					ni_error("vlan interface %s references non-ethernet device", ifp->name);
					goto failed;
				}
			}

			slave_state = ni_interface_state_add_child(master_state, slave_name, slave);
			if (slave_state->parent)
				goto multiple_masters;
			slave_state->want_state = want_state;
			break;

		case NI_IFTYPE_BOND:
			if ((bond = ifp->bonding) == NULL)
				continue;

			for (i = 0; i < bond->slave_names.count; ++i) {
				slave_name = bond->slave_names.data[i];

				slave = ni_interface_by_name(config, slave_name);

				slave_state = ni_interface_state_add_child(master_state, slave_name, slave);
				if (slave_state->parent)
					goto multiple_masters;
				slave_state->parent = master_state;

				/* Whatever we do, bonding slave devices should never have their network configured. */
				slave_state->want_state = (want_state == STATE_NETWORK_UP)? STATE_LINK_UP : want_state;
			}
			break;

		case NI_IFTYPE_BRIDGE:
			if ((bridge = ifp->bridge) == NULL)
				continue;

			for (i = 0; i < bridge->ports.count; ++i) {
				ni_bridge_port_t *port = bridge->ports.data[i];

				slave = ni_interface_by_name(config, port->name);

				slave_state = ni_interface_state_add_child(master_state, slave_name, slave);
				if (slave_state->parent)
					goto multiple_masters;
				slave_state->parent = master_state;
				slave_state->want_state = want_state;
			}
			break;

		default:
			break;

		multiple_masters:
			ni_error("interface %s used by more than one device (%s and %s)",
					slave_state->ifname, master_state->ifname,
					slave_state->parent->ifname);
		failed:
			ni_interface_state_array_destroy(&state_array);
			return -1;
		}
	}

	{
		unsigned int i;

		for (i = 0; i < state_array.count; ++i) {
			ni_interface_state_t *state = state_array.data[i];

			if (!state->is_slave)
				ni_interface_state_array_append(result, state);
		}
	}

	ni_interface_state_array_destroy(&state_array);
	return 0;
}

/*
 * Check whether we have all requested leases
 */
static int
ni_interface_network_is_up_afinfo(const char *ifname, const ni_afinfo_t *cfg_afi, const ni_afinfo_t *cur_afi)
{
	unsigned int type;

	for (type = 0; type < __NI_ADDRCONF_MAX; ++type) {
		if (type == NI_ADDRCONF_STATIC || !ni_afinfo_addrconf_test(cfg_afi, type))
			continue;
		if (!ni_afinfo_addrconf_test(cur_afi, type)) {
			ni_debug_wicked("%s: addrconf mode %s/%s not enabled", ifname,
					ni_addrfamily_type_to_name(cfg_afi->family),
					ni_addrconf_type_to_name(type));
			return 0;
		}
		if (!ni_addrconf_lease_is_valid(cur_afi->lease[type])) {
			ni_debug_wicked("%s: addrconf mode %s/%s enabled; no lease yet", ifname,
					ni_addrfamily_type_to_name(cfg_afi->family),
					ni_addrconf_type_to_name(type));
			return 0;
		}
	}

	return 1;
}

static int
ni_interface_state(const ni_interface_t *have, const ni_interface_t *want, int want_state)
{
	int have_state = STATE_DEVICE_DOWN;

	if (!(have->ifflags & NI_IFF_DEVICE_UP))
		goto out;

	if (want != NULL) {
		/* FIXME: here we should check the ethernet/link/bond/bridge
		 * composition. */
	}

	have_state = STATE_DEVICE_UP;
	if (!(have->ifflags & NI_IFF_LINK_UP))
		goto out;

	have_state = STATE_LINK_UP;
	if (!(have->ifflags & NI_IFF_NETWORK_UP))
		goto out;

	if (want != NULL) {
		if (!ni_interface_network_is_up_afinfo(want->name, &want->ipv4, &have->ipv4)
		 || !ni_interface_network_is_up_afinfo(want->name, &want->ipv6, &have->ipv6))
			goto out;
	}

	have_state = STATE_NETWORK_UP;

out:
	return have_state;
}

static int
ni_topology_update(ni_interface_state_array_t *state_array, ni_handle_t *system, unsigned int waited)
{
	unsigned int i;
	int rv, ready = 1;

	ni_debug_wicked("%s() called", __FUNCTION__);
	for (i = 0; i < state_array->count; ++i) {
		ni_interface_state_t *state = state_array->data[i];
		ni_interface_t *ifp;
		unsigned int ifflags = 0;

		if (state->have_state == STATE_ERROR || state->have_state == STATE_DONE)
			continue;

		/* Interface may not be present yet (eg for bridge or bond interfaces) */
		if ((ifp = ni_interface_by_name(system, state->ifname)) != NULL) {
			int new_state = ni_interface_state(ifp, state->config, state->want_state);

			if (state->have_state != new_state)
				ni_debug_wicked("%s: state changed from %d to %d",
						state->ifname, state->have_state, new_state);
			state->have_state = new_state;
		}

		ni_debug_wicked("%s: state is %d%s", state->ifname, state->have_state,
				state->waiting? ", waiting": "");

		/* Were we waiting to get to the next state? */
		if (state->waiting) {
			if (state->next_state != state->have_state) {
				ready = 0;
				continue;
			}
			state->waiting = 0;
		}

		if (state->want_state == STATE_DEVICE_DOWN) {
			/* Bring down devices - first shut down parent device, then
			 * go for the subordinate devices. */
			if (state->want_state == state->have_state) {
				rv = ni_topology_update(&state->children, system, waited);
				if (rv < 0)
					return -1;
				if (rv == 0)
					ready = 0;
				continue;
			}
		} else {
			/* Bring up devices - first bring up subordinate devices,
			 * then do the parent devices.
			 * FIXME: Even if the device is up, we should send it our
			 * desired configuration at least once!
			 */
			if (state->want_state == state->have_state)
				continue;

			if (state->children.count) {
				rv = ni_topology_update(&state->children, system, waited);
				if (rv < 0)
					return -1;
				if (rv == 0) {
					ready = 0;
					continue;
				}
			}
		}

		ready = 0;
		if (state->waiting) {
			if (waited > state->timeout) {
				ni_error("%s: timed out", state->ifname);
				state->have_state = STATE_ERROR;
				continue;
			}
			ni_debug_wicked("%s: still waiting", state->ifname);
			continue;
		}

		/* Try to reach the next state, which by default is our
		 * final desired state.
		 * The sole exception is if we want to bring up a device which
		 * is down; here we first go to an intermediate state to see
		 * whether the link is ready.
		 */
		state->next_state = state->want_state;
		state->timeout = state->behavior.wait;

		switch (state->want_state) {
		case STATE_DEVICE_DOWN:
			ni_debug_wicked("%s: trying to shut down device", state->ifname);
			ifflags = 0;
			break;

		case STATE_DEVICE_UP:
			ni_debug_wicked("%s: trying to bring device up", state->ifname);
			ifflags = NI_IFF_DEVICE_UP;
			break;

		case STATE_LINK_UP:
		case STATE_NETWORK_UP:
			if (state->have_state == STATE_DEVICE_DOWN) {
				ni_interface_t *cfg;

				/* Note, we do NOT use the final interface config, but just modify
				 * the current config (from device down to device up). */
				ni_debug_wicked("%s: trying to bring device up", state->ifname);
				state->next_state = STATE_LINK_UP;
				state->timeout = 10;
				ifflags |= NI_IFF_DEVICE_UP;

				/* Send the device our desired device configuration.
				 * This includes VLAN, bridge and bonding topology info, as
				 * well as ethtool settings etc */
				if ((cfg = state->config) != NULL) {
					if (cfg->ethernet) {
						if (ifp->ethernet)
							ni_ethernet_free(ifp->ethernet);
						ifp->ethernet = ni_ethernet_clone(cfg->ethernet);
					}
					if (cfg->vlan) {
						if (ifp->vlan)
							ni_vlan_free(ifp->vlan);
						ifp->vlan = ni_vlan_clone(cfg->vlan);
					}
					if (cfg->bridge) {
						if (ifp->bridge)
							ni_bridge_free(ifp->bridge);
						ifp->bridge = ni_bridge_clone(cfg->bridge);
					}
					if (cfg->bonding) {
						if (ifp->bonding)
							ni_bonding_free(ifp->bonding);
						ifp->bonding = ni_bonding_clone(cfg->bonding);
					}
				}
			} else if (state->want_state == STATE_LINK_UP) {
				ni_debug_wicked("%s: trying to bring link up", state->ifname);
				ifflags = NI_IFF_DEVICE_UP | NI_IFF_LINK_UP;
			} else {
				ni_debug_wicked("%s: trying to bring network up", state->ifname);
				ifflags = NI_IFF_DEVICE_UP | NI_IFF_LINK_UP | NI_IFF_NETWORK_UP;
			}
			break;

		default:
			ni_error("%s: bad want_state=%d", state->ifname, state->want_state);
			return -1;
		}

		/* If we're trying to go to the final desired state, use the config
		 * object if there is one. */
		if (state->next_state == state->want_state && state->config)
			ifp = state->config;
		if (ifp == NULL) {
			ni_error("%s: unknown device", state->ifname);
			return -1;
		}

		ifp->ifflags &= ~(NI_IFF_DEVICE_UP | NI_IFF_LINK_UP | NI_IFF_NETWORK_UP);
		ifp->ifflags |= ifflags;

		if (ni_interface_configure(system, ifp, NULL) < 0) {
			ni_error("%s: unable to configure", ifp->name);
			return -1;
		}

		state->waiting = 1;
		if (state->timeout == 0)
			state->timeout = 10;
	}
	return ready;
}

static int
ni_interfaces_wait2(ni_handle_t *system, ni_interface_state_array_t *state_array)
{
	unsigned int waited = 0, dots = 0;
	int rv;

	setvbuf(stdout, NULL,_IONBF,  0);
	while (1) {
		if ((rv = ni_refresh(system)) < 0) {
			ni_error("failed to get system state");
			return -1;
		}

		if (ni_topology_update(state_array, system, waited))
			return 0;

#if 0
		for (i = 0; i < iflist->count; ++i) {
			ni_interface_t *ifp = iflist->data[i];
			ni_ifaction_t *ifa = &ifp->startmode.ifaction[ifevent];

			if (ifa->wait == 0)
				continue;
			if (action == NI_INTERFACE_START) {
				/* We're trying to start the interface. */
				if (ni_interface_ensure_up(system, ifp)) {
					printf("\r"); clear_line(stdout);
					printf("%s: up\n", ifp->name);
					ifa->wait = 0;
					continue;
				}
			} else {
				/* We're trying to stop the interface */
				/* To be done */
				ifa->wait = 0;
				continue;
			}

			if (dots == 0) {
				printf("%s: ", ifp->name);
				dots = strlen(ifp->name + 2);
			}

			if (ifa->wait < waited) {
				printf("\r"); clear_line(stdout);
				ni_error("%s: failed to come up", ifp->name);
				ifa->wait = 0;
				if (ifa->mandatory)
					rv = -1;
				continue;
			}
			wait++;
		}

		if (!wait)
			break;
#endif

		sleep(1);

		printf(".");
		dots++;
		waited++;
	}

	return 0;
}

static int
ni_build_partial_topology(ni_handle_t *config, ni_evaction_t ifaction)
{
	static const unsigned int updownmask = NI_IFF_NETWORK_UP | NI_IFF_LINK_UP | NI_IFF_DEVICE_UP;
	ni_interface_t *pos = NULL, *ifp;
	unsigned int flmask;

	flmask = (ifaction == NI_INTERFACE_START)? updownmask : 0;
	for (ifp = ni_interface_first(config, &pos); ifp; ifp = ni_interface_next(config, &pos)) {
		ifp->ifflags &= ~updownmask;
		ifp->ifflags |= flmask;
	}

	for (ifp = ni_interface_first(config, &pos); ifp; ifp = ni_interface_next(config, &pos)) {
		ni_interface_t *slave;
		const char *slave_name;
		ni_bonding_t *bond;
		ni_bridge_t *bridge;
		ni_vlan_t *vlan;
		unsigned int i;

		switch (ifp->type) {
		case NI_IFTYPE_VLAN:
			if ((vlan = ifp->vlan) == NULL)
				continue;

			slave_name = vlan->interface_name;
			if (!(slave = ni_interface_by_name(config, slave_name)))
				continue;

			/* VLANs are special, real device must be an ether device,
			 * and can be referenced by more than one vlan */
			if (slave->type != NI_IFTYPE_ETHERNET) {
				ni_error("vlan interface %s references non-ethernet device", ifp->name);
				goto failed;
			}
			if (slave->parent && slave->parent->type != NI_IFTYPE_VLAN)
				goto multiple_masters;
			slave->parent = ifp;

			vlan->interface_dev = slave;
			break;

		case NI_IFTYPE_BOND:
			if ((bond = ifp->bonding) == NULL)
				continue;

			for (i = 0; i < bond->slave_names.count; ++i) {
				slave_name = bond->slave_names.data[i];

				slave = ni_interface_by_name(config, slave_name);
				ni_interface_array_append(&bond->slave_devs, slave);

				if (slave) {
					if (slave->parent)
						goto multiple_masters;
					slave->parent = ifp;

					/* Whatever we do, bonding slave devices should never
					 * have their network configured. */
					slave->ifflags &= ~NI_IFF_NETWORK_UP;
				}
			}
			break;

		case NI_IFTYPE_BRIDGE:
			if ((bridge = ifp->bridge) == NULL)
				continue;

			for (i = 0; i < bridge->ports.count; ++i) {
				ni_bridge_port_t *port = bridge->ports.data[i];

				if (!(slave = ni_interface_by_name(config, port->name)))
					continue;

				port->device = ni_interface_get(slave);

				if (slave->parent)
					goto multiple_masters;
				slave->parent = ifp;
			}
			break;

		default:
			break;

		multiple_masters:
			ni_error("interface %s used by more than one device (%s and %s)",
					slave->name, ifp->name, slave->parent->name);
		failed:
			return -1;
		}
	}

	return 0;
}

/*
 * Flatten the interface topology, putting subordinate interfaces
 * before aggregations like bridges, bonds and vlans.
 */
static int
__ni_interface_flatten(ni_interface_t *ifp, ni_interface_array_t *out)
{
	unsigned int i;

	if (ni_interface_array_index(out, ifp) >= 0)
		return 0;

	ni_interface_array_append(out, ifp);

	switch (ifp->type) {
	case NI_IFTYPE_VLAN:
		if (ifp->vlan) {
			ni_vlan_t *vlan = ifp->vlan;

			if (vlan->interface_dev)
				__ni_interface_flatten(vlan->interface_dev, out);
		}
		break;

	case NI_IFTYPE_BOND:
		if (ifp->bonding != NULL) {
			ni_bonding_t *bond = ifp->bonding;

			for (i = 0; i < bond->slave_devs.count; ++i) {
				ni_interface_t *slave = bond->slave_devs.data[i];

				if (slave)
					ni_interface_array_append(out, slave);
			}
		}
		break;

	case NI_IFTYPE_BRIDGE:
		if (ifp->bridge == NULL) {
			ni_bridge_t *bridge = ifp->bridge;

			for (i = 0; i < bridge->ports.count; ++i) {
				ni_bridge_port_t *port = bridge->ports.data[i];

				ni_interface_array_append(out, port->device);
			}
		}
		break;

	default:
		break;

	}

	return 0;
}

static int
__ni_interfact_filter(const ni_interface_t *ifp, int filter, ni_evaction_t action)
{
	if (filter < 0)
		return 1;
	return ifp->startmode.ifaction[filter].action == action;
}

static int
ni_interface_topology_flatten(ni_handle_t *config, ni_interface_array_t *out, int filter, ni_evaction_t action)
{
	ni_interface_t *pos = NULL, *ifp;

	for (ifp = ni_interface_first(config, &pos); ifp; ifp = ni_interface_next(config, &pos)) {
		if (ifp->type == NI_IFTYPE_LOOPBACK && __ni_interfact_filter(ifp, filter, action))
			ni_interface_array_append(out, ifp);
	}
	for (ifp = ni_interface_first(config, &pos); ifp; ifp = ni_interface_next(config, &pos)) {
		if (ifp->type == NI_IFTYPE_LOOPBACK || !__ni_interfact_filter(ifp, filter, action))
			continue;

		__ni_interface_flatten(ifp, out);
	}
	return 0;
}

/*
 * Check whether interface is up
 */
static int
ni_interface_ensure_up(ni_handle_t *system, const ni_interface_t *cfg)
{
	ni_interface_t *cur;

	/* Ignore subordinate interfaces (bridge ports, bond members) for now */
	if (cfg->parent)
		return 1;

	/* The interface pointer we were given contains the *config* view
	 * of the interface, ie the state we want it to be in.
	 * To inspect the current (system) state, we need to look it up
	 * first. */
	cur = ni_interface_by_name(system, cfg->name);
	if (cur == NULL)
		return 0;

	if ((cfg->ifflags ^ cur->ifflags) & (NI_IFF_NETWORK_UP | NI_IFF_LINK_UP))
		return 0;

	if (!ni_interface_network_is_up_afinfo(cfg->name, &cfg->ipv4, &cur->ipv4)
	 || !ni_interface_network_is_up_afinfo(cfg->name, &cfg->ipv6, &cur->ipv6))
		return 0;

	return 1;
}

/*
 * Wait for interfaces to come up
 */
int
ni_interfaces_wait(ni_handle_t *system, const ni_interface_array_t *iflist, unsigned int ifevent, ni_evaction_t action)
{
	unsigned int i, waited = 0, dots = 0;
	int rv;

	setvbuf(stdout, NULL,_IONBF,  0);
	while (1) {
		unsigned int wait = 0;

		for (i = 0; i < iflist->count; ++i) {
			ni_interface_t *ifp = iflist->data[i];
			ni_ifaction_t *ifa = &ifp->startmode.ifaction[ifevent];

			if (ifa->wait == 0)
				continue;
			if (action == NI_INTERFACE_START) {
				/* We're trying to start the interface. */
				if (ni_interface_ensure_up(system, ifp)) {
					printf("\r"); clear_line(stdout);
					printf("%s: up\n", ifp->name);
					ifa->wait = 0;
					continue;
				}
			} else {
				/* We're trying to stop the interface */
				/* To be done */
				ifa->wait = 0;
				continue;
			}

			if (dots == 0) {
				printf("%s: ", ifp->name);
				dots = strlen(ifp->name + 2);
			}

			if (ifa->wait < waited) {
				printf("\r"); clear_line(stdout);
				ni_error("%s: failed to come up", ifp->name);
				ifa->wait = 0;
				if (ifa->mandatory)
					rv = -1;
				continue;
			}
			wait++;
		}

		if (!wait)
			break;

		sleep(1);

		printf(".");
		dots++;
		waited++;

		if ((rv = ni_refresh(system)) < 0)
			return -1;
	}

	return 0;
}

/*
 * Bring a single interface up
 */
int
do_ifup_one(ni_handle_t *nih, ni_interface_t *ifp)
{
	if (opt_dryrun) {
		xml_node_t *ifnode;

		printf("Would send configure(%s, up)\n", ifp->name);
		ifnode = ni_syntax_xml_from_interface(ni_default_xml_syntax(), nih, ifp);
		if (ifnode) {
			xml_node_print(ifnode, stdout);
			xml_node_free(ifnode);
		}
		return 0;
	}
	if (ni_interface_configure(nih, ifp, NULL) < 0) {
		ni_error("%s: unable to bring up", ifp->name);
		return -1;
	}

	ni_debug_wicked("%s: asked to bring up", ifp->name);
	return 0;
}

/*
 * Bring a single interface down
 */
static int
do_ifdown_one(ni_handle_t *system, ni_interface_t *ifp, int delete)
{
	int rv;

	if (delete) {
		switch (ifp->type) {
		case NI_IFTYPE_VLAN:
		case NI_IFTYPE_BRIDGE:
		case NI_IFTYPE_BOND:
			break;

		default:
			delete = 0;
			break;
		}
	}

	ifp->ifflags &= ~(NI_IFF_NETWORK_UP | NI_IFF_LINK_UP);

	/* Clear IP addressing; this will make sure all addresses are
	 * removed from the interface, and dhcp is shut down etc.
	 */
	ni_interface_clear_addresses(ifp);
	ni_interface_clear_routes(ifp);

	if (opt_dryrun) {
		xml_node_t *ifnode;

		printf("Would send configure(%s, down)\n", ifp->name);
		ifnode = ni_syntax_xml_from_interface(ni_default_xml_syntax(), system, ifp);
		if (ifnode) {
			xml_node_print(ifnode, stdout);
			xml_node_free(ifnode);
		}
		return 0;
	}

	rv = ni_interface_configure(system, ifp, NULL);
	if (rv < 0) {
		ni_error("Unable to shut down interface %s\n", ifp->name);
		return -1;
	}
	ni_debug_wicked("%s: asked to bring down", ifp->name);

	if (delete) {
		rv = ni_interface_delete(system, ifp->name);
		if (rv < 0) {
			ni_error("Unable to delete interface %s\n", ifp->name);
			return -1;
		}
	}

	return rv;
}

/*
 * Handle "ifup" command
 */
int
do_ifup(int argc, char **argv)
{
	static struct option ifup_options[] = {
		{ "file", required_argument, NULL, 'f' },
		{ NULL }
	};
	ni_interface_state_array_t state_array = { 0 };
	const char *ifname = NULL;
	const char *opt_file = NULL;
	unsigned int ifevent = NI_IFACTION_MANUAL_UP;
	ni_handle_t *config = NULL;
	ni_handle_t *system = NULL;
	int c, rv = -1;

	optind = 1;
	while ((c = getopt_long(argc, argv, "", ifup_options, NULL)) != EOF) {
		switch (c) {
		case 'f':
			opt_file = optarg;
			break;

		default:
usage:
			fprintf(stderr,
				"wicked [options] ifup [ifup-options] all\n"
				"wicked [options] ifup [ifup-options] <ifname>\n"
				"\nSupported ifup-options:\n"
				"  --file <filename>\n"
				"      Read interface configuration(s) from file rather than using system config\n"
				"  --boot\n"
				"      Ignore interfaces with startmode != boot\n"
				);
			return 1;
		}
	}

	if (optind + 1 != argc) {
		fprintf(stderr, "Bad number of arguments\n");
		goto usage;
	}
	ifname = argv[optind++];

	if (!strcmp(ifname, "boot")) {
		ifevent = NI_IFACTION_BOOT;
		ifname = "all";
	}

	/* Using --file means, read the interface definition from a local file.
	 * Otherwise, first retrieve /config/interface/<ifname> from the server,
	 * change <status> to up, and send it back.
	 */
	if (opt_file) {
		ni_syntax_t *syntax = ni_syntax_new("netcf", opt_file);

		config = ni_netconfig_open(syntax);
		if ((rv = ni_refresh(config)) < 0) {
			ni_error("unable to load interface definition from %s", opt_file);
			goto failed;
		}
	} else {
		config = ni_indirect_open("/config");
		if (!strcmp(ifname, "all")) {
			rv = ni_refresh(config);
		} else {
			rv = ni_interface_refresh_one(config, ifname);
		}

		if (rv < 0) {
			ni_error("unable to obtain interface configuration");
			goto failed;
		}
	}

	system = ni_indirect_open("/system");
	if ((rv = ni_refresh(system)) < 0) {
		ni_error("cannot refresh interface state");
		goto failed;
	}

	if (!strcmp(ifname, "all")) {
		if (ni_build_partial_topology2(config, ifevent, NI_INTERFACE_START, &state_array) < 0) {
			ni_error("failed to build interface hierarchy");
			goto failed;
		}
	} else {
		ni_interface_state_t *state;
		ni_interface_t *ifp;

		if (!(ifp = ni_interface_by_name(config, ifname))) {
			ni_error("cannot find interface %s in interface description", ifname);
			goto failed;
		}

		state = ni_interface_state_new(ifname, ifp);
		state->behavior = ifp->startmode.ifaction[ifevent];
		state->want_state = STATE_NETWORK_UP;
		ni_interface_state_array_append(&state_array, state);
	}

#if 1
	rv = ni_interfaces_wait2(system, &state_array);
#else
	for (i = 0; rv >= 0 && i < iflist.count; ++i)
		rv = do_ifup_one(system, iflist.data[i]);


	/* Wait for all interfaces to come up */
	if (rv >= 0)
		rv = ni_interfaces_wait(system, &iflist, ifevent, NI_INTERFACE_START);
#endif

failed:
	if (config)
		ni_close(config);
	if (system)
		ni_close(system);
	ni_interface_state_array_destroy(&state_array);
	return (rv == 0);
}

/*
 * Handle "ifdown" command
 */
int
do_ifdown(int argc, char **argv)
{
	static struct option ifdown_options[] = {
		{ "delete", no_argument, NULL, 'd' },
		{ NULL }
	};
	int opt_delete = 0;
	const char *ifname = NULL;
	unsigned int ifevent = NI_IFACTION_MANUAL_DOWN;
	ni_handle_t *system = NULL;
	ni_interface_array_t iflist;
	int i, c, rv;

	optind = 1;
	while ((c = getopt_long(argc, argv, "", ifdown_options, NULL)) != EOF) {
		switch (c) {
		case 'd':
			opt_delete = 1;
			break;

		default:
usage:
			fprintf(stderr,
				"Usage:\n"
				"wicked [options] ifdown [ifdown-options] all\n"
				"wicked [options] ifdown [ifdown-options] <ifname>\n"
				"\nSupported ifdown-options:\n"
				"  --delete\n"
				"      Delete virtual interfaces in addition to shutting them down\n"
				);
			return 1;
		}
	}

	if (optind + 1 != argc) {
		fprintf(stderr, "Bad number of arguments\n");
		goto usage;
	}
	ifname = argv[optind++];

	/* Otherwise, first retrieve /system/interface/<ifname> from the server,
	 * change <status> to down, and send it back.
	 */
	{
		system = ni_indirect_open("/system");
		if (!strcmp(ifname, "all") || !strcmp(ifname, "shutdown")) {
			rv = ni_refresh(system);
		} else {
			rv = ni_interface_refresh_one(system, ifname);
		}

		if (rv < 0) {
			ni_error("unable to obtain interface configuration");
			goto failed;
		}
	}

	ifevent = NI_IFACTION_MANUAL_DOWN;
	if (!strcmp(ifname, "shutdown")) {
		ifevent = NI_IFACTION_SHUTDOWN;
		ifname = "all";
	}

	if (!strcmp(ifname, "all")) {
		if (ni_build_partial_topology(system, NI_INTERFACE_STOP)) {
			ni_error("failed to build interface hierarchy");
			goto failed;
		}

		ni_interface_array_init(&iflist);
		rv = ni_interface_topology_flatten(system, &iflist, ifevent, NI_INTERFACE_STOP);
	} else {
		ni_interface_t *ifp;

		if (!(ifp = ni_interface_by_name(system, ifname))) {
			ni_error("cannot find interface %s in interface description", ifname);
			goto failed;
		}
		ni_interface_array_append(&iflist, ifp);
	}

	if (rv >= 0) {
		for (i = iflist.count - 1; rv >= 0 && i >= 0; --i) {
			iflist.data[i]->ifflags &= ~(NI_IFF_NETWORK_UP | NI_IFF_LINK_UP);
			rv = do_ifdown_one(system, iflist.data[i], opt_delete);
		}

		ni_interface_array_destroy(&iflist);
	}

failed:
	if (system)
		ni_close(system);
	return (rv == 0);
}

/*
 * We also allow the user to send raw REST commands to the server,
 * if s/he so desires
 */
static int
do_rest(const char *cmd, int argc, char **argv)
{
	xml_node_t *send_xml = NULL, *recv_xml = NULL, **recvp = NULL;
	int rest_op;
	char *path;

	if (argc != 2) {
		ni_error("Missing path name\n");
		fprintf(stderr,
			"Usage:\n"
			"wicked [options] get /path\n"
			"wicked [options] put /path\n"
			"wicked [options] post /path\n"
			"wicked [options] delete /path\n"
			"\nput and post commands expect an XML document on standard input\n");
		return 1;
	}

	path = argv[1];

	rest_op = ni_wicked_rest_op_parse(cmd);
	if (rest_op < 0)
		return 1;

	if (rest_op == NI_REST_OP_PUT || rest_op == NI_REST_OP_POST) {
		send_xml = xml_node_scan(stdin);
		if (send_xml == NULL) {
			ni_error("unable to read XML from standard input");
			return 1;
		}
	}
	if (rest_op != NI_REST_OP_DELETE)
		recvp = &recv_xml;
	if (__wicked_request(rest_op, path, send_xml, recvp) < 0)
		return 1;

	if (recv_xml)
		xml_node_print(recv_xml, stdout);

	if (send_xml)
		xml_node_free(send_xml);
	if (recv_xml)
		xml_node_free(recv_xml);

	return 0;
}

/*
 * xpath
 * This is a utility that can be used by network scripts to extracts bits and
 * pieces of information from an XML file.
 * This is still a bit inconvenient, especially if you need to extract more than
 * one of two elements, since we have to parse and reparse the XML file every
 * time you invoke this program.
 * On the other hand, there's a few rather nifty things you can do. For instance,
 * the following will extract addres/prefixlen pairs for every IPv4 address
 * listed in an XML network config:
 *
 * wicked xpath \
 *	--reference "interface/protocol[@family = 'ipv4']/ip" \
 *	--file vlan.xml \
 *	'%{@address}/%{@prefix}'
 *
 * The "reference" argument tells wicked to look up the <protocol>
 * element with a "family" attribute of "ipv4", and within that,
 * any <ip> elements. For each of these, it obtains the address
 * and prefix attribute, and prints it separated by a slash.
 */
int
do_xpath(int argc, char **argv)
{
	static struct option xpath_options[] = {
		{ "reference", required_argument, NULL, 'r' },
		{ "file", required_argument, NULL, 'f' },
		{ NULL }
	};
	const char *opt_reference = NULL, *opt_file = "-";
	xpath_result_t *input;
	xml_document_t *doc;
	int c;

	optind = 1;
	while ((c = getopt_long(argc, argv, "", xpath_options, NULL)) != EOF) {
		switch (c) {
		case 'r':
			opt_reference = optarg;
			break;

		case 'f':
			opt_file = optarg;
			break;

		default:
			fprintf(stderr,
				"wicked [options] xpath [--reference <expr>] [--file <path>] expr ...\n");
			return 1;
		}
	}

	doc = xml_document_read(opt_file);
	if (!doc) {
		fprintf(stderr, "Error parsing XML document %s\n", opt_file);
		return 1;
	}

	if (opt_reference) {
		xpath_enode_t *enode;

		enode = xpath_expression_parse(opt_reference);
		if (!enode) {
			fprintf(stderr, "Error parsing XPATH expression %s\n", opt_reference);
			return 1;
		}

		input = xpath_expression_eval(enode, doc->root);
		if (!input) {
			fprintf(stderr, "Error evaluating XPATH expression\n");
			return 1;
		}

		if (input->type != XPATH_ELEMENT) {
			fprintf(stderr, "Failed to look up reference node - returned non-element result\n");
			return 1;
		}
		if (input->count == 0) {
			fprintf(stderr, "Failed to look up reference node - returned empty list\n");
			return 1;
		}
		xpath_expression_free(enode);
	} else {
		input = xpath_result_new(XPATH_ELEMENT);
		xpath_result_append_element(input, doc->root);
	}

	while (optind < argc) {
		const char *expression = argv[optind++];
		ni_string_array_t result;
		xpath_format_t *format;
		unsigned int n;

		format = xpath_format_parse(expression);
		if (!format) {
			fprintf(stderr, "Error parsing XPATH format string %s\n", expression);
			return 1;
		}

		ni_string_array_init(&result);
		for (n = 0; n < input->count; ++n) {
			xml_node_t *refnode = input->node[n].value.node;

			if (!xpath_format_eval(format, refnode, &result)) {
				fprintf(stderr, "Error evaluating XPATH expression\n");
				ni_string_array_destroy(&result);
				return 1;
			}
		}

		for (n = 0; n < result.count; ++n)
			printf("%s\n", result.data[n]);

		ni_string_array_destroy(&result);
		xpath_format_free(format);
	}

	return 0;
}

static void
clear_line(FILE *fp)
{
	if (isatty(fileno(fp))) {
		/* use termcap to do this */
	}
	fprintf(fp, "%20s\r", "");
}
