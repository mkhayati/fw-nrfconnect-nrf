#include "mqtt_cloud.h"
#include <net/mqtt.h>
#include <net/socket.h>
#include <net/cloud.h>
#include <random/rand32.h>
#include <stdio.h>

#if defined(CONFIG_AWS_FOTA)
#include <net/aws_fota.h>
#endif

#if defined(CONFIG_BOARD_QEMU_X86) && !defined(CONFIG_NRF_MODEM_LIB)
#include "certificates.h"
#endif

#include <logging/log.h>

LOG_MODULE_REGISTER(mqtt_cloud, CONFIG_MQTT_CLOUD_LOG_LEVEL);

BUILD_ASSERT(sizeof(CONFIG_MQTT_CLOUD_BROKER_HOST_NAME) > 1,
	    "MQTT Cloud hostname not set");

#if defined(CONFIG_MQTT_CLOUD_IPV6)
#define MQTT_AF_FAMILY AF_INET6
#else
#define MQTT_AF_FAMILY AF_INET
#endif

#define MQTT_TOPIC "things/"
#define MQTT_TOPIC_LEN (sizeof(MQTT_TOPIC))

#define MQTT_CLIENT_ID_PREFIX "%s"
#define MQTT_CLIENT_ID_LEN_MAX CONFIG_MQTT_CLOUD_CLIENT_ID_MAX_LEN

#define GET_TOPIC MQTT_TOPIC "%s/shadow/get"
#define GET_TOPIC_LEN (MQTT_TOPIC_LEN + MQTT_CLIENT_ID_LEN_MAX + 11)

#define UPDATE_TOPIC MQTT_TOPIC "%s/shadow/update"
#define UPDATE_TOPIC_LEN (MQTT_TOPIC_LEN + MQTT_CLIENT_ID_LEN_MAX + 14)

#define DELETE_TOPIC MQTT_TOPIC "%s/shadow/delete"
#define DELETE_TOPIC_LEN (MQTT_TOPIC_LEN + MQTT_CLIENT_ID_LEN_MAX + 14)

static char client_id_buf[MQTT_CLIENT_ID_LEN_MAX + 1];
static char get_topic[GET_TOPIC_LEN + 1];
static char update_topic[UPDATE_TOPIC_LEN + 1];
static char delete_topic[DELETE_TOPIC_LEN + 1];

#if defined(CONFIG_MQTT_CLOUD_TOPIC_UPDATE_ACCEPTED_SUBSCRIBE)
#define UPDATE_ACCEPTED_TOPIC MQTT_TOPIC "%s/shadow/update/accepted"
#define UPDATE_ACCEPTED_TOPIC_LEN (MQTT_TOPIC_LEN + MQTT_CLIENT_ID_LEN_MAX + 23)
static char update_accepted_topic[UPDATE_ACCEPTED_TOPIC_LEN + 1];
#endif

#if defined(CONFIG_MQTT_CLOUD_TOPIC_UPDATE_REJECTED_SUBSCRIBE)
#define UPDATE_REJECTED_TOPIC MQTT_TOPIC "%s/shadow/update/rejected"
#define UPDATE_REJECTED_TOPIC_LEN (MQTT_TOPIC_LEN + MQTT_CLIENT_ID_LEN_MAX + 23)
static char update_rejected_topic[UPDATE_REJECTED_TOPIC_LEN + 1];
#endif

#if defined(CONFIG_MQTT_CLOUD_TOPIC_UPDATE_DELTA_SUBSCRIBE)
#define UPDATE_DELTA_TOPIC MQTT_TOPIC "%s/shadow/update/delta"
#define UPDATE_DELTA_TOPIC_LEN (MQTT_TOPIC_LEN + MQTT_CLIENT_ID_LEN_MAX + 20)
static char update_delta_topic[UPDATE_DELTA_TOPIC_LEN + 1];
#endif

#if defined(CONFIG_MQTT_CLOUD_TOPIC_GET_ACCEPTED_SUBSCRIBE)
#define GET_ACCEPTED_TOPIC MQTT_TOPIC "%s/shadow/get/accepted"
#define GET_ACCEPTED_TOPIC_LEN (MQTT_TOPIC_LEN + MQTT_CLIENT_ID_LEN_MAX + 20)
static char get_accepted_topic[GET_ACCEPTED_TOPIC_LEN + 1];
#endif

#if defined(CONFIG_MQTT_CLOUD_TOPIC_GET_REJECTED_SUBSCRIBE)
#define GET_REJECTED_TOPIC MQTT_TOPIC "%s/shadow/get/rejected"
#define GET_REJECTED_TOPIC_LEN (MQTT_TOPIC_LEN + MQTT_CLIENT_ID_LEN_MAX + 20)
static char get_rejected_topic[GET_REJECTED_TOPIC_LEN + 1];
#endif

#if defined(CONFIG_MQTT_CLOUD_TOPIC_DELETE_ACCEPTED_SUBSCRIBE)
#define DELETE_ACCEPTED_TOPIC MQTT_TOPIC "%s/shadow/delete/accepted"
#define DELETE_ACCEPTED_TOPIC_LEN (MQTT_TOPIC_LEN + MQTT_CLIENT_ID_LEN_MAX + 23)
static char delete_accepted_topic[DELETE_ACCEPTED_TOPIC_LEN + 1];
#endif

#if defined(CONFIG_MQTT_CLOUD_TOPIC_DELETE_REJECTED_SUBSCRIBE)
#define DELETE_REJECTED_TOPIC MQTT_TOPIC "%s/shadow/delete/rejected"
#define DELETE_REJECTED_TOPIC_LEN (MQTT_TOPIC_LEN + MQTT_CLIENT_ID_LEN_MAX + 23)
static char delete_rejected_topic[DELETE_REJECTED_TOPIC_LEN + 1];
#endif

#if defined(CONFIG_CLOUD_API)
static struct cloud_backend *mqtt_cloud_backend;
#endif

#define MQTT_CLOUD_POLL_TIMEOUT_MS 500

static struct mqtt_cloud_app_topic_data app_topic_data;
static struct mqtt_client client;
static struct sockaddr_storage broker;

static char rx_buffer[CONFIG_MQTT_CLOUD_MQTT_RX_TX_BUFFER_LEN];
static char tx_buffer[CONFIG_MQTT_CLOUD_MQTT_RX_TX_BUFFER_LEN];
static char payload_buf[CONFIG_MQTT_CLOUD_MQTT_PAYLOAD_BUFFER_LEN];

static mqtt_cloud_evt_handler_t module_evt_handler;

static atomic_t disconnect_requested;
static atomic_t connection_poll_active;

static K_SEM_DEFINE(connection_poll_sem, 0, 1);

static int connect_error_translate(const int err)
{
	switch (err) {
#if defined(CONFIG_CLOUD_API)
	case 0:
		return CLOUD_CONNECT_RES_SUCCESS;
	case -ECHILD:
		return CLOUD_CONNECT_RES_ERR_NETWORK;
	case -EACCES:
		return CLOUD_CONNECT_RES_ERR_NOT_INITD;
	case -ENOEXEC:
		return CLOUD_CONNECT_RES_ERR_BACKEND;
	case -EINVAL:
		return CLOUD_CONNECT_RES_ERR_PRV_KEY;
	case -EOPNOTSUPP:
		return CLOUD_CONNECT_RES_ERR_CERT;
	case -ECONNREFUSED:
		return CLOUD_CONNECT_RES_ERR_CERT_MISC;
	case -ETIMEDOUT:
		return CLOUD_CONNECT_RES_ERR_TIMEOUT_NO_DATA;
	case -ENOMEM:
		return CLOUD_CONNECT_RES_ERR_NO_MEM;
	case -EINPROGRESS:
		return CLOUD_CONNECT_RES_ERR_ALREADY_CONNECTED;
	default:
		LOG_ERR("MQTT Cloud backend connect failed %d", err);
		return CLOUD_CONNECT_RES_ERR_MISC;
#else
	case 0:
		return MQTT_CLOUD_CONNECT_RES_SUCCESS;
	case -ECHILD:
		return MQTT_CLOUD_CONNECT_RES_ERR_NETWORK;
	case -EACCES:
		return MQTT_CLOUD_CONNECT_RES_ERR_NOT_INITD;
	case -ENOEXEC:
		return MQTT_CLOUD_CONNECT_RES_ERR_BACKEND;
	case -EINVAL:
		return MQTT_CLOUD_CONNECT_RES_ERR_PRV_KEY;
	case -EOPNOTSUPP:
		return MQTT_CLOUD_CONNECT_RES_ERR_CERT;
	case -ECONNREFUSED:
		return MQTT_CLOUD_CONNECT_RES_ERR_CERT_MISC;
	case -ETIMEDOUT:
		return MQTT_CLOUD_CONNECT_RES_ERR_TIMEOUT_NO_DATA;
	case -ENOMEM:
		return MQTT_CLOUD_CONNECT_RES_ERR_NO_MEM;
	case -EINPROGRESS:
		return MQTT_CLOUD_CONNECT_RES_ERR_ALREADY_CONNECTED;
	default:
		LOG_ERR("MQTT broker connect failed %d", err);
		return CLOUD_CONNECT_RES_ERR_MISC;
#endif /* !defined(CONFIG_CLOUD_API) */
	}
}

#if defined(CONFIG_CLOUD_API)
static int api_connect_error_translate(const int err)
{
	switch (err) {
	case MQTT_CLOUD_DISCONNECT_USER_REQUEST:
		return CLOUD_DISCONNECT_USER_REQUEST;
	case MQTT_CLOUD_DISCONNECT_CLOSED_BY_REMOTE:
		return CLOUD_DISCONNECT_CLOSED_BY_REMOTE;
	case MQTT_CLOUD_DISCONNECT_INVALID_REQUEST:
		return CLOUD_DISCONNECT_INVALID_REQUEST;
	case MQTT_CLOUD_DISCONNECT_MISC:
		return CLOUD_DISCONNECT_MISC;
	case MQTT_CLOUD_CONNECT_RES_ERR_NETWORK:
		return CLOUD_CONNECT_RES_ERR_NETWORK;
	case MQTT_CLOUD_CONNECT_RES_ERR_NOT_INITD:
		return CLOUD_CONNECT_RES_ERR_NOT_INITD;
	case MQTT_CLOUD_CONNECT_RES_ERR_BACKEND:
		return CLOUD_CONNECT_RES_ERR_BACKEND;
	case MQTT_CLOUD_CONNECT_RES_ERR_PRV_KEY:
		return CLOUD_CONNECT_RES_ERR_PRV_KEY;
	case MQTT_CLOUD_CONNECT_RES_ERR_CERT:
		return CLOUD_CONNECT_RES_ERR_CERT;
	case MQTT_CLOUD_CONNECT_RES_ERR_CERT_MISC:
		return CLOUD_CONNECT_RES_ERR_CERT_MISC;
	case MQTT_CLOUD_CONNECT_RES_ERR_TIMEOUT_NO_DATA:
		return CLOUD_CONNECT_RES_ERR_TIMEOUT_NO_DATA;
	case MQTT_CLOUD_CONNECT_RES_ERR_NO_MEM:
		return CLOUD_CONNECT_RES_ERR_NO_MEM;
	case MQTT_CLOUD_CONNECT_RES_ERR_ALREADY_CONNECTED:
		return CLOUD_CONNECT_RES_ERR_ALREADY_CONNECTED;
	default:
		LOG_ERR("Unknown error event %d", err);
		return -ENODATA;
	}
}
#endif /* !defined(CONFIG_CLOUD_API) */

static void mqtt_cloud_notify_event(const struct mqtt_cloud_evt *mqtt_cloud_evt)
{
#if defined(CONFIG_CLOUD_API)

	struct cloud_backend_config *config = mqtt_cloud_backend->config;
	struct cloud_event cloud_evt = { 0 };

	switch (mqtt_cloud_evt->type) {
	case MQTT_CLOUD_EVT_CONNECTING:
		cloud_evt.type = CLOUD_EVT_CONNECTING;
		break;
	case MQTT_CLOUD_EVT_CONNECTED:
		cloud_evt.data.persistent_session =
				mqtt_cloud_evt->data.persistent_session;
		cloud_evt.type = CLOUD_EVT_CONNECTED;
		cloud_evt.data.err =
		api_connect_error_translate(mqtt_cloud_evt->data.err);
		break;
	case MQTT_CLOUD_EVT_READY:
		cloud_evt.type = CLOUD_EVT_READY;
		break;
	case MQTT_CLOUD_EVT_DISCONNECTED:
		cloud_evt.type = CLOUD_EVT_DISCONNECTED;
		cloud_evt.data.err =
		api_connect_error_translate(mqtt_cloud_evt->data.err);
		break;
	case MQTT_CLOUD_EVT_DATA_RECEIVED:
		cloud_evt.type = CLOUD_EVT_DATA_RECEIVED;
		cloud_evt.data.msg.buf = mqtt_cloud_evt->data.msg.ptr;
		cloud_evt.data.msg.len = mqtt_cloud_evt->data.msg.len;
		cloud_evt.data.msg.endpoint.type = CLOUD_EP_MSG;
		cloud_evt.data.msg.endpoint.str =
				(char *)mqtt_cloud_evt->data.msg.topic.str;
		cloud_evt.data.msg.endpoint.len =
				mqtt_cloud_evt->data.msg.topic.len;
		break;
	case MQTT_CLOUD_EVT_FOTA_START:
		cloud_evt.type = CLOUD_EVT_FOTA_START;
		break;
	case MQTT_CLOUD_EVT_FOTA_DONE:
		cloud_evt.type = CLOUD_EVT_FOTA_DONE;
		break;
	case MQTT_CLOUD_EVT_FOTA_ERASE_PENDING:
		cloud_evt.type = CLOUD_EVT_FOTA_ERASE_PENDING;
		break;
	case MQTT_CLOUD_EVT_FOTA_ERASE_DONE:
		cloud_evt.type = CLOUD_EVT_FOTA_ERASE_DONE;
		break;
	case MQTT_CLOUD_EVT_ERROR:
		cloud_evt.data.err = mqtt_cloud_evt->data.err;
		cloud_evt.type = CLOUD_EVT_ERROR;
		break;
	case MQTT_CLOUD_EVT_FOTA_DL_PROGRESS:
		cloud_evt.type = CLOUD_EVT_FOTA_DL_PROGRESS;
		cloud_evt.data.fota_progress =
				mqtt_cloud_evt->data.fota_progress;
		break;
	default:
		LOG_ERR("Unknown MQTT Cloud event");
		break;
	}

	cloud_notify_event(mqtt_cloud_backend, &cloud_evt, config->user_data);
#else
	if ((module_evt_handler != NULL) && (mqtt_cloud_evt != NULL)) {
		module_evt_handler(mqtt_cloud_evt);
	} else {
		LOG_ERR("Library event handler not registered, or empty event");
	}
#endif /* !defined(CONFIG_CLOUD_API) */
}

#if defined(CONFIG_AWS_FOTA)
static void mqtt_fota_cb_handler(struct aws_fota_event *fota_evt)
{
	struct mqtt_cloud_evt mqtt_cloud_evt = { 0 };

	if (fota_evt == NULL) {
		return;
	}

	switch (fota_evt->id) {
	case MQTT_FOTA_EVT_START:
		LOG_DBG("MQTT_FOTA_EVT_START");
		mqtt_cloud_evt.type = MQTT_CLOUD_EVT_FOTA_START;
		break;
	case MQTT_FOTA_EVT_DONE:
		LOG_DBG("MQTT_FOTA_EVT_DONE");
		mqtt_cloud_evt.type = MQTT_CLOUD_EVT_FOTA_DONE;
		break;
	case MQTT_FOTA_EVT_ERASE_PENDING:
		LOG_DBG("MQTT_FOTA_EVT_ERASE_PENDING");
		mqtt_cloud_evt.type = MQTT_CLOUD_EVT_FOTA_ERASE_PENDING;
		break;
	case MQTT_FOTA_EVT_ERASE_DONE:
		LOG_DBG("MQTT_FOTA_EVT_ERASE_DONE");
		mqtt_cloud_evt.type = MQTT_CLOUD_EVT_FOTA_ERASE_DONE;
		break;
	case MQTT_FOTA_EVT_ERROR:
		LOG_ERR("MQTT_FOTA_EVT_ERROR");
		mqtt_cloud_evt.type = MQTT_CLOUD_EVT_ERROR;
		break;
	case MQTT_FOTA_EVT_DL_PROGRESS:
		LOG_DBG("MQTT_FOTA_EVT_DL_PROGRESS, (%d%%)",
			fota_evt->dl.progress);
		mqtt_cloud_evt.type = MQTT_CLOUD_EVT_FOTA_DL_PROGRESS;
		break;
	default:
		LOG_ERR("Unknown FOTA event");
		return;
	}

	mqtt_cloud_notify_event(&mqtt_cloud_evt);
}
#endif

static int mqtt_cloud_topics_populate(char *const id, size_t id_len)
{
	int err;
#if defined(CONFIG_MQTT_CLOUD_CLIENT_ID_APP)
	err = snprintf(client_id_buf, sizeof(client_id_buf),
		       MQTT_CLIENT_ID_PREFIX, id);
	if (err <= 0) {
		return -EINVAL;
	}
	if (err >= sizeof(client_id_buf)) {
		return -ENOMEM;
	}
#else
	err = snprintf(client_id_buf, sizeof(client_id_buf),
		       MQTT_CLIENT_ID_PREFIX, CONFIG_MQTT_CLOUD_CLIENT_ID_STATIC);
	if (err <= 0) {
		return -EINVAL;
	}
	if (err >= sizeof(client_id_buf)) {
		return -ENOMEM;
	}
#endif
	err = snprintf(get_topic, sizeof(get_topic),
		       GET_TOPIC, client_id_buf);
	if (err >= GET_TOPIC_LEN) {
		return -ENOMEM;
	}

	err = snprintf(update_topic, sizeof(update_topic),
		       UPDATE_TOPIC, client_id_buf);
	if (err >= UPDATE_TOPIC_LEN) {
		return -ENOMEM;
	}

	err = snprintf(delete_topic, sizeof(delete_topic),
		       DELETE_TOPIC, client_id_buf);
	if (err >= DELETE_TOPIC_LEN) {
		return -ENOMEM;
	}

#if defined(CONFIG_MQTT_CLOUD_TOPIC_GET_ACCEPTED_SUBSCRIBE)
	err = snprintf(get_accepted_topic, sizeof(get_accepted_topic),
		       GET_ACCEPTED_TOPIC, client_id_buf);
	if (err >= GET_ACCEPTED_TOPIC_LEN) {
		return -ENOMEM;
	}
#endif
#if defined(CONFIG_MQTT_CLOUD_TOPIC_GET_REJECTED_SUBSCRIBE)
	err = snprintf(get_rejected_topic, sizeof(get_rejected_topic),
		       GET_REJECTED_TOPIC, client_id_buf);
	if (err >= GET_REJECTED_TOPIC_LEN) {
		return -ENOMEM;
	}
#endif
#if defined(CONFIG_MQTT_CLOUD_TOPIC_UPDATE_ACCEPTED_SUBSCRIBE)
	err = snprintf(update_accepted_topic, sizeof(update_accepted_topic),
		       UPDATE_ACCEPTED_TOPIC, client_id_buf);
	if (err >= UPDATE_ACCEPTED_TOPIC_LEN) {
		return -ENOMEM;
	}
#endif
#if defined(CONFIG_MQTT_CLOUD_TOPIC_UPDATE_REJECTED_SUBSCRIBE)
	err = snprintf(update_rejected_topic, sizeof(update_rejected_topic),
		       UPDATE_REJECTED_TOPIC, client_id_buf);
	if (err >= UPDATE_REJECTED_TOPIC_LEN) {
		return -ENOMEM;
	}
#endif
#if defined(CONFIG_MQTT_CLOUD_TOPIC_UPDATE_DELTA_SUBSCRIBE)
	err = snprintf(update_delta_topic, sizeof(update_delta_topic),
		       UPDATE_DELTA_TOPIC, client_id_buf);
	if (err >= UPDATE_DELTA_TOPIC_LEN) {
		return -ENOMEM;
	}
#endif
#if defined(CONFIG_MQTT_CLOUD_TOPIC_DELETE_ACCEPTED_SUBSCRIBE)
	err = snprintf(delete_accepted_topic, sizeof(delete_accepted_topic),
		       DELETE_ACCEPTED_TOPIC, client_id_buf);
	if (err >= DELETE_ACCEPTED_TOPIC_LEN) {
		return -ENOMEM;
	}
#endif
#if defined(CONFIG_MQTT_CLOUD_TOPIC_DELETE_REJECTED_SUBSCRIBE)
	err = snprintf(delete_rejected_topic, sizeof(delete_rejected_topic),
		       DELETE_REJECTED_TOPIC, client_id_buf);
	if (err >= DELETE_REJECTED_TOPIC_LEN) {
		return -ENOMEM;
	}
#endif
	return 0;
}

/** Returns the number of topics subscribed to (0 or greater),
  * or a negative error code. */
static int topic_subscribe(void)
{
	int err;
	const struct mqtt_topic mqtt_cloud_rx_list[] = {
#if defined(CONFIG_MQTT_CLOUD_TOPIC_GET_ACCEPTED_SUBSCRIBE)
		{
			.topic = {
				.utf8 = get_accepted_topic,
				.size = strlen(get_accepted_topic)
			},
			.qos = MQTT_QOS_1_AT_LEAST_ONCE
		},
#endif
#if defined(CONFIG_MQTT_CLOUD_TOPIC_GET_REJECTED_SUBSCRIBE)
		{
			.topic = {
				.utf8 = get_rejected_topic,
				.size = strlen(get_rejected_topic)
			},
			.qos = MQTT_QOS_1_AT_LEAST_ONCE
		},
#endif
#if defined(CONFIG_MQTT_CLOUD_TOPIC_UPDATE_ACCEPTED_SUBSCRIBE)
		{
			.topic = {
				.utf8 = update_accepted_topic,
				.size = strlen(update_accepted_topic)
			},
			.qos = MQTT_QOS_1_AT_LEAST_ONCE
		},
#endif
#if defined(CONFIG_MQTT_CLOUD_TOPIC_UPDATE_REJECTED_SUBSCRIBE)
		{
			.topic = {
				.utf8 = update_rejected_topic,
				.size = strlen(update_rejected_topic)
			},
			.qos = MQTT_QOS_1_AT_LEAST_ONCE
		},
#endif
#if defined(CONFIG_MQTT_CLOUD_TOPIC_UPDATE_DELTA_SUBSCRIBE)
		{
			.topic = {
				.utf8 = update_delta_topic,
				.size = strlen(update_delta_topic)
			},
			.qos = MQTT_QOS_1_AT_LEAST_ONCE
		},
#endif
#if defined(CONFIG_MQTT_CLOUD_TOPIC_DELETE_ACCEPTED_SUBSCRIBE)
		{
			.topic = {
				.utf8 = delete_accepted_topic,
				.size = strlen(delete_accepted_topic)
			},
			.qos = MQTT_QOS_1_AT_LEAST_ONCE
		},
#endif
#if defined(CONFIG_MQTT_CLOUD_TOPIC_DELETE_REJECTED_SUBSCRIBE)
		{
			.topic = {
				.utf8 = delete_rejected_topic,
				.size = strlen(delete_rejected_topic)
			},
			.qos = MQTT_QOS_1_AT_LEAST_ONCE
		},
#endif
	};

	if (app_topic_data.list_count > 0) {
		const struct mqtt_subscription_list app_sub_list = {
			.list = app_topic_data.list,
			.list_count = app_topic_data.list_count,
			.message_id = sys_rand32_get()
		};

		for (size_t i = 0; i < app_sub_list.list_count; i++) {
			LOG_DBG("Subscribing to application topic: %s",
				log_strdup(app_sub_list.list[i].topic.utf8));
		}

		err = mqtt_subscribe(&client, &app_sub_list);
		if (err) {
			LOG_ERR("Application topics subscribe, error: %d", err);
		}
	}

	if (ARRAY_SIZE(mqtt_cloud_rx_list) > 0) {
		const struct mqtt_subscription_list mqtt_sub_list = {
			.list = (struct mqtt_topic *)&mqtt_cloud_rx_list,
			.list_count = ARRAY_SIZE(mqtt_cloud_rx_list),
			.message_id = sys_rand32_get()
		};

		for (size_t i = 0; i < mqtt_sub_list.list_count; i++) {
			LOG_DBG("Subscribing to MQTT shadow topic: %s",
				log_strdup(mqtt_sub_list.list[i].topic.utf8));
		}

		err = mqtt_subscribe(&client, &mqtt_sub_list);
		if (err) {
			LOG_ERR("MQTT shadow topics subscribe, error: %d", err);
		}
	}

	if (err < 0) {
		return err;
	}
	return app_topic_data.list_count + ARRAY_SIZE(mqtt_cloud_rx_list);
}

static int publish_get_payload(struct mqtt_client *const c, size_t length)
{
	if (length > sizeof(payload_buf)) {
		LOG_ERR("Incoming MQTT message too large for payload buffer");
		return -EMSGSIZE;
	}

	return mqtt_readall_publish_payload(c, payload_buf, length);
}

static void mqtt_evt_handler(struct mqtt_client *const c,
			     const struct mqtt_evt *mqtt_evt)
{
	int err;
	struct mqtt_cloud_evt mqtt_cloud_evt = { 0 };

#if defined(CONFIG_AWS_FOTA)
	err = aws_fota_mqtt_evt_handler(c, mqtt_evt);
	if (err == 0) {
		/* Event handled by FOTA library so it can be skipped. */
		return;
	} else if (err < 0) {
		LOG_ERR("aws_fota_mqtt_evt_handler, error: %d", err);
		LOG_DBG("Disconnecting MQTT client...");

		atomic_set(&disconnect_requested, 1);
		err = mqtt_disconnect(c);
		if (err) {
			LOG_ERR("Could not disconnect: %d", err);
			mqtt_cloud_evt.type = MQTT_CLOUD_EVT_ERROR;
			mqtt_cloud_evt.data.err = err;
			mqtt_cloud_notify_event(&mqtt_cloud_evt);
		}
	}
#endif

	switch (mqtt_evt->type) {
	case MQTT_EVT_CONNACK:
		if (mqtt_evt->param.connack.return_code) {
			LOG_ERR("MQTT_EVT_CONNACK, error: %d",
				mqtt_evt->param.connack.return_code);
			mqtt_cloud_evt.data.err =
				mqtt_evt->param.connack.return_code;
			mqtt_cloud_evt.type = MQTT_CLOUD_EVT_ERROR;
			mqtt_cloud_notify_event(&mqtt_cloud_evt);
			break;
		}

		LOG_DBG("MQTT client connected!");

		mqtt_cloud_evt.data.persistent_session =
				   !IS_ENABLED(CONFIG_MQTT_CLEAN_SESSION) &&
				   mqtt_evt->param.connack.session_present_flag;
		mqtt_cloud_evt.type = MQTT_CLOUD_EVT_CONNECTED;
		mqtt_cloud_notify_event(&mqtt_cloud_evt);

		if (!mqtt_evt->param.connack.session_present_flag) {
			int res = topic_subscribe();

			if (res < 0) {
				mqtt_cloud_evt.type = MQTT_CLOUD_EVT_ERROR;
				mqtt_cloud_evt.data.err = err;
				mqtt_cloud_notify_event(&mqtt_cloud_evt);
				break;
			}
			if (res == 0) {
				/* There were not topics to subscribe to. */
				mqtt_cloud_evt.type = MQTT_CLOUD_EVT_READY;
				mqtt_cloud_notify_event(&mqtt_cloud_evt);
			} /* else: wait for SUBACK */
		} else {
			/** pre-existing session:
			  * subscription is already established */
			mqtt_cloud_evt.type = MQTT_CLOUD_EVT_READY;
			mqtt_cloud_notify_event(&mqtt_cloud_evt);
		}
		break;
	case MQTT_EVT_DISCONNECT:
		LOG_DBG("MQTT_EVT_DISCONNECT: result = %d", mqtt_evt->result);
		mqtt_cloud_evt.type = MQTT_CLOUD_EVT_DISCONNECTED;
		mqtt_cloud_notify_event(&mqtt_cloud_evt);
		break;
	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *p = &mqtt_evt->param.publish;

		LOG_DBG("MQTT_EVT_PUBLISH: id = %d len = %d ",
			p->message_id,
			p->message.payload.len);

		err = publish_get_payload(c, p->message.payload.len);
		if (err) {
			LOG_ERR("publish_get_payload, error: %d", err);
			break;
		}

		if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			const struct mqtt_puback_param ack = {
				.message_id = p->message_id
			};

			mqtt_publish_qos1_ack(c, &ack);
		}

		mqtt_cloud_evt.type = MQTT_CLOUD_EVT_DATA_RECEIVED;
		mqtt_cloud_evt.data.msg.ptr = payload_buf;
		mqtt_cloud_evt.data.msg.len = p->message.payload.len;
		mqtt_cloud_evt.data.msg.topic.type = MQTT_CLOUD_SHADOW_TOPIC_UNKNOWN;
		mqtt_cloud_evt.data.msg.topic.str = p->message.topic.topic.utf8;
		mqtt_cloud_evt.data.msg.topic.len = p->message.topic.topic.size;

		mqtt_cloud_notify_event(&mqtt_cloud_evt);
	} break;
	case MQTT_EVT_PUBACK:
		LOG_DBG("MQTT_EVT_PUBACK: id = %d result = %d",
			mqtt_evt->param.puback.message_id,
			mqtt_evt->result);
		break;
	case MQTT_EVT_SUBACK:
		LOG_DBG("MQTT_EVT_SUBACK: id = %d result = %d",
			mqtt_evt->param.suback.message_id,
			mqtt_evt->result);
		/* MQTT subscription established. */
		mqtt_cloud_evt.type = MQTT_CLOUD_EVT_READY;
		mqtt_cloud_notify_event(&mqtt_cloud_evt);
		break;
	default:
		break;
	}
}

#if !defined(CONFIG_NRF_MODEM_LIB)
static int certificates_provision(void)
{
	static bool certs_added;
	int err;

	if (!IS_ENABLED(CONFIG_NET_SOCKETS_SOCKOPT_TLS) || certs_added) {
		return 0;
	}

	err = tls_credential_add(CONFIG_MQTT_CLOUD_SEC_TAG,
				 TLS_CREDENTIAL_CA_CERTIFICATE,
				 MQTT_CLOUD_CA_CERTIFICATE,
				 sizeof(MQTT_CLOUD_CA_CERTIFICATE));
	if (err < 0) {
		LOG_ERR("Failed to register CA certificate: %d",
			err);
		return err;
	}

	err = tls_credential_add(CONFIG_MQTT_CLOUD_SEC_TAG,
				 TLS_CREDENTIAL_PRIVATE_KEY,
				 MQTT_CLOUD_CLIENT_PRIVATE_KEY,
				 sizeof(MQTT_CLOUD_CLIENT_PRIVATE_KEY));
	if (err < 0) {
		LOG_ERR("Failed to register private key: %d", err);
		return err;
	}

	err = tls_credential_add(CONFIG_MQTT_CLOUD_SEC_TAG,
				 TLS_CREDENTIAL_SERVER_CERTIFICATE,
				 MQTT_CLOUD_CLIENT_PUBLIC_CERTIFICATE,
				 sizeof(MQTT_CLOUD_CLIENT_PUBLIC_CERTIFICATE));
	if (err < 0) {
		LOG_ERR("Failed to register public certificate: %d", err);
		return err;
	}

	certs_added = true;

	return 0;
}
#endif /* !defined(CONFIG_NRF_MODEM_LIB) */

#if defined(CONFIG_MQTT_CLOUD_STATIC_IPV4)
static int broker_init(void)
{
	struct sockaddr_in *broker4 =
		((struct sockaddr_in *)&broker);

	inet_pton(AF_INET, CONFIG_MQTT_CLOUD_STATIC_IPV4_ADDR,
		  &broker->sin_addr);
	broker4->sin_family = AF_INET;
	broker4->sin_port = htons(CONFIG_MQTT_CLOUD_PORT);

	LOG_DBG("IPv4 Address %s", log_strdup(CONFIG_MQTT_CLOUD_STATIC_IPV4_ADDR));

	return 0;
}
#else
static int broker_init(void)
{
	int err;
	struct addrinfo *result;
	struct addrinfo *addr;
	struct addrinfo hints = {
		.ai_family = MQTT_AF_FAMILY,
		.ai_socktype = SOCK_STREAM
	};

	err = getaddrinfo(CONFIG_MQTT_CLOUD_BROKER_HOST_NAME,
			  NULL, &hints, &result);
	if (err) {
		LOG_ERR("getaddrinfo, error %d", err);
		return -ECHILD;
	}

	addr = result;

	while (addr != NULL) {
		if ((addr->ai_addrlen == sizeof(struct sockaddr_in)) &&
		    (MQTT_AF_FAMILY == AF_INET)) {
			struct sockaddr_in *broker4 =
				((struct sockaddr_in *)&broker);
			char ipv4_addr[NET_IPV4_ADDR_LEN];

			broker4->sin_addr.s_addr =
				((struct sockaddr_in *)addr->ai_addr)
				->sin_addr.s_addr;
			broker4->sin_family = AF_INET;
			broker4->sin_port = htons(CONFIG_MQTT_CLOUD_PORT);

			inet_ntop(AF_INET, &broker4->sin_addr.s_addr, ipv4_addr,
				  sizeof(ipv4_addr));
			LOG_DBG("IPv4 Address found %s", log_strdup(ipv4_addr));
			break;
		} else if ((addr->ai_addrlen == sizeof(struct sockaddr_in6)) &&
			   (MQTT_AF_FAMILY == AF_INET6)) {
			struct sockaddr_in6 *broker6 =
				((struct sockaddr_in6 *)&broker);
			char ipv6_addr[NET_IPV6_ADDR_LEN];

			memcpy(broker6->sin6_addr.s6_addr,
			       ((struct sockaddr_in6 *)addr->ai_addr)
			       ->sin6_addr.s6_addr,
			       sizeof(struct in6_addr));
			broker6->sin6_family = AF_INET6;
			broker6->sin6_port = htons(CONFIG_MQTT_CLOUD_PORT);

			inet_ntop(AF_INET6, &broker6->sin6_addr.s6_addr,
				  ipv6_addr, sizeof(ipv6_addr));
			LOG_DBG("IPv4 Address found %s", log_strdup(ipv6_addr));
			break;
		}

		LOG_DBG("ai_addrlen = %u should be %u or %u",
			(unsigned int)addr->ai_addrlen,
			(unsigned int)sizeof(struct sockaddr_in),
			(unsigned int)sizeof(struct sockaddr_in6));

		addr = addr->ai_next;
		break;
	}

	freeaddrinfo(result);

	return err;
}
#endif /* !defined(CONFIG_MQTT_CLOUD_STATIC_IP) */

static int client_broker_init(struct mqtt_client *const client)
{
	int err;
#if defined(CONFIG_MQTT_CLOUD_AUTH)
	static struct mqtt_utf8 password;
	static struct mqtt_utf8 username;
#endif

	mqtt_client_init(client);

	err = broker_init();
	if (err) {
		return err;
	}

	client->broker			= &broker;
	client->evt_cb			= mqtt_evt_handler;
	client->client_id.utf8		= (char *)client_id_buf;
	client->client_id.size		= strlen(client_id_buf);
#if defined(CONFIG_MQTT_CLOUD_AUTH)
	password.utf8			= (char *)CONFIG_MQTT_CLOUD_PASSWORD;
	password.size			= strlen(CONFIG_MQTT_CLOUD_PASSWORD);
	client->password 		= &password;
#else
	client->password		= NULL;
#endif
#if defined(CONFIG_MQTT_CLOUD_AUTH)
	username.utf8 			= (char *)CONFIG_MQTT_CLOUD_USERNAME;
	username.size 			= strlen(CONFIG_MQTT_CLOUD_USERNAME);
	client->user_name		= &username;
#else
	client->user_name		= NULL;
#endif
	client->protocol_version	= MQTT_VERSION_3_1_1;
	client->rx_buf			= rx_buffer;
	client->rx_buf_size		= sizeof(rx_buffer);
	client->tx_buf			= tx_buffer;
	client->tx_buf_size		= sizeof(tx_buffer);
#if defined(MQTT_CLOUD_TLS)
	client->transport.type		= MQTT_TRANSPORT_SECURE;

	static sec_tag_t sec_tag_list[] = { CONFIG_MQTT_CLOUD_SEC_TAG };
	struct mqtt_sec_config *tls_cfg = &(client->transport).tls.config;

	tls_cfg->peer_verify		= 2;
	tls_cfg->cipher_count		= 0;
	tls_cfg->cipher_list		= NULL;
	tls_cfg->sec_tag_count		= ARRAY_SIZE(sec_tag_list);
	tls_cfg->sec_tag_list		= sec_tag_list;
	tls_cfg->hostname		= CONFIG_MQTT_CLOUD_BROKER_HOST_NAME;

#if defined(CONFIG_NRF_MODEM_LIB)
	tls_cfg->session_cache		=
		IS_ENABLED(CONFIG_MQTT_CLOUD_TLS_SESSION_CACHING) ?
			TLS_SESSION_CACHE_ENABLED : TLS_SESSION_CACHE_DISABLED;
#else
	/* TLS session caching is not supported by the Zephyr network stack */
	tls_cfg->session_cache = TLS_SESSION_CACHE_DISABLED;

	err = certificates_provision();
	if (err) {
		LOG_ERR("Could not provision certificates, error: %d", err);
		return err;
	}
#endif /* !defined(CONFIG_NRF_MODEM_LIB) */
#endif /* defined(MQTT_CLOUD_TLS) */

	return err;
}

static int connection_poll_start(void)
{
	if (atomic_get(&connection_poll_active)) {
		LOG_DBG("Connection poll in progress");
		return -EINPROGRESS;
	}

	atomic_set(&disconnect_requested, 0);
	k_sem_give(&connection_poll_sem);

	return 0;
}

int mqtt_cloud_ping(void)
{
	return mqtt_ping(&client);
}

int mqtt_cloud_keepalive_time_left(void)
{
	return (int)mqtt_keepalive_time_left(&client);
}

int mqtt_cloud_input(void)
{
	return mqtt_input(&client);
}

int mqtt_cloud_send(const struct mqtt_cloud_data *const tx_data)
{
	struct mqtt_cloud_data tx_data_pub = {
		.ptr	    = tx_data->ptr,
		.len	    = tx_data->len,
		.qos	    = tx_data->qos,
		.topic.type = tx_data->topic.type,
		.topic.str  = tx_data->topic.str,
		.topic.len  = tx_data->topic.len
	};

	switch (tx_data_pub.topic.type) {
#if defined(CONFIG_CLOUD_API)
	case CLOUD_EP_STATE_GET:
		tx_data_pub.topic.str = get_topic;
		tx_data_pub.topic.len = strlen(get_topic);
		break;
	case CLOUD_EP_STATE:
		tx_data_pub.topic.str = update_topic;
		tx_data_pub.topic.len = strlen(update_topic);
		break;
	case CLOUD_EP_STATE_DELETE:
		tx_data_pub.topic.str = delete_topic;
		tx_data_pub.topic.len = strlen(delete_topic);
		break;
#else
	case MQTT_CLOUD_SHADOW_TOPIC_GET:
		tx_data_pub.topic.str = get_topic;
		tx_data_pub.topic.len = strlen(get_topic);
		break;
	case MQTT_CLOUD_SHADOW_TOPIC_UPDATE:
		tx_data_pub.topic.str = update_topic;
		tx_data_pub.topic.len = strlen(update_topic);
		break;
	case MQTT_CLOUD_SHADOW_TOPIC_DELETE:
		tx_data_pub.topic.str = delete_topic;
		tx_data_pub.topic.len = strlen(delete_topic);
		break;
#endif
	default:
		if (tx_data_pub.topic.str == NULL ||
		    tx_data_pub.topic.len == 0) {
			LOG_ERR("No application topic present in tx_data");
			return -ENODATA;
		}
		break;
	}

	struct mqtt_publish_param param;

	param.message.topic.qos		= tx_data_pub.qos;
	param.message.topic.topic.utf8	= tx_data_pub.topic.str;
	param.message.topic.topic.size	= tx_data_pub.topic.len;
	param.message.payload.data	= tx_data_pub.ptr;
	param.message.payload.len	= tx_data_pub.len;
	param.message_id		= k_cycle_get_32();
	param.dup_flag			= 0;
	param.retain_flag		= 0;

	LOG_DBG("Publishing to topic: %s",
		log_strdup(param.message.topic.topic.utf8));

	return mqtt_publish(&client, &param);
}

int mqtt_cloud_disconnect(void)
{
	atomic_set(&disconnect_requested, 1);
	return mqtt_disconnect(&client);
}

int mqtt_cloud_connect(struct mqtt_cloud_config *const config)
{
	int err;

	if (IS_ENABLED(CONFIG_MQTT_CLOUD_CONNECTION_POLL_THREAD)) {
		err = connection_poll_start();
	} else {
		atomic_set(&disconnect_requested, 0);

		err = client_broker_init(&client);
		if (err) {
			LOG_ERR("client_broker_init, error: %d", err);
			return err;
		}

		err = mqtt_connect(&client);
		if (err) {
			LOG_ERR("mqtt_connect, error: %d", err);
		}

		err = connect_error_translate(err);

#if defined(MQTT_CLOUD_TLS)
		config->socket = client.transport.tls.sock;
#else
		config->socket = client.transport.tcp.sock;
#endif
	}

	return err;
}

int mqtt_cloud_subscription_topics_add(
			const struct mqtt_cloud_topic_data *const topic_list,
			size_t list_count)
{
	if (list_count == 0) {
		LOG_ERR("Application subscription list is 0");
		return -EMSGSIZE;
	}

	if (list_count != CONFIG_MQTT_CLOUD_APP_SUBSCRIPTION_LIST_COUNT) {
		LOG_ERR("Application subscription list count mismatch");
		return -EMSGSIZE;
	}

	for (size_t i = 0; i < list_count; i++) {
		app_topic_data.list[i].topic.utf8 = topic_list[i].str;
		app_topic_data.list[i].topic.size = topic_list[i].len;
		app_topic_data.list[i].qos = MQTT_QOS_1_AT_LEAST_ONCE;
	}

	app_topic_data.list_count = list_count;

	return 0;
}

int mqtt_cloud_init(const struct mqtt_cloud_config *const config,
		 mqtt_cloud_evt_handler_t event_handler)
{
	int err;

	if (IS_ENABLED(CONFIG_MQTT_CLOUD_CLIENT_ID_APP) &&
	    config->client_id_len >= CONFIG_MQTT_CLOUD_CLIENT_ID_MAX_LEN) {
		LOG_ERR("Client ID string too long");
		return -EMSGSIZE;
	}

	if (IS_ENABLED(CONFIG_MQTT_CLOUD_CLIENT_ID_APP) &&
	    config->client_id == NULL) {
		LOG_ERR("Client ID not set in the application");
		return -ENODATA;
	}

	err = mqtt_cloud_topics_populate(config->client_id, config->client_id_len);
	if (err) {
		LOG_ERR("mqtt_topics_populate, error: %d", err);
		return err;
	}

#if defined(CONFIG_AWS_FOTA)
	err = aws_fota_init(&client, mqtt_fota_cb_handler);
	if (err) {
		LOG_ERR("aws_fota_init, error: %d", err);
		return err;
	}
#endif

	module_evt_handler = event_handler;

	return err;
}

#if defined(CONFIG_MQTT_CLOUD_CONNECTION_POLL_THREAD)
void mqtt_cloud_cloud_poll(void)
{
	int err;
	struct pollfd fds[1];
	struct mqtt_cloud_evt mqtt_cloud_evt = {
		.type = MQTT_CLOUD_EVT_DISCONNECTED,
		.data = { .err = MQTT_CLOUD_DISCONNECT_MISC}
	};

start:
	k_sem_take(&connection_poll_sem, K_FOREVER);
	atomic_set(&connection_poll_active, 1);

	mqtt_cloud_evt.data.err = MQTT_CLOUD_CONNECT_RES_SUCCESS;
	mqtt_cloud_evt.type = MQTT_CLOUD_EVT_CONNECTING;
	mqtt_cloud_notify_event(&mqtt_cloud_evt);

	err = client_broker_init(&client);
	if (err) {
		LOG_ERR("client_broker_init, error: %d", err);
	}

	err = mqtt_connect(&client);
	if (err) {
		LOG_ERR("mqtt_connect, error: %d", err);
	}

	err = connect_error_translate(err);

	if (err != MQTT_CLOUD_CONNECT_RES_SUCCESS) {
		mqtt_cloud_evt.data.err = err;
		mqtt_cloud_evt.type = MQTT_CLOUD_EVT_CONNECTING;
		mqtt_cloud_notify_event(&mqtt_cloud_evt);
		goto reset;
	} else {
		LOG_DBG("MQTT broker connection request sent.");
	}

	fds[0].fd = client.transport.tls.sock;
	fds[0].events = POLLIN;

	mqtt_cloud_evt.type = MQTT_CLOUD_EVT_DISCONNECTED;

	while (true) {
		err = poll(fds, ARRAY_SIZE(fds), MQTT_CLOUD_POLL_TIMEOUT_MS);

		if (err == 0) {
			if (mqtt_cloud_keepalive_time_left() <
			    MQTT_CLOUD_POLL_TIMEOUT_MS) {
				mqtt_cloud_ping();
			}
			continue;
		}

		if ((fds[0].revents & POLLIN) == POLLIN) {
			mqtt_cloud_input();
			continue;
		}

		if (err < 0) {
			LOG_ERR("poll() returned an error: %d", err);
			mqtt_cloud_evt.data.err = MQTT_CLOUD_DISCONNECT_MISC;
			break;
		}

		if (atomic_get(&disconnect_requested)) {
			atomic_set(&disconnect_requested, 0);
			LOG_DBG("Expected disconnect event.");
			mqtt_cloud_evt.data.err = MQTT_CLOUD_DISCONNECT_MISC;
			mqtt_cloud_notify_event(&mqtt_cloud_evt);
			goto reset;
		}

		if ((fds[0].revents & POLLNVAL) == POLLNVAL) {
			LOG_DBG("Socket error: POLLNVAL");
			LOG_DBG("The cloud socket was unexpectedly closed.");
			mqtt_cloud_evt.data.err =
					MQTT_CLOUD_DISCONNECT_INVALID_REQUEST;
			break;
		}

		if ((fds[0].revents & POLLHUP) == POLLHUP) {
			LOG_DBG("Socket error: POLLHUP");
			LOG_DBG("Connection was closed by the cloud.");
			mqtt_cloud_evt.data.err =
					MQTT_CLOUD_DISCONNECT_CLOSED_BY_REMOTE;
			break;
		}

		if ((fds[0].revents & POLLERR) == POLLERR) {
			LOG_DBG("Socket error: POLLERR");
			LOG_DBG("Cloud connection was unexpectedly closed.");
			mqtt_cloud_evt.data.err = MQTT_CLOUD_DISCONNECT_MISC;
			break;
		}
	}


	mqtt_cloud_notify_event(&mqtt_cloud_evt);
	mqtt_cloud_disconnect();

reset:
	atomic_set(&connection_poll_active, 0);
	k_sem_take(&connection_poll_sem, K_NO_WAIT);
	goto start;
}

#ifdef CONFIG_BOARD_QEMU_X86
#define POLL_THREAD_STACK_SIZE 4096
#else
#define POLL_THREAD_STACK_SIZE 2560
#endif
K_THREAD_DEFINE(connection_poll_thread, POLL_THREAD_STACK_SIZE,
		mqtt_cloud_cloud_poll, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
#endif /* defined(CONFIG_MQTT_CLOUD_CONNECTION_POLL_THREAD) */

#if defined(CONFIG_CLOUD_API)
static int api_init(const struct cloud_backend *const backend,
		    cloud_evt_handler_t handler)
{
	backend->config->handler = handler;
	mqtt_cloud_backend = (struct cloud_backend *)backend;

	struct mqtt_cloud_config config = {
		.client_id = backend->config->id,
		.client_id_len = backend->config->id_len
	};

	return mqtt_cloud_init(&config, NULL);
}

static int api_ep_subscriptions_add(const struct cloud_backend *const backend,
				    const struct cloud_endpoint *const list,
				    size_t list_count)
{
	struct mqtt_cloud_topic_data topic_list[list_count];

	for (size_t i = 0; i < list_count; i++) {
		topic_list[i].str = list[i].str;
		topic_list[i].len = list[i].len;
	}

	return mqtt_cloud_subscription_topics_add(topic_list, list_count);
}

static int api_connect(const struct cloud_backend *const backend)
{
	struct mqtt_cloud_config config = {
		.socket = backend->config->socket
	};

	return mqtt_cloud_connect(&config);
}

static int api_disconnect(const struct cloud_backend *const backend)
{
	return mqtt_cloud_disconnect();
}

static int api_send(const struct cloud_backend *const backend,
		    const struct cloud_msg *const msg)
{
	struct mqtt_cloud_data tx_data = {
		.ptr = msg->buf,
		.len = msg->len,
		.qos = msg->qos,
		.topic.str = msg->endpoint.str,
		.topic.len = msg->endpoint.len,
		.topic.type = msg->endpoint.type
	};

	return mqtt_cloud_send(&tx_data);
}

static int api_input(const struct cloud_backend *const backend)
{
	return mqtt_cloud_input();
}

static int api_ping(const struct cloud_backend *const backend)
{
	return mqtt_cloud_ping();
}

static int api_keepalive_time_left(const struct cloud_backend *const backend)
{
	return mqtt_cloud_keepalive_time_left();
}

static const struct cloud_api mqtt_cloud_api = {
	.init			= api_init,
	.ep_subscriptions_add	= api_ep_subscriptions_add,
	.connect		= api_connect,
	.disconnect		= api_disconnect,
	.send			= api_send,
	.input			= api_input,
	.ping			= api_ping,
	.keepalive_time_left	= api_keepalive_time_left
};

CLOUD_BACKEND_DEFINE(MQTT_CLOUD, mqtt_cloud_api);
#endif /* defined(CONFIG_CLOUD_API) */
