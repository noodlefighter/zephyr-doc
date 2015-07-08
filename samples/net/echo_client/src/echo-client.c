/* echo-client.c - Send and receive unicast or multicast packets */

/*
 * Copyright (c) 2015 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The echo-client application is acting as a client that is run in Zephyr OS,
 * and echo-server is run in the host acting as a server. The client will send
 * either unicast or multicast packets to the server which will reply the packet
 * back to the originator.
 */

#if defined(CONFIG_STDOUT_CONSOLE)
#include <stdio.h>
#define PRINT           printf
#else
#include <misc/printk.h>
#define PRINT           printk
#endif

#include <zephyr.h>
#include <sections.h>

#include <drivers/rand32.h>

#include <net/ip_buf.h>
#include <net/net_core.h>
#include <net/net_socket.h>

#include <bluetooth/bluetooth.h>
#include <gatt/ipss.h>

#if defined(CONFIG_NET_TESTING)
#include <net_testing.h>
#else
#if defined(CONFIG_NETWORKING_WITH_IPV6)
#include <contiki/ipv6/uip-ds6.h>
#endif
#endif

/* Generated by http://www.lipsum.com/
 * 3 paragraphs, 176 words, 1230 bytes of Lorem Ipsum
 */
static const char *lorem_ipsum =
	"Lorem ipsum dolor sit amet, consectetur adipiscing elit. Vestibulum id cursus felis, sit amet suscipit velit. Integer facilisis malesuada porta. Nunc at accumsan mauris. Etiam vehicula, arcu consequat feugiat venenatis, tellus velit gravida ligula, quis posuere sem leo eget urna. Curabitur condimentum leo nec orci mattis, nec faucibus dui rutrum. Ut mollis orci in iaculis consequat. Nulla volutpat nibh eu velit sagittis, a iaculis dui aliquam."
	"\n"
	"Quisque interdum consequat eros a eleifend. Fusce dapibus nisl sit amet velit posuere imperdiet. Quisque accumsan tempor massa sit amet tincidunt. Integer sollicitudin vehicula tristique. Nulla sagittis massa turpis, ac ultricies neque posuere eu. Nulla et imperdiet ex. Etiam venenatis sed lacus tincidunt hendrerit. In libero nisl, congue id tellus vitae, tincidunt tristique mauris. Nullam sed porta massa. Sed condimentum sem eu convallis euismod. Suspendisse lobortis purus faucibus, gravida turpis id, mattis velit. Maecenas eleifend sapien eu tincidunt lobortis. Sed elementum sapien id enim laoreet consequat."
	"\n"
	"Aenean et neque aliquam, lobortis lectus in, consequat leo. Sed quis egestas nulla. Quisque ac risus quis elit mollis finibus. Phasellus efficitur imperdiet metus."
	"\n";

static int expecting;
static int ipsum_len;

#if defined(CONFIG_NETWORKING_WITH_IPV6)
/* admin-local, dynamically allocated multicast address */
#define MCAST_IPADDR { { { 0xff, 0x84, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x2 } } }

/* Define the peer IP address where to send messages */
#if !defined(CONFIG_NET_TESTING)
#define PEER_IPADDR { { { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1 } } }
#define MY_IPADDR { { { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x2 } } }
#define MY_PREFIX_LEN 64
#endif

static const struct in6_addr in6addr_my = MY_IPADDR;
static const struct in6_addr in6addr_peer = PEER_IPADDR;

#else /* CONFIG_NETWORKING_WITH_IPV6 */

#error "IPv4 not supported at the moment, fix me!"

/* Organization-local 239.192.0.0/14 */
#define MCAST_IPADDR { { { 239, 192, 0, 2 } } }

#if !defined(CONFIG_NET_TESTING)
#define PEER_IPADDR { { { 192, 0, 2, 1 } } }
#endif

#endif /* CONFIG_NETWORKING_WITH_IPV6 */

#define MY_PORT 8484
#define PEER_PORT 4242

static struct net_context *unicast, *multicast;

static inline void init_app(void)
{
	PRINT("%s: run mcast tester\n", __func__);

	sys_rand32_init();

#if defined(CONFIG_NET_TESTING)
	net_testing_setup();
#else
#if defined(CONFIG_NETWORKING_WITH_IPV6)
	uip_ds6_prefix_add((uip_ipaddr_t *)&in6addr_peer, MY_PREFIX_LEN, 0);
#endif
#endif
}

static inline void reverse(unsigned char *buf, int len)
{
	int i, last = len - 1;

	for (i = 0; i < len / 2; i++) {
		unsigned char tmp = buf[i];
		buf[i] = buf[last - i];
		buf[last - i] = tmp;
	}
}

#if 1
#define WAIT_TIME  5
#define WAIT_TICKS (WAIT_TIME * sys_clock_ticks_per_sec)
#else
#define WAIT_TICKS TICKS_UNLIMITED
#endif

static inline bool send_packet(const char *name,
			       struct net_context *ctx,
			       int ipsum_len,
			       int pos)
{
	struct net_buf *buf;
	bool fail = false;

	buf = ip_buf_get_tx(ctx);
	if (buf) {
		uint8_t *ptr;
		int sending_len = ipsum_len - pos;

		ptr = net_buf_add(buf, sending_len);
		memcpy(ptr, lorem_ipsum + pos, sending_len);
		sending_len = buf->len;

		if (net_send(buf) < 0) {
			PRINT("%s: sending %d bytes failed\n",
			      __func__, sending_len);
			ip_buf_unref(buf);
			fail = true;
			goto out;
		} else {
			PRINT("%s: sent %d bytes\n", __func__,
			      sending_len);
		}
	}

out:
	return fail;
}

static inline bool wait_reply(const char *name,
			      struct net_context *ctx,
			      int ipsum_len,
			      int pos)
{
	struct net_buf *buf;
	bool fail = false;
	int expected_len = ipsum_len - pos;

	/* Wait for the answer */
	buf = net_receive(ctx, WAIT_TICKS);
	if (buf) {
		if (ip_buf_appdatalen(buf) != expected_len) {
			PRINT("%s: received %d bytes, expected %d\n",
			      name, ip_buf_appdatalen(buf), expected_len);
			fail = true;
			goto free_buf;
		}

		/* In this test we reverse the received bytes.
		 * We could just pass the data back as is but
		 * this way it is possible to see how the app
		 * can manipulate the received data.
		 */
		reverse(ip_buf_appdata(buf), ip_buf_appdatalen(buf));

		/* Did we get all the data back?
		 */
		if (memcmp(lorem_ipsum + pos, ip_buf_appdata(buf),
			   expected_len)) {
			PRINT("%s: received data mismatch.\n", name);
			fail = true;
			goto free_buf;
		}

		PRINT("%s: received %d bytes\n", __func__,
			      expected_len);

	free_buf:
		ip_buf_unref(buf);
	} else {
		PRINT("%s: expected data, got none\n", name);
		fail = true;
	}

	return fail;
}


static inline bool get_context(struct net_context **unicast,
			       struct net_context **multicast)
{
	static struct net_addr mcast_addr;
	static struct net_addr any_addr;
	static struct net_addr peer_addr;
	static struct net_addr my_addr;
	int proto = IPPROTO_UDP;

#if defined(CONFIG_NETWORKING_WITH_IPV6)
	static const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
	static const struct in6_addr in6addr_mcast = MCAST_IPADDR;

	mcast_addr.in6_addr = in6addr_mcast;
	mcast_addr.family = AF_INET6;

	peer_addr.in6_addr = in6addr_peer;
	peer_addr.family = AF_INET6;

	any_addr.in6_addr = in6addr_any;
	any_addr.family = AF_INET6;

	my_addr.in6_addr = in6addr_my;
	my_addr.family = AF_INET6;
#else
	static const struct in_addr in4addr_any = { { { 0 } } };
	static const struct in_addr in4addr_my = { { { 0 } } };
	static struct in_addr in4addr_mcast = MCAST_IPADDR;
	static struct in_addr in4addr_peer = PEER_IPADDR;

	mcast_addr.in_addr = in4addr_mcast;
	mcast_addr.family = AF_INET;

	peer_addr.in_addr = in4addr_peer;
	peer_addr.family = AF_INET;

	any_addr.in_addr = in4addr_any;
	any_addr.family = AF_INET;

	my_addr.in_addr = in4addr_my;
	my_addr.family = AF_INET;
#endif

#ifdef CONFIG_NETWORKING_WITH_TCP
	proto = IPPROTO_TCP;
#endif /* CONFIG_NETWORKING_WITH_TCP */

	*unicast = net_context_get(proto,
				   &peer_addr, PEER_PORT,
				   &my_addr, MY_PORT);
	if (!*unicast) {
		PRINT("%s: Cannot get sending network context\n",
		      __func__);
		return false;
	}

	*multicast = net_context_get(IPPROTO_UDP,
				     &mcast_addr, PEER_PORT,
				     &my_addr, MY_PORT);
	if (!*multicast) {
		PRINT("%s: Cannot get mcast sending network context\n",
		      __func__);
		return false;
	}

	return true;
}

#if defined(CONFIG_NANOKERNEL)
#define STACKSIZE 2000
static char __noinit __stack stack_receiving[STACKSIZE];
#endif

void sending(int resend)
{
	static bool send_unicast = true;

	PRINT("%s: Sending packet\n", __func__);

	if (resend) {
		expecting = resend;
	} else {
		expecting = sys_rand32_get() % ipsum_len;
	}

	if (send_unicast) {
		if (send_packet(__func__, unicast, ipsum_len,
				expecting)) {
			PRINT("Unicast sending %d bytes FAIL\n",
			      ipsum_len - expecting);
		}
	} else {
		if (send_packet(__func__, multicast, ipsum_len,
				expecting)) {
			PRINT("Multicast sending %d bytes FAIL\n",
			      ipsum_len - expecting);
		}
	}
}

void receiving(void)
{
	int expecting_len = 0;

	sending(expecting_len);

	while (1) {
		PRINT("%s: Waiting packet\n", __func__);

		if (wait_reply(__func__, unicast,
			       ipsum_len, expecting)) {
			if (expecting_len > 0) {
				PRINT("Resend %d bytes -> FAIL\n",
				      ipsum_len - expecting);
				expecting_len = 0;
			} else {
				PRINT("Waiting %d bytes -> resending\n",
				      ipsum_len - expecting);
				expecting_len = expecting;
			}
		}

		sending(expecting_len);
	}
}

void main(void)
{
	net_init();

	init_app();

	ipsum_len = strlen(lorem_ipsum);

#if defined(CONFIG_NETWORKING_WITH_BT)
	if (bt_enable(NULL)) {
		PRINT("Bluetooth init failed\n");
		return;
	}
	ipss_init();
	ipss_advertise();
#endif

	if (!get_context(&unicast, &multicast)) {
		PRINT("%s: Cannot get network context\n", __func__);
		return;
	}

#if defined(CONFIG_MICROKERNEL)
	receiving();
#else
	task_fiber_start(&stack_receiving[0], STACKSIZE,
			(nano_fiber_entry_t)receiving, 0, 0, 7, 0);
#endif
}
