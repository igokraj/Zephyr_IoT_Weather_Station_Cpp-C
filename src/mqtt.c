#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/tls_credentials.h>
#include <string.h>
#include <stdio.h>

/*
 NOTE
 Parts marked with CONSTANT are standard AWS MQTT/TLS setup.
 They stay the same in every project like this one, only the values
 in secret.h (see secret_example.h) and the certs in src/creds/ change.
 Lines marked with CHANGE are the ones to adjust, the comment says when.
*/


/* User .h files */
#include "mqtt.h"
#include "creds/creds.h"
#include "secret.h"

LOG_MODULE_REGISTER(mqtt, LOG_LEVEL_DBG);

/* AWS IoT Core settings */
#define AWS_PORT          8883  /* CHANGE: 443 if the network blocks 8883 (needs ALPN, see AWS sample) */
#define MQTT_BUFFER_SIZE 256u  /* CHANGE: increase if topic + payload get bigger than ~200 B */
#define APP_BUFFER_SIZE  256u  /* CHANGE: increase for longer messages from AWS */
#define MQTT_TIMEOUT_MS  10000 /* CHANGE: longer for weak Wi-Fi, shorter to save battery */

#define TLS_TAG_DEVICE 1
#define TLS_TAG_AWS_CA 2

/*
 Credential tags the TLS connection may use: our cert + key (tag 1)
 and the Amazon Root CA used to check the server (tag 2)
*/

/* *** CONSTANT *** */
static const sec_tag_t sec_tls_tags[] = {
	TLS_TAG_DEVICE,
	TLS_TAG_AWS_CA,
};

/* *** CONSTANT *** */
static uint8_t rx_buffer[MQTT_BUFFER_SIZE];
static uint8_t tx_buffer[MQTT_BUFFER_SIZE];
static uint8_t buffer[APP_BUFFER_SIZE];
static struct mqtt_client client_ctx;
static struct net_sockaddr_storage aws_broker;
static bool connack_received;
static bool puback_received;
static bool message_received;


/*
 Register the device cert, its private key and the Amazon Root CA in the
 TLS credential store, so the TLS handshake can find them by tag
*/

/* *** CONSTANT *** */
static int setup_credentials(void)
{
	int ret;

	ret = tls_credential_add(TLS_TAG_DEVICE, TLS_CREDENTIAL_PUBLIC_CERTIFICATE,
				 public_cert, public_cert_len);
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("Failed to add device certificate (err %d)", ret);
		return ret;
	}

	ret = tls_credential_add(TLS_TAG_DEVICE, TLS_CREDENTIAL_PRIVATE_KEY,
				 private_key, private_key_len);
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("Failed to add device private key (err %d)", ret);
		return ret;
	}

	ret = tls_credential_add(TLS_TAG_AWS_CA, TLS_CREDENTIAL_CA_CERTIFICATE,
				 ca_cert, ca_cert_len);
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("Failed to add CA certificate (err %d)", ret);
		return ret;
	}
	return 0;
}

/* Look up the AWS endpoint in DNS and store its IP address in aws_broker */
/* *** CONSTANT *** */
static int resolve_broker_addr(void)
{
	struct zsock_addrinfo *ai = NULL;
	const struct zsock_addrinfo hints = {
		.ai_family = NET_AF_INET,
		.ai_socktype = NET_SOCK_STREAM,
	};
	char port_string[6];

	snprintf(port_string, sizeof(port_string), "%d", AWS_PORT);

	int ret = zsock_getaddrinfo(AWS_ENDPOINT, port_string, &hints, &ai);
	if (ret != 0) {
		LOG_ERR("Failed to resolve %s (err %d)", AWS_ENDPOINT, ret);
		return -EHOSTUNREACH;
	}

	memcpy(&aws_broker, ai->ai_addr, MIN(ai->ai_addrlen, sizeof(aws_broker)));
	zsock_freeaddrinfo(ai);
	return 0;
}

/*
 Read a message from AWS into buffer and confirm it with PUBACK (QoS 1).
 A message bigger than buffer is read to the end, but discarded.
*/
/* *** CONSTANT *** */
static ssize_t handle_published_message(const struct mqtt_publish_param *pub)
{
	int ret;
	size_t received = 0u;
	const size_t message_size = pub->message.payload.len;
	const bool discarded = message_size >= APP_BUFFER_SIZE;

	/* The topic has no '\0' at the end, so %.*s prints exactly size characters */
	LOG_INF("RECEIVED on topic \"%.*s\" [ id: %u qos: %u ] payload: %u / %u B",
		pub->message.topic.topic.size, (const char *)pub->message.topic.topic.utf8,
		pub->message_id, pub->message.topic.qos, message_size, APP_BUFFER_SIZE);

	while (received < message_size) {
		uint8_t *p = discarded ? buffer : &buffer[received];

		ret = mqtt_read_publish_payload_blocking(&client_ctx, p, APP_BUFFER_SIZE);
		if (ret < 0) {
			return ret;
		}

		received += ret;
	}

	if (!discarded) {
		buffer[message_size] = '\0';
	}

	/* Send ACK */
	switch (pub->message.topic.qos) {
	case MQTT_QOS_1_AT_LEAST_ONCE: {
		struct mqtt_puback_param puback;

		puback.message_id = pub->message_id;
		mqtt_publish_qos1_ack(&client_ctx, &puback);
	} break;
	case MQTT_QOS_2_EXACTLY_ONCE: /* unhandled (not supported by AWS) */
	case MQTT_QOS_0_AT_MOST_ONCE: /* nothing to do */
	default:
		break;
	}

	return discarded ? -ENOMEM : received;
}

/* Ask AWS to forward messages from AWS_SUBSCRIBE_TOPIC to this device */
/* *** CONSTANT *** */
static int subscribe_topic(void)
{
	int ret;
	struct mqtt_topic topics[] = {{
		.topic = {.utf8 = (const uint8_t*)AWS_SUBSCRIBE_TOPIC,
			  .size = strlen(AWS_SUBSCRIBE_TOPIC)},
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
	}};
	const struct mqtt_subscription_list sub_list = {
		.list = topics,
		.list_count = ARRAY_SIZE(topics),
		.message_id = 3u,
	};

	LOG_INF("Subscribing to %hu topic(s)", sub_list.list_count);

	ret = mqtt_subscribe(&client_ctx, &sub_list);
	if (ret != 0) {
		LOG_ERR("Failed to subscribe to topics: %d", ret);
	}

	return ret;
}

/*
 Called by mqtt_input() whenever a packet from AWS arrives.
 It only sets flags, wait_for() checks them.
*/
static void mqtt_event_cb(struct mqtt_client *client, const struct mqtt_evt *evt)
{
	switch (evt->type) {
	/* AWS accepted (or refused) our CONNECT */
	case MQTT_EVT_CONNACK:
		if (evt->result == 0) {
			LOG_INF("MQTT connected");
			connack_received = true;
		}
		else {
			LOG_ERR("MQTT connection refused (%d)", evt->result);
		}
		break;
	
	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *pub = &evt->param.publish;

		if (handle_published_message(pub) >= 0) {
		message_received = true;
		}
	} break;	

	/* AWS got our QoS 1 message */
	case MQTT_EVT_PUBACK:
		LOG_INF("Message delivered (id %u)", evt->param.puback.message_id);
		puback_received = true;
		break;
	case MQTT_EVT_DISCONNECT:
		LOG_INF("MQTT disconnected");
		break;
	case MQTT_EVT_SUBACK:
		LOG_INF("SUBACK result: %d, code: 0x%02x", evt->result,
			evt->param.suback.return_codes.data[0]);
		break;

	default:
		break;
	}
}

/* Fill in the MQTT client: who we are, where we connect, buffers and TLS */
/* *** CONSTANT *** */
static void client_setup(void)
{
	mqtt_client_init(&client_ctx);

	client_ctx.broker = &aws_broker;
	client_ctx.evt_cb = mqtt_event_cb;

	client_ctx.client_id.utf8 = (const uint8_t *)AWS_THING_NAME;
	client_ctx.client_id.size = strlen(AWS_THING_NAME);
	client_ctx.protocol_version = MQTT_VERSION_3_1_1;

	client_ctx.rx_buf = rx_buffer;
	client_ctx.rx_buf_size = sizeof(rx_buffer);
	client_ctx.tx_buf = tx_buffer;
	client_ctx.tx_buf_size = sizeof(tx_buffer);

	client_ctx.transport.type = MQTT_TRANSPORT_SECURE;
	struct mqtt_sec_config *tls_config = &client_ctx.transport.tls.config;

	tls_config->peer_verify = TLS_PEER_VERIFY_REQUIRED;
	tls_config->cipher_list = NULL;
	tls_config->sec_tag_list = sec_tls_tags;
	tls_config->sec_tag_count = ARRAY_SIZE(sec_tls_tags);
	tls_config->hostname = AWS_ENDPOINT;
}

/* Read packets from AWS until the callback sets *flag or the time runs out */
/* *** CONSTANT *** */
static int wait_for(bool *flag, int timeout_ms)
{
	struct zsock_pollfd fds = {
		.fd = client_ctx.transport.tls.sock,
		.events = ZSOCK_POLLIN,
	};
	int64_t deadline = k_uptime_get() + timeout_ms;

	while (!*flag) {
		int left = (int)(deadline - k_uptime_get());
		if (left <= 0) {
			return -ETIMEDOUT;
		}

		if (zsock_poll(&fds, 1, left) < 0) {
			return -errno;
		}

		if (fds.revents & ZSOCK_POLLIN) {
			int ret = mqtt_input(&client_ctx);
			if (ret != 0) {
				return ret;
			}
		}

		if (fds.revents & (ZSOCK_POLLHUP | ZSOCK_POLLERR)) {
			return -ECONNRESET;
		}
	}
	return 0;
}


/* Publish one message with QoS 1 and wait until AWS confirms it with PUBACK */
static int publish_and_wait(const char *topic, const char *payload, uint16_t id)
{
	struct mqtt_publish_param msg = {
		.message.topic.topic.utf8 = (const uint8_t *)topic,
		.message.topic.topic.size = strlen(topic),
		.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE, /* CHANGE: QoS 0 if delivery confirmation is not needed */
		.message.payload.data = (uint8_t *)payload,
		.message.payload.len = strlen(payload),
		.message_id = id,
	};

	/* Clear the flag so we wait for the PUBACK of this message, not an older one */
	puback_received = false;

	int ret = mqtt_publish(&client_ctx, &msg);
	if (ret != 0) {
		LOG_ERR("mqtt_publish failed (err %d)", ret);
		return ret;
	}
	LOG_INF("Published to %s: %s", topic, payload);

	/* CHANGE: remove this wait when using QoS 0, AWS sends no PUBACK then */
	ret = wait_for(&puback_received, MQTT_TIMEOUT_MS);
	if (ret != 0) {
		LOG_ERR("No PUBACK (err %d)", ret);
	}
	return ret;
}

/*
 Connect to AWS, publish the messages and disconnect.
 payload goes to AWS_PUBLISH_TOPIC_1, payload2 to AWS_PUBLISH_TOPIC_2
 (pass NULL as payload2 to skip it).
 Returns 0 only when AWS confirmed every message with PUBACK.
*/
int aws_mqtt_send(const char *payload, const char *payload2)
{
	int ret;

	connack_received = false;
	message_received = false;

	ret = setup_credentials();
	if (ret != 0) {
		return ret;
	}

	ret = resolve_broker_addr();
	if (ret != 0) {
		return ret;
	}

	client_setup();

	ret = mqtt_connect(&client_ctx);
	if (ret != 0) {
		LOG_ERR("mqtt_connect failed (err %d)", ret);
		return ret;
	}

	/* mqtt_connect() only sends CONNECT, the answer comes later */
	ret = wait_for(&connack_received, MQTT_TIMEOUT_MS);
	if (ret != 0) {
		LOG_ERR("No CONNACK (err %d)", ret);
		goto disconnect;
	}

	/* A failed subscribe should not stop the measurements from being sent */
	subscribe_topic();

	/* CHANGE: add another publish_and_wait() with the next id for another topic */
	ret = publish_and_wait(AWS_PUBLISH_TOPIC_1, payload, 1);
	if (ret != 0) {
		goto disconnect;
	}

	if (payload2 != NULL) {
		ret = publish_and_wait(AWS_PUBLISH_TOPIC_2, payload2, 2);
	}

	// Give aws a moment to deliver a message
	if (ret == 0) {
		wait_for(&message_received, 4000);
	}

disconnect:
	mqtt_disconnect(&client_ctx, NULL);
	return ret;
}


/*
Returns the message from AWS as a text
*/
char *aws_mqtt_get_message(void) {
	return message_received ? (char *)buffer : NULL;
}
