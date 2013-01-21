
/*
 * The olsr.org Optimized Link-State Routing daemon(olsrd)
 * Copyright (c) 2004-2012, the olsr.org team - see HISTORY file
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 * * Neither the name of olsr.org, olsrd nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Visit http://www.olsr.org for more information.
 *
 * If you find this software useful feel free to make a donation
 * to the project. For more information see the website or contact
 * the copyright holders.
 *
 */

#include "common/common_types.h"
#include "common/avl.h"
#include "common/avl_comp.h"
#include "config/cfg_db.h"
#include "config/cfg_schema.h"
#include "rfc5444/rfc5444_iana.h"
#include "rfc5444/rfc5444_print.h"
#include "rfc5444/rfc5444_reader.h"
#include "rfc5444/rfc5444_writer.h"
#include "core/olsr_logging.h"
#include "core/olsr_memcookie.h"
#include "core/olsr_subsystem.h"
#include "core/olsr_timer.h"
#include "tools/olsr_cfg.h"
#include "tools/olsr_rfc5444.h"

/* constants and definitions */
#define _LOG_RFC5444_NAME "rfc5444"

struct _rfc5444_config {
  struct olsr_packet_managed_config socket;

  uint64_t aggregation_interval;
};

/* prototypes */

static struct olsr_rfc5444_target *_create_target(
    struct olsr_rfc5444_interface *, struct netaddr *dst, bool unicast);
static void _destroy_target(struct olsr_rfc5444_target *);

static void _cb_receive_data(struct olsr_packet_socket *,
      union netaddr_socket *from, size_t length);
static void _cb_send_unicast_packet(
    struct rfc5444_writer *, struct rfc5444_writer_interface *, void *, size_t);
static void _cb_send_multicast_packet(
    struct rfc5444_writer *, struct rfc5444_writer_interface *, void *, size_t);
static void _cb_forward_message(struct rfc5444_reader_tlvblock_context *context,
    uint8_t *buffer, size_t length);

static bool _cb_single_ifselector(struct rfc5444_writer *, struct rfc5444_writer_interface *, void *);
static bool _cb_forward_ifselector(struct rfc5444_writer *, struct rfc5444_writer_interface *, void *);

static struct rfc5444_reader_addrblock_entry *_alloc_addrblock_entry(void);
static struct rfc5444_reader_tlvblock_entry *_alloc_tlvblock_entry(void);
static struct rfc5444_writer_address *_alloc_address_entry(void);
static struct rfc5444_writer_addrtlv *_alloc_addrtlv_entry(void);
static void _free_addrblock_entry(void *);
static void _free_tlvblock_entry(void *);
static void _free_address_entry(void *);
static void _free_addrtlv_entry(void *);

static void _cb_add_seqno(struct rfc5444_writer *, struct rfc5444_writer_interface *);
static void _cb_aggregation_event (void *);

static void _cb_cfg_rfc5444_changed(void);
static void _cb_cfg_interface_changed(void);
static void _cb_interface_changed(struct olsr_packet_managed *managed, bool);

/* memory block for rfc5444 targets plus MTU sized packet buffer */
static struct olsr_memcookie_info _protocol_memcookie = {
  .name = "RFC5444 Protocol",
  .size = sizeof(struct olsr_rfc5444_protocol),
};

static struct olsr_memcookie_info _interface_memcookie = {
  .name = "RFC5444 Interface",
  .size = sizeof(struct olsr_rfc5444_interface),
};

static struct olsr_memcookie_info _target_memcookie = {
  .name = "RFC5444 Target",
  .size = sizeof(struct olsr_rfc5444_target),
};

static struct olsr_memcookie_info _tlvblock_memcookie = {
  .name = "RFC5444 TLVblock",
  .size = sizeof(struct rfc5444_reader_tlvblock_entry),
  .min_free_count = 32,
};

static struct olsr_memcookie_info _addrblock_memcookie = {
  .name = "RFC5444 Addrblock",
  .size = sizeof(struct rfc5444_reader_addrblock_entry),
  .min_free_count = 32,
};

static struct olsr_memcookie_info _address_memcookie = {
  .name = "RFC5444 Address",
  .size = sizeof(struct rfc5444_writer_address),
  .min_free_count = 32,
};

static struct olsr_memcookie_info _addrtlv_memcookie = {
  .name = "RFC5444 AddrTLV",
  .size = sizeof(struct rfc5444_writer_addrtlv),
  .min_free_count = 32,
};

/* timer for aggregating multiple rfc5444 messages to the same target */
static struct olsr_timer_info _aggregation_timer = {
  .name = "RFC5444 aggregation",
  .callback = _cb_aggregation_event,
};

/* configuration settings for handler */
static struct cfg_schema_section _rfc5444_section = {
  .type = CFG_RFC5444_SECTION,
  .mode = CFG_SSMODE_UNNAMED,
  .cb_delta_handler = _cb_cfg_rfc5444_changed,
};

static struct cfg_schema_entry _rfc5444_entries[] = {
  CFG_MAP_ACL_V46(_rfc5444_config, socket.acl, "acl", "default_accept",
    "Access control list for RFC5444 interface"),
  CFG_MAP_NETADDR_V4(_rfc5444_config, socket.bindto_v4, "bindto_v4", NETADDR_STR_ANY4,
    "Bind RFC5444 ipv4 socket to this address", true, true),
  CFG_MAP_NETADDR_V6(_rfc5444_config, socket.bindto_v6, "bindto_v6", NETADDR_STR_ANY6,
    "Bind RFC5444 ipv6 socket to this address", true, true),
  CFG_MAP_INT_MINMAX(_rfc5444_config, socket.port, "port", RFC5444_MANET_UDP_PORT_TXT,
    "UDP port for RFC5444 interface", 1, 65535),

  CFG_MAP_CLOCK(_rfc5444_config, aggregation_interval, "agregation_interval", "0.100",
    "Interval in seconds for message aggregation"),
};

static struct cfg_schema_section _interface_section = {
  .type = CFG_INTERFACE_SECTION,
  .mode = CFG_SSMODE_NAMED,
  .cb_delta_handler = _cb_cfg_interface_changed,
};

static struct cfg_schema_entry _interface_entries[] = {
  CFG_MAP_ACL_V46(olsr_packet_managed_config, acl, "acl", "default_accept",
    "Access control list for RFC5444 interface"),
  CFG_MAP_NETADDR_V4(olsr_packet_managed_config, bindto_v4, "bindto_v4", NETADDR_STR_ANY4,
    "Bind RFC5444 ipv4 socket to this address", true, true),
  CFG_MAP_NETADDR_V6(olsr_packet_managed_config, bindto_v6, "bindto_v6", NETADDR_STR_LINKLOCAL6,
    "Bind RFC5444 ipv6 socket to this address", true, true),
  CFG_MAP_NETADDR_V4(olsr_packet_managed_config, multicast_v4, "multicast_v4", RFC5444_MANET_MULTICAST_V4_TXT,
    "ipv4 multicast address of this socket", false, true),
  CFG_MAP_NETADDR_V6(olsr_packet_managed_config, multicast_v6, "multicast_v6", RFC5444_MANET_MULTICAST_V6_TXT,
    "ipv6 multicast address of this socket", false, true),
};

static uint64_t _aggregation_interval;

/* rfc5444 handling */
static const struct rfc5444_reader _reader_template = {
  .forward_message = _cb_forward_message,
  .malloc_addrblock_entry = _alloc_addrblock_entry,
  .malloc_tlvblock_entry = _alloc_tlvblock_entry,
  .free_addrblock_entry = _free_addrblock_entry,
  .free_tlvblock_entry = _free_tlvblock_entry,
};
static const struct rfc5444_writer _writer_template = {
  .malloc_address_entry = _alloc_address_entry,
  .malloc_addrtlv_entry = _alloc_addrtlv_entry,
  .free_address_entry = _free_address_entry,
  .free_addrtlv_entry = _free_addrtlv_entry,
  .msg_size = RFC5444_MAX_MESSAGE_SIZE,
  .addrtlv_size = RFC5444_ADDRTLV_BUFFER,
};

/* rfc5444_printer */
static struct autobuf _printer_buffer;
static struct rfc5444_print_session _printer_session;

static struct rfc5444_reader _printer = {
  .malloc_addrblock_entry = _alloc_addrblock_entry,
  .malloc_tlvblock_entry = _alloc_tlvblock_entry,
  .free_addrblock_entry = _free_addrblock_entry,
  .free_tlvblock_entry = _free_tlvblock_entry,
};

/* configuration for RFC5444 socket */
static uint8_t _incoming_buffer[RFC5444_MAX_PACKET_SIZE];

static struct olsr_packet_config _socket_config = {
  .input_buffer = _incoming_buffer,
  .input_buffer_length = sizeof(_incoming_buffer),
  .receive_data = _cb_receive_data,
};

/* tree of active rfc5444 protocols */
static struct avl_tree _protocol_tree;

/* default protocol */
static struct olsr_rfc5444_protocol *_rfc5444_protocol = NULL;
static struct olsr_rfc5444_interface *_rfc5444_unicast = NULL;

/* rfc5444 handler state and logging source */
OLSR_SUBSYSTEM_STATE(_rfc5444_state);
static enum log_source LOG_RFC5444;

/**
 * Initialize RFC5444 handling system
 * @return -1 if an error happened, 0 otherwise
 */
int
olsr_rfc5444_init(void) {
  if (olsr_subsystem_init(&_rfc5444_state))
    return 0;

  LOG_RFC5444 = olsr_log_register_source(_LOG_RFC5444_NAME);

  avl_init(&_protocol_tree, avl_comp_strcasecmp, false, NULL);

  olsr_memcookie_add(&_protocol_memcookie);
  olsr_memcookie_add(&_target_memcookie);
  olsr_memcookie_add(&_addrblock_memcookie);
  olsr_memcookie_add(&_tlvblock_memcookie);
  olsr_memcookie_add(&_address_memcookie);
  olsr_memcookie_add(&_addrtlv_memcookie);

  olsr_timer_add(&_aggregation_timer);

  cfg_schema_add_section(olsr_cfg_get_schema(), &_rfc5444_section,
      _rfc5444_entries, ARRAYSIZE(_rfc5444_entries));

  cfg_schema_add_section(olsr_cfg_get_schema(), &_interface_section,
      _interface_entries, ARRAYSIZE(_interface_entries));

  _rfc5444_protocol = olsr_rfc5444_add_protocol(RFC5444_PROTOCOL, true);
  if (_rfc5444_protocol == NULL) {
    olsr_rfc5444_cleanup();
    return -1;
  }

  olsr_memcookie_add(&_interface_memcookie);
  _rfc5444_unicast = olsr_rfc5444_add_interface(
      _rfc5444_protocol, NULL, RFC5444_UNICAST_TARGET);
  if (_rfc5444_unicast == NULL) {
    olsr_rfc5444_cleanup();
    return -1;
  }

  if (abuf_init(&_printer_buffer)) {
    olsr_rfc5444_cleanup();
    return -1;
  }
  _printer_session.output = &_printer_buffer;

  rfc5444_reader_init(&_printer);
  rfc5444_print_add(&_printer_session, &_printer);

  return 0;
}

/**
 * Cleanup all allocated resources of RFC5444 handling
 */
void
olsr_rfc5444_cleanup(void) {
  struct olsr_rfc5444_protocol *protocol, *p_it;
  struct olsr_rfc5444_interface *interf, *i_it;
  struct olsr_rfc5444_target *target, *t_it;

  if (olsr_subsystem_cleanup(&_rfc5444_state))
    return;

  /* cleanup existing instances */
  avl_for_each_element_safe(&_protocol_tree, protocol, _node, p_it) {
    avl_for_each_element_safe(&protocol->_interface_tree, interf, _node, i_it) {
      avl_for_each_element_safe(&interf->_target_tree, target, _node, t_it) {
        target->_refcount = 1;
        olsr_rfc5444_remove_target(target);
      }
      interf->_refcount = 1;
      olsr_rfc5444_remove_interface(interf, NULL);
    }
    protocol->_refcount = 1;
    olsr_rfc5444_remove_protocol(protocol);
  }

  cfg_schema_remove_section(olsr_cfg_get_schema(), &_interface_section);

  olsr_timer_remove(&_aggregation_timer);

  if (_printer_session.output) {
    rfc5444_print_remove(&_printer_session);
    rfc5444_reader_cleanup(&_printer);
  }
  abuf_free(&_printer_buffer);

  olsr_memcookie_remove(&_protocol_memcookie);
  olsr_memcookie_remove(&_interface_memcookie);
  olsr_memcookie_remove(&_target_memcookie);
  olsr_memcookie_remove(&_tlvblock_memcookie);
  olsr_memcookie_remove(&_addrblock_memcookie);
  olsr_memcookie_remove(&_address_memcookie);
  olsr_memcookie_remove(&_addrtlv_memcookie);
  return;
}

/**
 * Trigger the creation of a RFC5444 message
 * @param target interface for outgoing message
 * @param msgid id of created message
 * @return return code of rfc5444 writer
 */
enum rfc5444_result olsr_rfc5444_send(
    struct olsr_rfc5444_target *target, uint8_t msgid) {
  struct olsr_rfc5444_interface *interf;
#if OONF_LOGGING_LEVEL >= OONF_LOGGING_LEVEL_INFO
  struct netaddr_str buf;
#endif

  interf = target->interface;

  /* check if socket can send data */
  if (!olsr_packet_managed_is_active(&interf->_socket,
      netaddr_get_address_family(&target->dst))) {
    return RFC5444_OKAY;
  }

  if (!olsr_timer_is_active(&target->_aggregation)) {
    /* activate aggregation timer */
    olsr_timer_start(&target->_aggregation, _aggregation_interval);
  }

  /* create message */
  OLSR_INFO(LOG_RFC5444, "Create message id %d for protocol %s/target %s on interface %s",
      msgid, target->interface->protocol->name, netaddr_to_string(&buf, &target->dst),
      target->interface->name);

  return rfc5444_writer_create_message(&target->interface->protocol->writer,
      msgid, _cb_single_ifselector, target);
}

struct olsr_rfc5444_protocol *
olsr_rfc5444_add_protocol(const char *name, bool fixed_local_port) {
  struct olsr_rfc5444_protocol *protocol;

  protocol = avl_find_element(&_protocol_tree, name, protocol, _node);
  if (protocol) {
    protocol->_refcount++;
    return protocol;
  }

  protocol = olsr_memcookie_malloc(&_protocol_memcookie);
  if (protocol == NULL) {
    return NULL;
  }

  /* set name */
  strscpy(protocol->name, name, sizeof(protocol->name));
  protocol->fixed_local_port = fixed_local_port;

  /* hook into global protocol tree */
  protocol->_node.key = protocol->name;
  avl_insert(&_protocol_tree, &protocol->_node);

  /* initialize rfc5444 reader/writer */
  memcpy(&protocol->reader, &_reader_template, sizeof(_reader_template));
  memcpy(&protocol->writer, &_writer_template, sizeof(_writer_template));
  protocol->writer.msg_buffer = protocol->_msg_buffer;
  protocol->writer.addrtlv_buffer = protocol->_addrtlv_buffer;
  rfc5444_reader_init(&protocol->reader);
  rfc5444_writer_init(&protocol->writer);

  /* init interface subtree */
  avl_init(&protocol->_interface_tree, avl_comp_strcasecmp, false, NULL);

  /* set initial refcount */
  protocol->_refcount = 1;

  return protocol;
}

void
olsr_rfc5444_remove_protocol(struct olsr_rfc5444_protocol *protocol) {
  if (protocol->_refcount > 1) {
    protocol->_refcount--;
    return;
  }

  rfc5444_reader_cleanup(&protocol->reader);
  rfc5444_writer_cleanup(&protocol->writer);
  olsr_memcookie_free(&_protocol_memcookie, protocol);
}

void
olsr_rfc5444_reconfigure_protocol(
    struct olsr_rfc5444_protocol *protocol, uint16_t port) {
  struct olsr_rfc5444_interface *interf;

  /* nothing to do? */
  if (port == protocol->port) {
    return;
  }

  OLSR_INFO(LOG_RFC5444, "Reconfigure protocol %s to port %u", protocol->name, port);

  /* store protocol port */
  protocol->port = port;

  avl_for_each_element(&protocol->_interface_tree, interf, _node) {
    olsr_packet_remove_managed(&interf->_socket, true);
    olsr_packet_add_managed(&interf->_socket);

    if (port) {
      olsr_rfc5444_reconfigure_interface(interf, NULL);
    }
  }
}

struct olsr_rfc5444_interface *
olsr_rfc5444_add_interface(struct olsr_rfc5444_protocol *protocol,
    struct olsr_rfc5444_interface_listener *listener, const char *name) {
  struct olsr_rfc5444_interface *interf;

  interf = avl_find_element(&protocol->_interface_tree,
      name, interf, _node);
  if (interf == NULL) {
    interf = olsr_memcookie_malloc(&_interface_memcookie);
    if (interf == NULL) {
      return NULL;
    }

    /* set name */
    strscpy(interf->name, name, sizeof(interf->name));

    /* set protocol reference */
    interf->protocol = protocol;

    /* hook into protocol */
    interf->_node.key = interf->name;
    avl_insert(&protocol->_interface_tree, &interf->_node);

    /* initialize target subtree */
    avl_init(&interf->_target_tree, avl_comp_netaddr, false, NULL);

    /* initialize socket */
    memcpy (&interf->_socket.config, &_socket_config, sizeof(_socket_config));
    interf->_socket.config.user = interf;
    interf->_socket.cb_settings_change = _cb_interface_changed;
    olsr_packet_add_managed(&interf->_socket);

    /* initialize listener list */
    list_init_head(&interf->_listener);

    /* increase protocol refcount */
    protocol->_refcount++;
  }

  /* increase reference count */
  interf->_refcount += 1;

  if (listener) {
    /* hookup listener */
    list_add_tail(&interf->_listener, &listener->_node);
    listener->interface = interf;
  }
  return interf;
}

void
olsr_rfc5444_remove_interface(struct olsr_rfc5444_interface *interf,
    struct olsr_rfc5444_interface_listener *listener) {
  if (listener != NULL && listener->interface != NULL) {
    list_remove(&listener->_node);
    listener->interface = NULL;
  }

  if (interf->_refcount > 1) {
    interf->_refcount--;
    return;
  }

  /* remove multicast targets */
  if (interf->multicast4) {
    _destroy_target(interf->multicast4);
  }
  if (interf->multicast6) {
    _destroy_target(interf->multicast6);
  }

  /* remove from protocol tree */
  avl_remove(&interf->protocol->_interface_tree, &interf->_node);

  /* decrease protocol refcount */
  olsr_rfc5444_remove_protocol(interf->protocol);

  /* remove socket */
  olsr_packet_remove_managed(&interf->_socket, false);

  /* free memory */
  olsr_memcookie_free(&_interface_memcookie, interf);
}

/**
 * Reconfigure the parameters of an rfc5444 interface. You cannot reconfigure
 * the interface name with this command.
 * @param interf pointer to existing rfc5444 interface
 * @param config new socket configuration, NULL to just reapply the current
 *  configuration
 */
void
olsr_rfc5444_reconfigure_interface(struct olsr_rfc5444_interface *interf,
    struct olsr_packet_managed_config *config) {
  struct olsr_rfc5444_target *target, *old;
  uint16_t port;

#if OONF_LOGGING_LEVEL >= OONF_LOGGING_LEVEL_WARN
  struct netaddr_str buf;
#endif

  old = NULL;

  if (config != NULL) {
    /* copy socket configuration */
    memcpy(&interf->_socket_config, config, sizeof(interf->_socket_config));

    /* overwrite interface name */
    strscpy(interf->_socket_config.interface, interf->name,
        sizeof(interf->_socket_config.interface));
  }
  else {
    config = &interf->_socket_config;
  }

  /* get port */
  port = interf->protocol->port;

  /* set fixed configuration options */
  if (interf->_socket_config.multicast_port == 0) {
    interf->_socket_config.multicast_port = port;
  }
  if (interf->protocol->fixed_local_port && interf->_socket_config.port == 0) {
    interf->_socket_config.port = port;
  }

  OLSR_INFO(LOG_RFC5444, "Reconfigure RFC5444 interface %s to port %u/%u",
      interf->name, interf->_socket_config.port, interf->_socket_config.multicast_port);

  if (strcmp(interf->name, RFC5444_UNICAST_TARGET) == 0) {
    /* unicast interface */
    netaddr_invalidate(&interf->_socket_config.multicast_v4);
    netaddr_invalidate(&interf->_socket_config.multicast_v6);
    interf->_socket_config.port = port;
    interf->_socket_config.interface[0] = 0;
  }

  if (port == 0) {
    /* delay configuration apply */
    OLSR_INFO_NH(LOG_RFC5444, "    delay configuration, we still lack to protocol port");
    return;
  }

  /* apply socket configuration */
  olsr_packet_apply_managed(&interf->_socket, &interf->_socket_config);

  /* handle IPv4 multicast target */
  if (interf->multicast4) {
    old = interf->multicast4;
    interf->multicast4 = NULL;
  }
  if (netaddr_get_address_family(&config->multicast_v4) != AF_UNSPEC) {
    target = _create_target(interf, &config->multicast_v4, false);
    if (target == NULL) {
      OLSR_WARN(LOG_RFC5444, "Could not create multicast target %s for interface %s",
          netaddr_to_string(&buf, &config->multicast_v4), interf->name);
      interf->multicast4 = old;
      old = NULL;
    }
    else {
      interf->multicast4 = target;
    }
  }
  if (old) {
    _destroy_target(old);
  }

  /* handle IPv6 multicast target */
  if (interf->multicast6) {
    old = interf->multicast6;
    interf->multicast6 = NULL;
  }
  if (netaddr_get_address_family(&config->multicast_v6) != AF_UNSPEC) {
    target = _create_target(interf, &config->multicast_v6, false);
    if (target == NULL) {
      OLSR_WARN(LOG_RFC5444, "Could not create multicast socket %s for interface %s",
          netaddr_to_string(&buf, &config->multicast_v6), interf->name);
      interf->multicast6 = old;
      old = NULL;
    }
    else {
      interf->multicast6 = target;
    }
  }
  if (old) {
    _destroy_target(old);
  }
}

struct olsr_rfc5444_target *
olsr_rfc5444_add_target(struct olsr_rfc5444_interface *interf,
    struct netaddr *dst) {
  struct olsr_rfc5444_target *target;

  target = avl_find_element(&interf->_target_tree, dst, target, _node);
  if (target) {
    target->_refcount++;
    return target;
  }

  target = _create_target(interf, dst, true);
  if (target == NULL) {
    return NULL;
  }

  /* hook into interface tree */
  target->_node.key = &target->dst;
  avl_insert(&interf->_target_tree, &target->_node);

  /* increase interface refcount */
  interf->_refcount++;
  return target;
}

void
olsr_rfc5444_remove_target(struct olsr_rfc5444_target *target) {
  if (target->_refcount > 1) {
    target->_refcount--;
    return;
  }

  /* remove from protocol tree */
  avl_remove(&target->interface->_target_tree, &target->_node);

  /* decrease protocol refcount */
  olsr_rfc5444_remove_interface(target->interface, NULL);

  /* remove target */
  _destroy_target(target);
}

uint16_t
olsr_rfc5444_next_target_seqno(struct olsr_rfc5444_target *target) {
  target->_seqno++;

  return target->_seqno;
}

static struct olsr_rfc5444_target *
_create_target(struct olsr_rfc5444_interface *interf,
    struct netaddr *dst, bool unicast) {
  static struct olsr_rfc5444_target *target;

  target = olsr_memcookie_malloc(&_target_memcookie);
  if (target == NULL) {
    return NULL;
  }

  /* initialize rfc5444 interfaces */
  target->rfc5444_if.packet_buffer = target->_packet_buffer;
  target->rfc5444_if.packet_size = RFC5444_MAX_PACKET_SIZE;
  target->rfc5444_if.addPacketHeader = _cb_add_seqno;
  if (unicast) {
    target->rfc5444_if.sendPacket = _cb_send_unicast_packet;
  }
  else {
    target->rfc5444_if.sendPacket = _cb_send_multicast_packet;
  }
  target->rfc5444_if.last_seqno = random() & 0xffff;
  rfc5444_writer_register_interface(
      &interf->protocol->writer, &target->rfc5444_if);

  /* get initial sequence number */
  target->_seqno = random() & 0xffff;

  /* copy socket description */
  memcpy(&target->dst, dst, sizeof(target->dst));


  /* set interface reference */
  target->interface = interf;

  /* aggregation timer */
  target->_aggregation.info = &_aggregation_timer;
  target->_aggregation.cb_context = target;

  target->_refcount = 1;
  return target;
}

static void
_destroy_target(struct olsr_rfc5444_target *target) {
  /* cleanup interface */
  rfc5444_writer_unregister_interface(
      &target->interface->protocol->writer, &target->rfc5444_if);

  /* stop timer */
  olsr_timer_stop(&target->_aggregation);

  /* free memory */
  olsr_memcookie_free(&_target_memcookie, target);
}

static void
_print_packet_to_buffer(union netaddr_socket *sock __attribute__((unused)),
    struct olsr_rfc5444_interface *interf __attribute__((unused)),
    uint8_t *ptr, size_t len,
    const char *success __attribute__((unused)),
    const char *error __attribute__((unused))) {
  enum rfc5444_result result;
#if OONF_LOGGING_LEVEL >= OONF_LOGGING_LEVEL_WARN
  struct netaddr_str buf;
#endif

  if (olsr_log_mask_test(log_global_mask, LOG_RFC5444, LOG_SEVERITY_DEBUG)) {
    abuf_clear(&_printer_buffer);
    rfc5444_print_hexdump(&_printer_buffer, "", ptr, len);

    result = rfc5444_reader_handle_packet(&_printer, ptr, len);
    if (result) {
      OLSR_WARN(LOG_RFC5444, "%s %s for printing: %s (%d)",
          error, netaddr_socket_to_string(&buf, sock), rfc5444_strerror(result), result);
    }
    else {
      OLSR_DEBUG(LOG_RFC5444, "%s %s through %s:",
          success, netaddr_socket_to_string(&buf, sock), interf->name);

      OLSR_DEBUG_NH(LOG_RFC5444, "%s", abuf_getptr(&_printer_buffer));
    }
  }
}

/**
 * Handle incoming packet from a socket
 * @param sock pointer to packet socket
 * @param from originator of incoming packet
 * @param length length of incoming packet
 */
static void
_cb_receive_data(struct olsr_packet_socket *sock,
      union netaddr_socket *from, size_t length) {
  struct olsr_rfc5444_protocol *protocol;
  struct olsr_rfc5444_interface *interf;
  enum rfc5444_result result;
#if OONF_LOGGING_LEVEL >= OONF_LOGGING_LEVEL_WARN
  struct netaddr_str buf;
#endif

  interf = sock->config.user;
  protocol = interf->protocol;

  protocol->input_address = from;
  protocol->input_interface = interf;

  _print_packet_to_buffer(from, interf, sock->config.input_buffer, length,
      "Incoming RFC5444 packet from",
      "Error while parsing incoming RFC5444 packet from");

  result = rfc5444_reader_handle_packet(
      &protocol->reader, sock->config.input_buffer, length);
  if (result) {
    OLSR_WARN(LOG_RFC5444, "Error while parsing incoming packet from %s: %s (%d)",
        netaddr_socket_to_string(&buf, from), rfc5444_strerror(result), result);
  }
}

/**
 * Callback for sending an ipv4 packet to a rfc5444 target
 * @param writer rfc5444 writer
 * @param interf rfc5444 interface
 * @param ptr pointer to outgoing buffer
 * @param size_t length of buffer
 */
static void
_cb_send_multicast_packet(struct rfc5444_writer *writer __attribute__((unused)),
    struct rfc5444_writer_interface *interf, void *ptr, size_t len) {
  struct olsr_rfc5444_target *target;
  union netaddr_socket sock;

  target = container_of(interf, struct olsr_rfc5444_target, rfc5444_if);

  netaddr_socket_init(&sock, &target->dst, target->interface->protocol->port,
      if_nametoindex(target->interface->name));

  _print_packet_to_buffer(&sock, target->interface, ptr, len,
      "Outgoing RFC5444 packet to",
      "Error while parsing outgoing RFC5444 packet to");

  olsr_packet_send_managed_multicast(&target->interface->_socket,
      ptr, len, netaddr_get_address_family(&target->dst));
}

/**
 * Callback for sending an ipv4 packet to a rfc5444 target
 * @param writer rfc5444 writer
 * @param interf rfc5444 interface
 * @param ptr pointer to outgoing buffer
 * @param size_t length of buffer
 */
static void
_cb_send_unicast_packet(struct rfc5444_writer *writer __attribute__((unused)),
    struct rfc5444_writer_interface *interf, void *ptr, size_t len) {
  struct olsr_rfc5444_target *target;
  union netaddr_socket sock;

  target = container_of(interf, struct olsr_rfc5444_target, rfc5444_if);

  netaddr_socket_init(&sock, &target->dst, target->interface->protocol->port,
      if_nametoindex(target->interface->name));

  _print_packet_to_buffer(&sock, target->interface, ptr, len,
      "Outgoing RFC5444 packet to",
      "Error while parsing outgoing RFC5444 packet to");

  olsr_packet_send_managed(&target->interface->_socket, &sock, ptr, len);
}

static void
_cb_forward_message(
    struct rfc5444_reader_tlvblock_context *context,
    uint8_t *buffer, size_t length) {
  struct olsr_rfc5444_protocol *protocol;
  enum rfc5444_result result;

  if (!context->has_origaddr || !context->has_seqno) {
    /* do not forward messages that cannot run through the duplicate check */
    return;
  }

  // TODO: handle duplicate detection
  return;

  protocol = container_of(context->reader, struct olsr_rfc5444_protocol, reader);
  result = rfc5444_writer_forward_msg(&protocol->writer, buffer, length,
      _cb_forward_ifselector, NULL);
  if (result) {
    OLSR_WARN(LOG_RFC5444, "Error while forwarding message: %s (%d)",
        rfc5444_strerror(result), result);
  }
}

/**
 * Selector for outgoing interfaces
 * @param writer rfc5444 writer
 * @param interf rfc5444 interface
 * @param ptr custom pointer, contains rfc5444 target
 *   or NULL if all interfaces
 * @return true if ptr is NULL or interface corresponds to custom pointer
 */
static bool
_cb_single_ifselector(struct rfc5444_writer *writer __attribute__((unused)),
    struct rfc5444_writer_interface *interf, void *ptr) {
  struct olsr_rfc5444_target *target = ptr;

  return &target->rfc5444_if == interf;
}

/**
 * Selector for forwarding interfaces
 * @param writer rfc5444 writer
 * @param interf rfc5444 interface
 * @param ptr NULL in this case
 * @return true if ptr is NULL or interface corresponds to custom pointer
 */
static bool
_cb_forward_ifselector(struct rfc5444_writer *writer __attribute__((unused)),
    struct rfc5444_writer_interface *interf __attribute__((unused)),
    void *ptr __attribute__((unused))) {
  // TODO: select forwarding interfaces
  return true;
}

/**
 * Internal memory allocation function for addrblock
 * @return pointer to cleared addrblock
 */
static struct rfc5444_reader_addrblock_entry *
_alloc_addrblock_entry(void) {
  return olsr_memcookie_malloc(&_addrblock_memcookie);
}

/**
 * Internal memory allocation function for rfc5444_reader_tlvblock_entry
 * @return pointer to cleared rfc5444_reader_tlvblock_entry
 */
static struct rfc5444_reader_tlvblock_entry *
_alloc_tlvblock_entry(void) {
  return olsr_memcookie_malloc(&_tlvblock_memcookie);
}

/**
 * Internal memory allocation function for rfc5444_writer_address
 * @return pointer to cleared rfc5444_writer_address
 */
static struct rfc5444_writer_address *
_alloc_address_entry(void) {
  return olsr_memcookie_malloc(&_address_memcookie);
}

/**
 * Internal memory allocation function for rfc5444_writer_addrtlv
 * @return pointer to cleared rfc5444_writer_addrtlv
 */
static struct rfc5444_writer_addrtlv *
_alloc_addrtlv_entry(void) {
  return olsr_memcookie_malloc(&_addrtlv_memcookie);
}

/**
 * Free an addrblock entry
 * @param pointer to addrblock
 */
static void
_free_addrblock_entry(void *addrblock) {
  olsr_memcookie_free(&_addrblock_memcookie, addrblock);
}

/**
 * Free a tlvblock entry
 * @param pointer to tlvblock
 */
static void
_free_tlvblock_entry(void *tlvblock) {
  olsr_memcookie_free(&_tlvblock_memcookie, tlvblock);
}

/**
 * Free a tlvblock entry
 * @param pointer to tlvblock
 */
static void
_free_address_entry(void *address) {
  olsr_memcookie_free(&_address_memcookie, address);
}

/**
 * Free a tlvblock entry
 * @param pointer to tlvblock
 */
static void
_free_addrtlv_entry(void *addrtlv) {
  olsr_memcookie_free(&_addrtlv_memcookie, addrtlv);
}

/**
 * Callback to add sequence number to outgoing RFC5444 packet
 * @param writer pointer to rfc5444 writer
 * @param interf pointer to rfc5444 interface
 */
static void
_cb_add_seqno(struct rfc5444_writer *writer, struct rfc5444_writer_interface *interf) {
  struct olsr_rfc5444_target *target;

  target = container_of(interf, struct olsr_rfc5444_target, rfc5444_if);

  rfc5444_writer_set_pkt_header(writer, interf, target->_pktseqno_refcount > 0);
  if (target->_pktseqno_refcount > 0) {
    rfc5444_writer_set_pkt_seqno(writer, interf, interf->last_seqno + 1);
  }
}

/**
 * Timer callback for message aggregation
 * @param ptr pointer to rfc5444 target
 */
static void
_cb_aggregation_event (void *ptr) {
  struct olsr_rfc5444_target *target;

  target = ptr;

  rfc5444_writer_flush(
      &target->interface->protocol->writer, &target->rfc5444_if, false);
}

/**
 * Configuration has changed, handle the changes
 */
static void
_cb_cfg_rfc5444_changed(void) {
  struct _rfc5444_config config;
  int result;

  memset(&config, 0, sizeof(config));
  result = cfg_schema_tobin(&config, _rfc5444_section.post,
      _rfc5444_entries, ARRAYSIZE(_rfc5444_entries));
  if (result) {
    OLSR_WARN(LOG_RFC5444,
        "Could not convert "CFG_RFC5444_SECTION" to binary (%d)",
        -(result+1));
    return;
  }

  /* apply values */
  olsr_rfc5444_reconfigure_protocol(_rfc5444_protocol, config.socket.port);
  _aggregation_interval = config.aggregation_interval;

  /* create unicast socket */

}

/**
 * Configuration has changed, handle the changes
 */
static void
_cb_cfg_interface_changed(void) {
  struct olsr_packet_managed_config config;

  struct olsr_rfc5444_interface *interf;
  int result;

  interf = NULL;

  interf = avl_find_element(
      &_rfc5444_protocol->_interface_tree,
      _interface_section.section_name, interf, _node);

  if (_interface_section.post == NULL) {
    /* this section has been removed */
    if (interf) {
      olsr_rfc5444_remove_interface(interf, NULL);
    }
    return;
  }

  memset(&config, 0, sizeof(config));
  result = cfg_schema_tobin(&config, _interface_section.post,
      _interface_entries, ARRAYSIZE(_interface_entries));
  if (result) {
    OLSR_WARN(LOG_RFC5444,
        "Could not convert "CFG_INTERFACE_SECTION" '%s' to binary (%d)",
        _interface_section.section_name, -(result+1));
    return;
  }

  if (interf == NULL) {
    interf = olsr_rfc5444_add_interface(_rfc5444_protocol,
        NULL, _interface_section.post->name);
    if (interf == NULL) {
      OLSR_WARN(LOG_RFC5444,
          "Could not generate interface '%s' for protocol '%s'",
          _interface_section.section_name, _rfc5444_protocol->name);
      return;
    }
  }

  olsr_rfc5444_reconfigure_interface(interf, &config);
}

/**
 * Interface settings of a rfc5444 interface changed
 * @param managed
 * @param changed true if socket addresses changed
 */
static void
_cb_interface_changed(struct olsr_packet_managed *managed, bool changed) {
  struct olsr_rfc5444_interface *interf;
  struct olsr_rfc5444_interface_listener *l;

  OLSR_INFO(LOG_RFC5444, "RFC5444 Interface change event: %s", managed->_managed_config.interface);

  interf = container_of(managed, struct olsr_rfc5444_interface, _socket);

  if (changed) {
    olsr_rfc5444_reconfigure_interface(interf, NULL);
  }

  list_for_each_element(&interf->_listener, l, _node) {
    l->cb_interface_changed(l, changed);
  }
}