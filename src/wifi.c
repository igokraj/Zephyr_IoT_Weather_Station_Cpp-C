#include <zephyr/logging/log.h>
#include <zephyr/net/wifi_mgmt.h>
#include <string.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>

/* User .h files */
#include "secret.h"

/*
 NOTE
 Parts marked with CONSTANT are standard Zephyr Wi-Fi station setup.
 They stay the same in every project like this one.
 Lines marked with CHANGE are the ones to adjust, the comment says when.
*/

LOG_MODULE_REGISTER(WiFi, LOG_LEVEL_DBG);

/* Signalled by the callback when the router accepts or rejects us */
/* *** CONSTANT *** */
K_SEM_DEFINE(wifi_sem, 0, 1);
static int wifi_status;
#define WIFI_TIMEOUT K_SECONDS(15) /* CHANGE: longer if it often times out (weak signal), shorter to save battery */

/* Signalled when DHCP gives us an IP address */
/* *** CONSTANT *** */
K_SEM_DEFINE(ipv4_sem, 0, 1);
static struct net_mgmt_event_callback ipv4_mgmt_cb;

/* Called by Zephyr when the interface gets an IPv4 address from DHCP */
/* *** CONSTANT *** */
static void ipv4_event_handler(struct net_mgmt_event_callback *cb,
uint64_t mgmt_event, struct net_if *iface)
{
    if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
        LOG_INF("IPv4 address assigned");
        k_sem_give(&ipv4_sem);
    }
}

/* Save the result of the connection attempt and wake up wifi_connect() */
/* *** CONSTANT *** */
static struct net_mgmt_event_callback wifi_shell_mgmt_cb;
static void handle_wifi_connect_result(struct net_mgmt_event_callback *cb)
{
const struct wifi_status *status = (const struct wifi_status *)
                       cb->info;
    wifi_status = status->status;

    if (wifi_status) {
        LOG_ERR("Connection request failed (%d)", wifi_status);
    }
    else {
        LOG_INF("WiFI connected");
    }
    k_sem_give(&wifi_sem);
    }

/* Called by Zephyr for Wi-Fi events, passes the connect result on */
/* *** CONSTANT *** */
static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
uint64_t mgmt_event, struct net_if *iface)
{
switch (mgmt_event) {
case NET_EVENT_WIFI_CONNECT_RESULT:
handle_wifi_connect_result(cb);
break;
default:
break;
    }
}

/*
 Connect to the router and wait until DHCP gives us an IP address.
 Returns 0 only when the device is ready to talk to the internet.
*/
int wifi_connect(void)
{
    /* Register the callbacks before connecting, so no event is missed */
    net_mgmt_init_event_callback(&wifi_shell_mgmt_cb, wifi_mgmt_event_handler,
                                 NET_EVENT_WIFI_CONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi_shell_mgmt_cb);

    net_mgmt_init_event_callback(&ipv4_mgmt_cb, ipv4_event_handler,
                                 NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&ipv4_mgmt_cb);


    struct net_if *iface = net_if_get_default();
    static struct wifi_connect_req_params cnx_params = {
        .ssid = WIFI_SSID,
        .psk = WIFI_PSK,
        /* channel = 0 -> scan all the channels, it is possible to
           speed up the connection by specifying the router's 2.4 GHz channel
           (1 in my case) */
        .channel = 0, /* CHANGE: set the router's channel */
        .security = WIFI_SECURITY_TYPE_PSK, /* CHANGE: WIFI_SECURITY_TYPE_SAE for a WPA3-only router (needs CONFIG_ESP32_WIFI_ENABLE_WPA3_SAE=y) */
    };
    cnx_params.ssid_length = strlen(WIFI_SSID);
    cnx_params.psk_length  = strlen(WIFI_PSK);

    if (net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &cnx_params, sizeof(cnx_params))) {
        LOG_ERR("WiFi: connection request denied");
        return -1;
    }

    /* The request only starts connecting, the result comes later in the callback */
    if (k_sem_take(&wifi_sem, WIFI_TIMEOUT) != 0) {
        LOG_ERR("WiFi: timeout");
        return -1;
    }

    if (wifi_status != 0) {
        return -1;
    }

    /* Connected to the router, now wait for DHCP to give us an IP address */
    if (k_sem_take(&ipv4_sem, WIFI_TIMEOUT) != 0) {
        LOG_ERR("WiFi: no IP address from DHCP");
        return -1;
    }
    return 0;
}