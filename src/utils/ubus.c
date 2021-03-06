#include <dirent.h>
#include <libubus.h>

#include "networksocket.h"
#include "tcpsocket.h"
#include "dawn_uci.h"
#include "dawn_iwinfo.h"
#include "datastorage.h"
#include "ubus.h"
#include "msghandler.h"


#define REQ_TYPE_PROBE 0
#define REQ_TYPE_AUTH 1
#define REQ_TYPE_ASSOC 2


static struct ubus_context *ctx = NULL;

static struct blob_buf b;
static struct blob_buf b_send_network;
static struct blob_buf b_probe;
static struct blob_buf b_domain;
static struct blob_buf b_notify;
static struct blob_buf b_clients;
static struct blob_buf b_umdns;
static struct blob_buf b_beacon;
static struct blob_buf b_nr;

void update_clients(struct uloop_timeout *t);

void update_tcp_connections(struct uloop_timeout *t);

void update_channel_utilization(struct uloop_timeout *t);

void run_server_update(struct uloop_timeout *t);

void update_beacon_reports(struct uloop_timeout *t);

struct uloop_timeout client_timer = {
        .cb = update_clients
};
struct uloop_timeout hostapd_timer = {
        .cb = update_hostapd_sockets
};
struct uloop_timeout umdns_timer = {
        .cb = update_tcp_connections
};
struct uloop_timeout channel_utilization_timer = {
        .cb = update_channel_utilization
};

void remove_ap_array_cb(struct uloop_timeout* t);

void denied_req_array_cb(struct uloop_timeout* t);

void remove_client_array_cb(struct uloop_timeout* t);

void remove_probe_array_cb(struct uloop_timeout* t);

struct uloop_timeout probe_timeout = {
        .cb = remove_probe_array_cb
};

struct uloop_timeout client_timeout = {
        .cb = remove_client_array_cb
};

struct uloop_timeout ap_timeout = {
        .cb = remove_ap_array_cb
};

struct uloop_timeout denied_req_timeout = {
        .cb = denied_req_array_cb
};

// TODO: Never scheduled?
struct uloop_timeout usock_timer = {
        .cb = run_server_update
};

struct uloop_timeout beacon_reports_timer = {
        .cb = update_beacon_reports
};

#define MAX_HOSTAPD_SOCKETS 10

LIST_HEAD(hostapd_sock_list);

struct hostapd_sock_entry {
    struct list_head list;

    uint32_t id;
    char iface_name[MAX_INTERFACE_NAME];
    char hostname[HOST_NAME_MAX];
    uint8_t bssid_addr[ETH_ALEN];
    char ssid[SSID_MAX_LEN];
    uint8_t ht_support;
    uint8_t vht_support;
    uint64_t last_channel_time;
    uint64_t last_channel_time_busy;
    int chan_util_samples_sum;
    int chan_util_num_sample_periods;
    int chan_util_average; //TODO: Never evaluated?

    // add neighbor report string
    /*
    [Elemen ID|1][LENGTH|1][BSSID|6][BSSID INFORMATION|4][Operating Class|1][Channel Number|1][PHY Type|1][Operational Subelements]
    */
    char neighbor_report[NEIGHBOR_REPORT_LEN];

    struct ubus_subscriber subscriber;
    struct ubus_event_handler wait_handler;
    bool subscribed;
};

struct hostapd_sock_entry* hostapd_sock_arr[MAX_HOSTAPD_SOCKETS];
int hostapd_sock_last = -1;

enum {
    AUTH_BSSID_ADDR,
    AUTH_CLIENT_ADDR,
    AUTH_TARGET_ADDR,
    AUTH_SIGNAL,
    AUTH_FREQ,
    __AUTH_MAX,
};

static const struct blobmsg_policy auth_policy[__AUTH_MAX] = {
        [AUTH_BSSID_ADDR] = {.name = "bssid", .type = BLOBMSG_TYPE_STRING},
        [AUTH_CLIENT_ADDR] = {.name = "address", .type = BLOBMSG_TYPE_STRING},
        [AUTH_TARGET_ADDR] = {.name = "target", .type = BLOBMSG_TYPE_STRING},
        [AUTH_SIGNAL] = {.name = "signal", .type = BLOBMSG_TYPE_INT32},
        [AUTH_FREQ] = {.name = "freq", .type = BLOBMSG_TYPE_INT32},
};

enum {
    BEACON_REP_ADDR,
    BEACON_REP_OP_CLASS,
    BEACON_REP_CHANNEL,
    BEACON_REP_START_TIME,
    BEACON_REP_DURATION,
    BEACON_REP_REPORT_INFO,
    BEACON_REP_RCPI,
    BEACON_REP_RSNI,
    BEACON_REP_BSSID,
    BEACON_REP_ANTENNA_ID,
    BEACON_REP_PARENT_TSF,
    __BEACON_REP_MAX,
};

static const struct blobmsg_policy beacon_rep_policy[__BEACON_REP_MAX] = {
        [BEACON_REP_ADDR] = {.name = "address", .type = BLOBMSG_TYPE_STRING},
        [BEACON_REP_OP_CLASS] = {.name = "op-class", .type = BLOBMSG_TYPE_INT16},
        [BEACON_REP_CHANNEL] = {.name = "channel", .type = BLOBMSG_TYPE_INT64},
        [BEACON_REP_START_TIME] = {.name = "start-time", .type = BLOBMSG_TYPE_INT32},
        [BEACON_REP_DURATION] = {.name = "duration", .type = BLOBMSG_TYPE_INT16},
        [BEACON_REP_REPORT_INFO] = {.name = "report-info", .type = BLOBMSG_TYPE_INT16},
        [BEACON_REP_RCPI] = {.name = "rcpi", .type = BLOBMSG_TYPE_INT16},
        [BEACON_REP_RSNI] = {.name = "rsni", .type = BLOBMSG_TYPE_INT16},
        [BEACON_REP_BSSID] = {.name = "bssid", .type = BLOBMSG_TYPE_STRING},
        [BEACON_REP_ANTENNA_ID] = {.name = "antenna-id", .type = BLOBMSG_TYPE_INT16},
        [BEACON_REP_PARENT_TSF] = {.name = "parent-tsf", .type = BLOBMSG_TYPE_INT16},
};

enum {
    DAWN_UMDNS_TABLE,
    __DAWN_UMDNS_TABLE_MAX,
};

static const struct blobmsg_policy dawn_umdns_table_policy[__DAWN_UMDNS_TABLE_MAX] = {
        [DAWN_UMDNS_TABLE] = {.name = "_dawn._tcp", .type = BLOBMSG_TYPE_TABLE},
};

enum {
    DAWN_UMDNS_IPV4,
    DAWN_UMDNS_PORT,
    __DAWN_UMDNS_MAX,
};

static const struct blobmsg_policy dawn_umdns_policy[__DAWN_UMDNS_MAX] = {
        [DAWN_UMDNS_IPV4] = {.name = "ipv4", .type = BLOBMSG_TYPE_STRING},
        [DAWN_UMDNS_PORT] = {.name = "port", .type = BLOBMSG_TYPE_INT32},
};

enum {
    RRM_ARRAY,
    __RRM_MAX,
};

static const struct blobmsg_policy rrm_array_policy[__RRM_MAX] = {
        [RRM_ARRAY] = {.name = "value", .type = BLOBMSG_TYPE_ARRAY},
};

/* Function Definitions */
static int hostapd_notify(struct ubus_context *ctx, struct ubus_object *obj,
                          struct ubus_request_data *req, const char *method,
                          struct blob_attr *msg);

static int ubus_get_clients();

static int
add_mac(struct ubus_context *ctx, struct ubus_object *obj,
        struct ubus_request_data *req, const char *method,
        struct blob_attr *msg);

static int reload_config(struct ubus_context *ctx, struct ubus_object *obj,
                         struct ubus_request_data *req, const char *method,
                         struct blob_attr *msg);

static int get_hearing_map(struct ubus_context *ctx, struct ubus_object *obj,
                           struct ubus_request_data *req, const char *method,
                           struct blob_attr *msg);

static int get_network(struct ubus_context *ctx, struct ubus_object *obj,
                       struct ubus_request_data *req, const char *method,
                       struct blob_attr *msg);

static void ubus_add_oject();

static void respond_to_notify(uint32_t id);

//static int handle_uci_config(struct blob_attr *msg);

void subscribe_to_new_interfaces(const char *hostapd_sock_path);

bool subscriber_to_interface(const char *ifname);

bool subscribe(struct hostapd_sock_entry *hostapd_entry);

int parse_to_beacon_rep(struct blob_attr *msg, probe_entry *beacon_rep);

void ubus_set_nr();

void add_client_update_timer(time_t time) {
    uloop_timeout_set(&client_timer, time);
}

static inline int
subscription_wait(struct ubus_event_handler *handler) {
    return ubus_register_event_handler(ctx, handler, "ubus.object.add");
}

void blobmsg_add_macaddr(struct blob_buf *buf, const char *name, const uint8_t *addr) {
    char *s;

    s = blobmsg_alloc_string_buffer(buf, name, 20);
    sprintf(s, MACSTR, MAC2STR(addr));
    blobmsg_add_string_buffer(buf);
}

static int decide_function(probe_entry *prob_req, int req_type) {
    if (mac_in_maclist(prob_req->client_addr)) {
        return 1;
    }

    if (prob_req->counter < dawn_metric.min_probe_count) {
        return 0;
    }

    if (req_type == REQ_TYPE_PROBE && dawn_metric.eval_probe_req <= 0) {
        return 1;
    }

    if (req_type == REQ_TYPE_AUTH && dawn_metric.eval_auth_req <= 0) {
        return 1;
    }

    if (req_type == REQ_TYPE_ASSOC && dawn_metric.eval_assoc_req <= 0) {
        return 1;
    }

    if (better_ap_available(prob_req->bssid_addr, prob_req->client_addr, NULL, 0)) {
        return 0;
    }

    return 1;
}

int parse_to_auth_req(struct blob_attr *msg, auth_entry *auth_req) {
    struct blob_attr *tb[__AUTH_MAX];

    blobmsg_parse(auth_policy, __AUTH_MAX, tb, blob_data(msg), blob_len(msg));

    if (hwaddr_aton(blobmsg_data(tb[AUTH_BSSID_ADDR]), auth_req->bssid_addr))
        return UBUS_STATUS_INVALID_ARGUMENT;

    if (hwaddr_aton(blobmsg_data(tb[AUTH_CLIENT_ADDR]), auth_req->client_addr))
        return UBUS_STATUS_INVALID_ARGUMENT;

    if (hwaddr_aton(blobmsg_data(tb[AUTH_TARGET_ADDR]), auth_req->target_addr))
        return UBUS_STATUS_INVALID_ARGUMENT;

    if (tb[AUTH_SIGNAL]) {
        auth_req->signal = blobmsg_get_u32(tb[AUTH_SIGNAL]);
    }

    if (tb[AUTH_FREQ]) {
        auth_req->freq = blobmsg_get_u32(tb[AUTH_FREQ]);
    }

    return 0;
}

int parse_to_assoc_req(struct blob_attr *msg, assoc_entry *assoc_req) {
    return (parse_to_auth_req(msg, assoc_req));
}

int parse_to_beacon_rep(struct blob_attr *msg, probe_entry *beacon_rep) {
    struct blob_attr *tb[__BEACON_REP_MAX];

    blobmsg_parse(beacon_rep_policy, __BEACON_REP_MAX, tb, blob_data(msg), blob_len(msg));

    if(!tb[BEACON_REP_BSSID] || !tb[BEACON_REP_ADDR])
    {
        return -1;
    }

    if (hwaddr_aton(blobmsg_data(tb[BEACON_REP_BSSID]), beacon_rep->bssid_addr))
        return UBUS_STATUS_INVALID_ARGUMENT;

    ap check_null = {.bssid_addr = {0, 0, 0, 0, 0, 0}};
    if(mac_is_equal(check_null.bssid_addr,beacon_rep->bssid_addr))
    {
        fprintf(stderr, "Received NULL MAC! Client is strange!\n");
        return -1;
    }

    ap ap_entry_rep = ap_array_get_ap(beacon_rep->bssid_addr);

    // no client from network!!
    if (!mac_is_equal(ap_entry_rep.bssid_addr, beacon_rep->bssid_addr)) {
        return -1; //TODO: Check this
    }

    if (hwaddr_aton(blobmsg_data(tb[BEACON_REP_ADDR]), beacon_rep->client_addr))
        return UBUS_STATUS_INVALID_ARGUMENT;

    int rcpi = 0;
    int rsni = 0;
    rcpi = blobmsg_get_u16(tb[BEACON_REP_RCPI]);
    rsni = blobmsg_get_u16(tb[BEACON_REP_RSNI]);


    // HACKY WORKAROUND!
    printf("Try update RCPI and RSNI for beacon report!\n");
    if(!probe_array_update_rcpi_rsni(beacon_rep->bssid_addr, beacon_rep->client_addr, rcpi, rsni, true))
    {
        printf("Beacon: No Probe Entry Existing!\n");
        beacon_rep->counter = dawn_metric.min_probe_count;
        hwaddr_aton(blobmsg_data(tb[BEACON_REP_ADDR]), beacon_rep->target_addr);  // TODO: Should this be ->bssid_addr?
        beacon_rep->signal = 0;
        beacon_rep->freq = ap_entry_rep.freq;
        beacon_rep->rcpi = rcpi;
        beacon_rep->rsni = rsni;

        beacon_rep->ht_capabilities = false; // that is very problematic!!!
        beacon_rep->vht_capabilities = false; // that is very problematic!!!
        printf("Inserting to array!\n");
        beacon_rep->time = time(0);
        insert_to_array(*beacon_rep, false, false, true);
        ubus_send_probe_via_network(*beacon_rep);
    }
    return 0;
}

int handle_auth_req(struct blob_attr* msg) {

    print_probe_array();
    auth_entry auth_req;
    parse_to_auth_req(msg, &auth_req);

    printf("Auth entry: ");
    print_auth_entry(auth_req);

    if (mac_in_maclist(auth_req.client_addr)) {
        return WLAN_STATUS_SUCCESS;
    }

    probe_entry tmp = probe_array_get_entry(auth_req.bssid_addr, auth_req.client_addr);

    printf("Entry found\n");
    print_probe_entry(tmp);

    // block if entry was not already found in probe database
    if (!(mac_is_equal(tmp.bssid_addr, auth_req.bssid_addr) && mac_is_equal(tmp.client_addr, auth_req.client_addr))) {
        printf("Deny authentication!\n");

        if (dawn_metric.use_driver_recog) {
            auth_req.time = time(0);
            insert_to_denied_req_array(auth_req, 1);
        }
        return dawn_metric.deny_auth_reason;
    }

    if (!decide_function(&tmp, REQ_TYPE_AUTH)) {
        printf("Deny authentication\n");
        if (dawn_metric.use_driver_recog) {
            auth_req.time = time(0);
            insert_to_denied_req_array(auth_req, 1);
        }
        return dawn_metric.deny_auth_reason;
    }

    // maybe send here that the client is connected?
    printf("Allow authentication!\n");
    return WLAN_STATUS_SUCCESS;
}

static int handle_assoc_req(struct blob_attr *msg) {

    print_probe_array();
    auth_entry auth_req;
    parse_to_assoc_req(msg, &auth_req);
    printf("Association entry: ");
    print_auth_entry(auth_req);

    if (mac_in_maclist(auth_req.client_addr)) {
        return WLAN_STATUS_SUCCESS;
    }

    probe_entry tmp = probe_array_get_entry(auth_req.bssid_addr, auth_req.client_addr);

    printf("Entry found\n");
    print_probe_entry(tmp);

    // block if entry was not already found in probe database
    if (!(mac_is_equal(tmp.bssid_addr, auth_req.bssid_addr) && mac_is_equal(tmp.client_addr, auth_req.client_addr))) {
        printf("Deny associtation!\n");
        if (dawn_metric.use_driver_recog) {
            auth_req.time = time(0);
            insert_to_denied_req_array(auth_req, 1);
        }
        return dawn_metric.deny_assoc_reason;
    }

    if (!decide_function(&tmp, REQ_TYPE_ASSOC)) {
        printf("Deny association\n");
        if (dawn_metric.use_driver_recog) {
            auth_req.time = time(0);
            insert_to_denied_req_array(auth_req, 1);
        }
        return dawn_metric.deny_assoc_reason;
    }

    printf("Allow association!\n");
    return WLAN_STATUS_SUCCESS;
}

static int handle_probe_req(struct blob_attr *msg) {
    probe_entry prob_req;
    probe_entry tmp_prob_req;

    if (parse_to_probe_req(msg, &prob_req) == 0) {
        prob_req.time = time(0);
        tmp_prob_req = insert_to_array(prob_req, 1, true, false); // TODO: Chnage 1 to true?
        ubus_send_probe_via_network(tmp_prob_req);
        //send_blob_attr_via_network(msg, "probe");
    }

    if (!decide_function(&tmp_prob_req, REQ_TYPE_PROBE)) {
        return WLAN_STATUS_AP_UNABLE_TO_HANDLE_NEW_STA; // no reason needed...
    }
    return WLAN_STATUS_SUCCESS;
}

static int handle_beacon_rep(struct blob_attr *msg) {
    probe_entry beacon_rep;

    if (parse_to_beacon_rep(msg, &beacon_rep) == 0) {
        printf("Inserting beacon Report!\n");
        // insert_to_array(beacon_rep, 1);
        printf("Sending via network!\n");
        // send_blob_attr_via_network(msg, "beacon-report");
    }
    return 0;
}


int send_blob_attr_via_network(struct blob_attr *msg, char *method) {

    if (!msg) {
        return -1;
    }

    char *data_str;
    char *str;
    data_str = blobmsg_format_json(msg, true);
    blob_buf_init(&b_send_network, 0);
    blobmsg_add_string(&b_send_network, "method", method);
    blobmsg_add_string(&b_send_network, "data", data_str);

    str = blobmsg_format_json(b_send_network.head, true);

    if (network_config.network_option == 2) {
        send_tcp(str);
    } else {
        if (network_config.use_symm_enc) {
            send_string_enc(str);
        } else {
            send_string(str);
        }
    }

    free(data_str);
    free(str);

    return 0;
}

static int hostapd_notify(struct ubus_context *ctx, struct ubus_object *obj,
                          struct ubus_request_data *req, const char *method,
                          struct blob_attr *msg) {
    char *str;
    str = blobmsg_format_json(msg, true);
    printf("Method new: %s : %s\n", method, str);
    free(str);

    struct hostapd_sock_entry *entry;
    struct ubus_subscriber *subscriber;

    subscriber = container_of(obj, struct ubus_subscriber, obj);
    entry = container_of(subscriber, struct hostapd_sock_entry, subscriber);

    struct blob_attr *cur; int rem;
    blob_buf_init(&b_notify, 0);
    blobmsg_for_each_attr(cur, msg, rem){
        blobmsg_add_blob(&b_notify, cur);
    }

    blobmsg_add_macaddr(&b_notify, "bssid", entry->bssid_addr);
    blobmsg_add_string(&b_notify, "ssid", entry->ssid);

    if (strncmp(method, "probe", 5) == 0) {
        return handle_probe_req(b_notify.head);
    } else if (strncmp(method, "auth", 4) == 0) {
        return handle_auth_req(b_notify.head);
    } else if (strncmp(method, "assoc", 5) == 0) {
        return handle_assoc_req(b_notify.head);
    } else if (strncmp(method, "deauth", 6) == 0) {
        send_blob_attr_via_network(b_notify.head, "deauth");
        return handle_deauth_req(b_notify.head);
    } else if (strncmp(method, "beacon-report", 12) == 0) {
        return handle_beacon_rep(b_notify.head);
    }
    return 0;
}

int dawn_init_ubus(const char *ubus_socket, const char *hostapd_dir) {
    uloop_init();
    signal(SIGPIPE, SIG_IGN);

    ctx = ubus_connect(ubus_socket);
    if (!ctx) {
        fprintf(stderr, "Failed to connect to ubus\n");
        return -1;
    } else {
        printf("Connected to ubus\n");
    }

    ubus_add_uloop(ctx);

    // set dawn metric
    dawn_metric = uci_get_dawn_metric();

    uloop_timeout_add(&hostapd_timer);  // callback = update_hostapd_sockets

    // set up callbacks to remove aged data
    uloop_add_data_cbs();

    // get clients
    uloop_timeout_add(&client_timer);  // callback = update_clients

    uloop_timeout_add(&channel_utilization_timer);  // callback = update_channel_utilization

    // request beacon reports
    if(timeout_config.update_beacon_reports) // allow setting timeout to 0
        uloop_timeout_add(&beacon_reports_timer); // callback = update_beacon_reports

    ubus_add_oject();

    if (network_config.network_option == 2)
    {
        start_umdns_update();
        if(run_server(network_config.tcp_port))
            uloop_timeout_set(&usock_timer, 1 * 1000);
    }

    subscribe_to_new_interfaces(hostapd_dir_glob);

    uloop_run();

    close_socket();

    ubus_free(ctx);
    uloop_done();
    return 0;
}

static void ubus_get_clients_cb(struct ubus_request *req, int type, struct blob_attr *msg) {
    struct hostapd_sock_entry *sub, *entry = NULL;

    if (!msg)
        return;

    char *data_str = blobmsg_format_json(msg, 1);
    blob_buf_init(&b_domain, 0);
    blobmsg_add_json_from_string(&b_domain, data_str);
    blobmsg_add_u32(&b_domain, "collision_domain", network_config.collision_domain);
    blobmsg_add_u32(&b_domain, "bandwidth", network_config.bandwidth);

    list_for_each_entry(sub, &hostapd_sock_list, list)
    {
        if (sub->id == req->peer) {
            entry = sub;
        }
    }

    if (entry == NULL) {
        fprintf(stderr, "Failed to find interface!\n");
        free(data_str);
        return;
    }

    if (!entry->subscribed) {
        fprintf(stderr, "Interface %s is not subscribed!\n", entry->iface_name);
        free(data_str);
        return;
    }

    blobmsg_add_macaddr(&b_domain, "bssid", entry->bssid_addr);
    blobmsg_add_string(&b_domain, "ssid", entry->ssid);
    blobmsg_add_u8(&b_domain, "ht_supported", entry->ht_support);
    blobmsg_add_u8(&b_domain, "vht_supported", entry->vht_support);

    blobmsg_add_u32(&b_domain, "ap_weight", dawn_metric.ap_weight);

    //int channel_util = get_channel_utilization(entry->iface_name, &entry->last_channel_time, &entry->last_channel_time_busy);
    blobmsg_add_u32(&b_domain, "channel_utilization", entry->chan_util_average);

    blobmsg_add_string(&b_domain, "neighbor_report", entry->neighbor_report);

    blobmsg_add_string(&b_domain, "iface", entry->iface_name);
    blobmsg_add_string(&b_domain, "hostname", entry->hostname);

    send_blob_attr_via_network(b_domain.head, "clients");
    // TODO: Have we just bit-packed data to send to something locally to unpack it again?  Performance / scalability?
    parse_to_clients(b_domain.head, 1, req->peer);

    print_client_array();
    print_ap_array();

    free(data_str);
}

static int ubus_get_clients() {
    int timeout = 1;
    struct hostapd_sock_entry *sub;
    list_for_each_entry(sub, &hostapd_sock_list, list)
    {
        if (sub->subscribed) {
            blob_buf_init(&b_clients, 0);
            ubus_invoke(ctx, sub->id, "get_clients", b_clients.head, ubus_get_clients_cb, NULL, timeout * 1000);
        }
    }
    return 0;
}

static void ubus_get_rrm_cb(struct ubus_request *req, int type, struct blob_attr *msg) {
    struct hostapd_sock_entry *sub, *entry = NULL;
    struct blob_attr *tb[__RRM_MAX];

    if (!msg)
        return;

    list_for_each_entry(sub, &hostapd_sock_list, list)
    {
        if (sub->id == req->peer) {
            entry = sub;
        }
    }

    blobmsg_parse(rrm_array_policy, __RRM_MAX, tb, blob_data(msg), blob_len(msg));

    if (!tb[RRM_ARRAY]) {
        return;
    }
    struct blob_attr *attr;
    //struct blobmsg_hdr *hdr;
    int len = blobmsg_data_len(tb[RRM_ARRAY]);
    int i = 0;

     __blob_for_each_attr(attr, blobmsg_data(tb[RRM_ARRAY]), len)
     {
         if(i==2)
         {
            char* neighborreport = blobmsg_get_string(blobmsg_data(attr));
            strcpy(entry->neighbor_report,neighborreport);
            printf("Copied Neighborreport: %s,\n", entry->neighbor_report);
         }
         i++;
     }
}

static int ubus_get_rrm() {
    int timeout = 1;
    struct hostapd_sock_entry *sub;
    list_for_each_entry(sub, &hostapd_sock_list, list)
    {
        if (sub->subscribed) {
            blob_buf_init(&b, 0);
            ubus_invoke(ctx, sub->id, "rrm_nr_get_own", b.head, ubus_get_rrm_cb, NULL, timeout * 1000);
        }
    }
    return 0;
}

void update_clients(struct uloop_timeout *t) {
    ubus_get_clients();
    if(dawn_metric.set_hostapd_nr)
        ubus_set_nr();
    // maybe to much?! don't set timer again...
    uloop_timeout_set(&client_timer, timeout_config.update_client * 1000);
}

void run_server_update(struct uloop_timeout *t) {
    if(run_server(network_config.tcp_port))
        uloop_timeout_set(&usock_timer, 1 * 1000);
}

void update_channel_utilization(struct uloop_timeout *t) {
    struct hostapd_sock_entry *sub;

    list_for_each_entry(sub, &hostapd_sock_list, list)
    {

        if (sub->subscribed) {
            sub->chan_util_samples_sum += get_channel_utilization(sub->iface_name, &sub->last_channel_time,
                                                                  &sub->last_channel_time_busy);
            sub->chan_util_num_sample_periods++;

            if (sub->chan_util_num_sample_periods > dawn_metric.chan_util_avg_period) {
                sub->chan_util_average = sub->chan_util_samples_sum / sub->chan_util_num_sample_periods;
                sub->chan_util_samples_sum = 0;
                sub->chan_util_num_sample_periods = 0;
            }
        }
    }
    uloop_timeout_set(&channel_utilization_timer, timeout_config.update_chan_util * 1000);
}

void ubus_send_beacon_report(uint8_t client[], int id)
{
    printf("Crafting Beacon Report\n");
    int timeout = 1;
    blob_buf_init(&b_beacon, 0);
    blobmsg_add_macaddr(&b_beacon, "addr", client);
    blobmsg_add_u32(&b_beacon, "op_class", dawn_metric.op_class);
    blobmsg_add_u32(&b_beacon, "channel", dawn_metric.scan_channel);
    blobmsg_add_u32(&b_beacon, "duration", dawn_metric.duration);
    blobmsg_add_u32(&b_beacon, "mode", dawn_metric.mode);
    printf("Adding string\n");
    blobmsg_add_string(&b_beacon, "ssid", "");

    printf("Invoking beacon report!\n");
    ubus_invoke(ctx, id, "rrm_beacon_req", b_beacon.head, NULL, NULL, timeout * 1000);
}

void update_beacon_reports(struct uloop_timeout *t) {
    if(!timeout_config.update_beacon_reports) // if 0 just return
    {
        return;
    }
    printf("Sending beacon report!\n");
    struct hostapd_sock_entry *sub;
    list_for_each_entry(sub, &hostapd_sock_list, list)
    {
        if (sub->subscribed) {
            printf("Sending beacon report Sub!\n");
            send_beacon_reports(sub->bssid_addr, sub->id);
        }
    }
    uloop_timeout_set(&beacon_reports_timer, timeout_config.update_beacon_reports * 1000);
}

void update_tcp_connections(struct uloop_timeout *t) {
    ubus_call_umdns();
    uloop_timeout_set(&umdns_timer, timeout_config.update_tcp_con * 1000);
}

void start_umdns_update() {
    // update connections
    uloop_timeout_add(&umdns_timer); // callback = update_tcp_connections
}

void update_hostapd_sockets(struct uloop_timeout *t) {
    subscribe_to_new_interfaces(hostapd_dir_glob);
    uloop_timeout_set(&hostapd_timer, timeout_config.update_hostapd * 1000);
}

void ubus_set_nr(){
    struct hostapd_sock_entry *sub;


    list_for_each_entry(sub, &hostapd_sock_list, list)
    {
        if (sub->subscribed) {
            int timeout = 1;
            blob_buf_init(&b_nr, 0);
            ap_get_nr(&b_nr, sub->bssid_addr);
            ubus_invoke(ctx, sub->id, "rrm_nr_set", b_nr.head, NULL, NULL, timeout * 1000);
        }
    }
}

void del_client_all_interfaces(const uint8_t *client_addr, uint32_t reason, uint8_t deauth, uint32_t ban_time) {
    struct hostapd_sock_entry *sub;

    blob_buf_init(&b, 0);
    blobmsg_add_macaddr(&b, "addr", client_addr);
    blobmsg_add_u32(&b, "reason", reason);
    blobmsg_add_u8(&b, "deauth", deauth);
    blobmsg_add_u32(&b, "ban_time", ban_time);

    list_for_each_entry(sub, &hostapd_sock_list, list)
    {
        if (sub->subscribed) {
            int timeout = 1;
            ubus_invoke(ctx, sub->id, "del_client", b.head, NULL, NULL, timeout * 1000);
        }
    }
}

void del_client_interface(uint32_t id, const uint8_t *client_addr, uint32_t reason, uint8_t deauth, uint32_t ban_time) {
    struct hostapd_sock_entry *sub;

    blob_buf_init(&b, 0);
    blobmsg_add_macaddr(&b, "addr", client_addr);
    blobmsg_add_u32(&b, "reason", reason);
    blobmsg_add_u8(&b, "deauth", deauth);
    blobmsg_add_u32(&b, "ban_time", ban_time);


    list_for_each_entry(sub, &hostapd_sock_list, list)
    {
        if (sub->subscribed) {
            int timeout = 1;
            ubus_invoke(ctx, id, "del_client", b.head, NULL, NULL, timeout * 1000);
        }
    }

}

int wnm_disassoc_imminent(uint32_t id, const uint8_t *client_addr, char *dest_ap, uint32_t duration) {
    struct hostapd_sock_entry *sub;

    blob_buf_init(&b, 0);
    blobmsg_add_macaddr(&b, "addr", client_addr);
    blobmsg_add_u32(&b, "duration", duration);
    blobmsg_add_u8(&b, "abridged", 1); // prefer aps in neighborlist

    // ToDo: maybe exchange to a list of aps
    void* nbs = blobmsg_open_array(&b, "neighbors");
    if (dest_ap != NULL)
    {
        blobmsg_add_string(&b, NULL, dest_ap);
        printf("BSS TRANSITION TO %s\n", dest_ap);
    }

    blobmsg_close_array(&b, nbs);
    list_for_each_entry(sub, &hostapd_sock_list, list)
    {
        if (sub->subscribed) {
            int timeout = 1; //TDO: Maybe ID is wrong?! OR CHECK HERE ID
            ubus_invoke(ctx, id, "wnm_disassoc_imminent", b.head, NULL, NULL, timeout * 1000);
        }
    }

    return 0;
}

static void ubus_umdns_cb(struct ubus_request *req, int type, struct blob_attr *msg) {
    struct blob_attr *tb[__DAWN_UMDNS_TABLE_MAX];

    if (!msg)
        return;

    blobmsg_parse(dawn_umdns_table_policy, __DAWN_UMDNS_MAX, tb, blob_data(msg), blob_len(msg));

    if (!tb[DAWN_UMDNS_TABLE]) {
        return;
    }

    struct blob_attr *attr;
    struct blobmsg_hdr *hdr;
    int len = blobmsg_data_len(tb[DAWN_UMDNS_TABLE]);

    __blob_for_each_attr(attr, blobmsg_data(tb[DAWN_UMDNS_TABLE]), len)
    {
        hdr = blob_data(attr);

        struct blob_attr *tb_dawn[__DAWN_UMDNS_MAX];
        blobmsg_parse(dawn_umdns_policy, __DAWN_UMDNS_MAX, tb_dawn, blobmsg_data(attr), blobmsg_len(attr));

        printf("Hostname: %s\n", hdr->name);
        if (tb_dawn[DAWN_UMDNS_IPV4] && tb_dawn[DAWN_UMDNS_PORT]) {
            printf("IPV4: %s\n", blobmsg_get_string(tb_dawn[DAWN_UMDNS_IPV4]));
            printf("Port: %d\n", blobmsg_get_u32(tb_dawn[DAWN_UMDNS_PORT]));
        } else {
            return;
        }
        add_tcp_conncection(blobmsg_get_string(tb_dawn[DAWN_UMDNS_IPV4]), blobmsg_get_u32(tb_dawn[DAWN_UMDNS_PORT]));
    }
}

int ubus_call_umdns() {
    u_int32_t id;
    if (ubus_lookup_id(ctx, "umdns", &id)) {
        fprintf(stderr, "Failed to look up test object for %s\n", "umdns");
        return -1;
    }

    int timeout = 1;
    blob_buf_init(&b_umdns, 0);
    ubus_invoke(ctx, id, "update", b_umdns.head, NULL, NULL, timeout * 1000);
    ubus_invoke(ctx, id, "browse", b_umdns.head, ubus_umdns_cb, NULL, timeout * 1000);

    return 0;
}

//TODO: ADD STUFF HERE!!!!
int ubus_send_probe_via_network(struct probe_entry_s probe_entry) {
    blob_buf_init(&b_probe, 0);
    blobmsg_add_macaddr(&b_probe, "bssid", probe_entry.bssid_addr);
    blobmsg_add_macaddr(&b_probe, "address", probe_entry.client_addr);
    blobmsg_add_macaddr(&b_probe, "target", probe_entry.target_addr);
    blobmsg_add_u32(&b_probe, "signal", probe_entry.signal);
    blobmsg_add_u32(&b_probe, "freq", probe_entry.freq);

    blobmsg_add_u32(&b_probe, "rcpi", probe_entry.rcpi);
    blobmsg_add_u32(&b_probe, "rsni", probe_entry.rsni);

    if (probe_entry.ht_capabilities)
    {
        void *ht_cap = blobmsg_open_table(&b, "ht_capabilities");
        blobmsg_close_table(&b, ht_cap);
    }

    if (probe_entry.vht_capabilities) {
        void *vht_cap = blobmsg_open_table(&b, "vht_capabilities");
        blobmsg_close_table(&b, vht_cap);
    }

    send_blob_attr_via_network(b_probe.head, "probe");

    return 0;
}

int send_set_probe(uint8_t client_addr[]) {
    blob_buf_init(&b_probe, 0);
    blobmsg_add_macaddr(&b_probe, "bssid", client_addr);
    blobmsg_add_macaddr(&b_probe, "address", client_addr);

    send_blob_attr_via_network(b_probe.head, "setprobe");

    return 0;
}

enum {
    MAC_ADDR,
    __ADD_DEL_MAC_MAX
};

static const struct blobmsg_policy add_del_policy[__ADD_DEL_MAC_MAX] = {
        [MAC_ADDR] = {"addrs", BLOBMSG_TYPE_ARRAY},
};

static const struct ubus_method dawn_methods[] = {
        UBUS_METHOD("add_mac", add_mac, add_del_policy),
        UBUS_METHOD_NOARG("get_hearing_map", get_hearing_map),
        UBUS_METHOD_NOARG("get_network", get_network),
        UBUS_METHOD_NOARG("reload_config", reload_config)
};

static struct ubus_object_type dawn_object_type =
        UBUS_OBJECT_TYPE("dawn", dawn_methods);

static struct ubus_object dawn_object = {
        .name = "dawn",
        .type = &dawn_object_type,
        .methods = dawn_methods,
        .n_methods = ARRAY_SIZE(dawn_methods),
};

int parse_add_mac_to_file(struct blob_attr *msg) {
    struct blob_attr *tb[__ADD_DEL_MAC_MAX];
    struct blob_attr *attr;

    printf("Parsing MAC!\n");

    blobmsg_parse(add_del_policy, __ADD_DEL_MAC_MAX, tb, blob_data(msg), blob_len(msg));

    if (!tb[MAC_ADDR])
        return UBUS_STATUS_INVALID_ARGUMENT;

    int len = blobmsg_data_len(tb[MAC_ADDR]);
    printf("Length of array maclist: %d\n", len);

    __blob_for_each_attr(attr, blobmsg_data(tb[MAC_ADDR]), len)
    {
        printf("Iteration through MAC-list\n");
        uint8_t addr[ETH_ALEN];
        hwaddr_aton(blobmsg_data(attr), addr);

        if (insert_to_maclist(addr) == 0) {
            // TODO: File can grow arbitarily large.  Resource consumption risk.
            // TODO: Consolidate use of file across source: shared resource for name, single point of access?
            write_mac_to_file("/tmp/dawn_mac_list", addr);
        }
    }

    return 0;
}

static int add_mac(struct ubus_context *ctx, struct ubus_object *obj,
                   struct ubus_request_data *req, const char *method,
                   struct blob_attr *msg) {
    parse_add_mac_to_file(msg);

    // here we need to send it via the network!
    send_blob_attr_via_network(msg, "addmac");

    return 0;
}

int send_add_mac(uint8_t *client_addr) {
    blob_buf_init(&b, 0);
    blobmsg_add_macaddr(&b, "addr", client_addr);
    send_blob_attr_via_network(b.head, "addmac");
    return 0;
}

static int reload_config(struct ubus_context *ctx, struct ubus_object *obj,
                           struct ubus_request_data *req, const char *method,
                           struct blob_attr *msg) {
    int ret;
    blob_buf_init(&b, 0);
    uci_reset();
    dawn_metric = uci_get_dawn_metric();
    timeout_config = uci_get_time_config();
    uci_get_dawn_hostapd_dir();
    uci_get_dawn_sort_order();

    if(timeout_config.update_beacon_reports) // allow setting timeout to 0
        uloop_timeout_add(&beacon_reports_timer); // callback = update_beacon_reports

    uci_send_via_network();
    ret = ubus_send_reply(ctx, req, b.head);
    if (ret)
        fprintf(stderr, "Failed to send reply: %s\n", ubus_strerror(ret));
    return 0;
}

static int get_hearing_map(struct ubus_context *ctx, struct ubus_object *obj,
                           struct ubus_request_data *req, const char *method,
                           struct blob_attr *msg) {
    int ret;

    build_hearing_map_sort_client(&b);
    ret = ubus_send_reply(ctx, req, b.head);
    if (ret)
        fprintf(stderr, "Failed to send reply: %s\n", ubus_strerror(ret));
    return 0;
}


static int get_network(struct ubus_context *ctx, struct ubus_object *obj,
                       struct ubus_request_data *req, const char *method,
                       struct blob_attr *msg) {
    int ret;

    build_network_overview(&b);
    ret = ubus_send_reply(ctx, req, b.head);
    if (ret)
        fprintf(stderr, "Failed to send reply: %s\n", ubus_strerror(ret));
    return 0;
}

static void ubus_add_oject() {
    int ret;

    ret = ubus_add_object(ctx, &dawn_object);
    if (ret)
        fprintf(stderr, "Failed to add object: %s\n", ubus_strerror(ret));
}

static void respond_to_notify(uint32_t id) {
    // This is needed to respond to the ubus notify ...
    // Maybe we need to disable on shutdown...
    // But it is not possible when we disable the notify that other daemons are running that relay on this notify...
    int ret;

    blob_buf_init(&b, 0);
    blobmsg_add_u32(&b, "notify_response", 1);

    int timeout = 1;
    ret = ubus_invoke(ctx, id, "notify_response", b.head, NULL, NULL, timeout * 1000);
    if (ret)
        fprintf(stderr, "Failed to invoke: %s\n", ubus_strerror(ret));
}

static void enable_rrm(uint32_t id) {
    int ret;

    blob_buf_init(&b, 0);
    blobmsg_add_u8(&b, "neighbor_report", 1);
    blobmsg_add_u8(&b, "beacon_report", 1);
    blobmsg_add_u8(&b, "bss_transition", 1);

    int timeout = 1;
    ret = ubus_invoke(ctx, id, "bss_mgmt_enable", b.head, NULL, NULL, timeout * 1000);
    if (ret)
        fprintf(stderr, "Failed to invoke: %s\n", ubus_strerror(ret));
}

static void hostapd_handle_remove(struct ubus_context *ctx,
                                  struct ubus_subscriber *s, uint32_t id) {
    fprintf(stdout, "Object %08x went away\n", id);
    struct hostapd_sock_entry *hostapd_sock = container_of(s,
    struct hostapd_sock_entry, subscriber);

    if (hostapd_sock->id != id) {
        printf("ID is not the same!\n");
        return;
    }

    hostapd_sock->subscribed = false;
    subscription_wait(&hostapd_sock->wait_handler);

}

bool subscribe(struct hostapd_sock_entry *hostapd_entry) {
    char subscribe_name[sizeof("hostapd.") + MAX_INTERFACE_NAME + 1];

    if (hostapd_entry->subscribed)
        return false;

    sprintf(subscribe_name, "hostapd.%s", hostapd_entry->iface_name);

    if (ubus_lookup_id(ctx, subscribe_name, &hostapd_entry->id)) {
        fprintf(stdout, "Failed to lookup ID!");
        subscription_wait(&hostapd_entry->wait_handler);
        return false;
    }

    if (ubus_subscribe(ctx, &hostapd_entry->subscriber, hostapd_entry->id)) {
        fprintf(stdout, "Failed to register subscriber!");
        subscription_wait(&hostapd_entry->wait_handler);
        return false;
    }

    hostapd_entry->subscribed = true;

    get_bssid(hostapd_entry->iface_name, hostapd_entry->bssid_addr);
    get_ssid(hostapd_entry->iface_name, hostapd_entry->ssid, (SSID_MAX_LEN) * sizeof(char));

    hostapd_entry->ht_support = (uint8_t) support_ht(hostapd_entry->iface_name);
    hostapd_entry->vht_support = (uint8_t) support_vht(hostapd_entry->iface_name);

    respond_to_notify(hostapd_entry->id);
    enable_rrm(hostapd_entry->id);
    ubus_get_rrm();

    printf("Subscribed to: %s\n", hostapd_entry->iface_name);

    return true;
}

static void
wait_cb(struct ubus_context *ctx, struct ubus_event_handler *ev_handler,
        const char *type, struct blob_attr *msg) {
    static const struct blobmsg_policy wait_policy = {
            "path", BLOBMSG_TYPE_STRING
    };

    struct blob_attr *attr;
    const char *path;
    struct hostapd_sock_entry *sub = container_of(ev_handler,
            struct hostapd_sock_entry, wait_handler);

    if (strcmp(type, "ubus.object.add"))
        return;

    blobmsg_parse(&wait_policy, 1, &attr, blob_data(msg), blob_len(msg));
    if (!attr)
        return;

    path = blobmsg_data(attr);

    path = strchr(path, '.');
    if (!path)
        return;

    if (strcmp(sub->iface_name, path + 1))
        return;

    subscribe(sub);
}

bool subscriber_to_interface(const char *ifname) {

    struct hostapd_sock_entry *hostapd_entry;

    hostapd_entry = calloc(1, sizeof(struct hostapd_sock_entry));
    strcpy(hostapd_entry->iface_name, ifname);

    // add hostname
    uci_get_hostname(hostapd_entry->hostname);

    hostapd_entry->subscriber.cb = hostapd_notify;
    hostapd_entry->subscriber.remove_cb = hostapd_handle_remove;
    hostapd_entry->wait_handler.cb = wait_cb;

    hostapd_entry->subscribed = false;

    if (ubus_register_subscriber(ctx, &hostapd_entry->subscriber)) {
        fprintf(stderr, "Failed to register subscriber!");
        return false;
    }

    list_add(&hostapd_entry->list, &hostapd_sock_list);

    return subscribe(hostapd_entry);
}

void subscribe_to_new_interfaces(const char *hostapd_sock_path) {
    DIR *dirp;
    struct dirent *entry;
    struct hostapd_sock_entry *sub = NULL;

    if (ctx == NULL) {
        return;
    }

    dirp = opendir(hostapd_sock_path);  // error handling?
    if (!dirp) {
        fprintf(stderr, "[SUBSCRIBING] No hostapd sockets!\n");
        return;
    }
    while ((entry = readdir(dirp)) != NULL) {
        if (entry->d_type == DT_SOCK) {
            bool do_subscribe = true;
            if (strcmp(entry->d_name, "global") == 0)
                continue;
            list_for_each_entry(sub, &hostapd_sock_list, list)
            {
                if (strncmp(sub->iface_name, entry->d_name, MAX_INTERFACE_NAME) == 0) {
                    do_subscribe = false;
                    break;
                }
            }
            if (do_subscribe) {
                subscriber_to_interface(entry->d_name);
            }

        }
    }
    closedir(dirp);
    return;
}

int uci_send_via_network()
{
    void *metric, *times;

    blob_buf_init(&b, 0);
    metric = blobmsg_open_table(&b, "metric");
    blobmsg_add_u32(&b, "ht_support", dawn_metric.ht_support);
    blobmsg_add_u32(&b, "vht_support", dawn_metric.vht_support);
    blobmsg_add_u32(&b, "no_ht_support", dawn_metric.no_ht_support);
    blobmsg_add_u32(&b, "no_vht_support", dawn_metric.no_vht_support);
    blobmsg_add_u32(&b, "rssi", dawn_metric.rssi);
    blobmsg_add_u32(&b, "low_rssi", dawn_metric.low_rssi);
    blobmsg_add_u32(&b, "freq", dawn_metric.freq);
    blobmsg_add_u32(&b, "chan_util", dawn_metric.chan_util);


    blobmsg_add_u32(&b, "max_chan_util", dawn_metric.max_chan_util);
    blobmsg_add_u32(&b, "rssi_val", dawn_metric.rssi_val);
    blobmsg_add_u32(&b, "low_rssi_val", dawn_metric.low_rssi_val);
    blobmsg_add_u32(&b, "chan_util_val", dawn_metric.chan_util_val);
    blobmsg_add_u32(&b, "max_chan_util_val", dawn_metric.max_chan_util_val);
    blobmsg_add_u32(&b, "min_probe_count", dawn_metric.min_probe_count);
    blobmsg_add_u32(&b, "bandwidth_threshold", dawn_metric.bandwidth_threshold);
    blobmsg_add_u32(&b, "use_station_count", dawn_metric.use_station_count);
    blobmsg_add_u32(&b, "max_station_diff", dawn_metric.max_station_diff);
    blobmsg_add_u32(&b, "eval_probe_req", dawn_metric.eval_probe_req);
    blobmsg_add_u32(&b, "eval_auth_req", dawn_metric.eval_auth_req);
    blobmsg_add_u32(&b, "eval_assoc_req", dawn_metric.eval_assoc_req);
    blobmsg_add_u32(&b, "kicking", dawn_metric.kicking);
    blobmsg_add_u32(&b, "deny_auth_reason", dawn_metric.deny_auth_reason);
    blobmsg_add_u32(&b, "deny_assoc_reason", dawn_metric.deny_assoc_reason);
    blobmsg_add_u32(&b, "use_driver_recog", dawn_metric.use_driver_recog);
    blobmsg_add_u32(&b, "min_number_to_kick", dawn_metric.min_kick_count);
    blobmsg_add_u32(&b, "chan_util_avg_period", dawn_metric.chan_util_avg_period);
    blobmsg_add_u32(&b, "set_hostapd_nr", dawn_metric.set_hostapd_nr);
    blobmsg_add_u32(&b, "op_class", dawn_metric.op_class);
    blobmsg_add_u32(&b, "duration", dawn_metric.duration);
    blobmsg_add_u32(&b, "mode", dawn_metric.mode);
    blobmsg_add_u32(&b, "scan_channel", dawn_metric.scan_channel);
    blobmsg_close_table(&b, metric);

    times = blobmsg_open_table(&b, "times");
    blobmsg_add_u32(&b, "update_client", timeout_config.update_client);
    blobmsg_add_u32(&b, "denied_req_threshold", timeout_config.denied_req_threshold);
    blobmsg_add_u32(&b, "remove_client", timeout_config.remove_client);
    blobmsg_add_u32(&b, "remove_probe", timeout_config.remove_probe);
    blobmsg_add_u32(&b, "remove_ap", timeout_config.remove_ap);
    blobmsg_add_u32(&b, "update_hostapd", timeout_config.update_hostapd);
    blobmsg_add_u32(&b, "update_tcp_con", timeout_config.update_tcp_con);
    blobmsg_add_u32(&b, "update_chan_util", timeout_config.update_chan_util);
    blobmsg_add_u32(&b, "update_beacon_reports", timeout_config.update_beacon_reports);
    blobmsg_close_table(&b, times);

    send_blob_attr_via_network(b.head, "uci");

    return 0;
}
int build_hearing_map_sort_client(struct blob_buf *b) {
    print_probe_array();
    pthread_mutex_lock(&probe_array_mutex);

    void *client_list, *ap_list, *ssid_list;
    char ap_mac_buf[20];
    char client_mac_buf[20];

    blob_buf_init(b, 0);
    int m;
    for (m = 0; m <= ap_entry_last; m++) {
        if (m > 0) {
            if (strcmp((char *) ap_array[m].ssid, (char *) ap_array[m - 1].ssid) == 0) {
                continue;
            }
        }
        ssid_list = blobmsg_open_table(b, (char *) ap_array[m].ssid);

        int i;
        for (i = 0; i <= probe_entry_last; i++) {
            /*if(!mac_is_equal(ap_array[m].bssid_addr, probe_array[i].bssid_addr))
            {
                continue;
            }*/

            ap ap_entry_i = ap_array_get_ap(probe_array[i].bssid_addr);

            if (!mac_is_equal(ap_entry_i.bssid_addr, probe_array[i].bssid_addr)) {
                continue;
            }

            if (strcmp((char *) ap_entry_i.ssid, (char *) ap_array[m].ssid) != 0) {
                continue;
            }

            int k;
            sprintf(client_mac_buf, MACSTR, MAC2STR(probe_array[i].client_addr));
            client_list = blobmsg_open_table(b, client_mac_buf);
            for (k = i; k <= probe_entry_last; k++) {
                ap ap_entry = ap_array_get_ap(probe_array[k].bssid_addr);

                if (!mac_is_equal(ap_entry.bssid_addr, probe_array[k].bssid_addr)) {
                    continue;
                }

                if (strcmp((char *) ap_entry.ssid, (char *) ap_array[m].ssid) != 0) {
                    continue;
                }

                if (!mac_is_equal(probe_array[k].client_addr, probe_array[i].client_addr)) {
                    i = k - 1;
                    break;
                } else if (k == probe_entry_last) {
                    i = k;
                }

                sprintf(ap_mac_buf, MACSTR, MAC2STR(probe_array[k].bssid_addr));
                ap_list = blobmsg_open_table(b, ap_mac_buf);
                blobmsg_add_u32(b, "signal", probe_array[k].signal);
                blobmsg_add_u32(b, "rcpi", probe_array[k].rcpi);
                blobmsg_add_u32(b, "rsni", probe_array[k].rsni);
                blobmsg_add_u32(b, "freq", probe_array[k].freq);
                blobmsg_add_u8(b, "ht_capabilities", probe_array[k].ht_capabilities);
                blobmsg_add_u8(b, "vht_capabilities", probe_array[k].vht_capabilities);


                // check if ap entry is available
                blobmsg_add_u32(b, "channel_utilization", ap_entry.channel_utilization);
                blobmsg_add_u32(b, "num_sta", ap_entry.station_count);
                blobmsg_add_u8(b, "ht_support", ap_entry.ht_support);
                blobmsg_add_u8(b, "vht_support", ap_entry.vht_support);

                blobmsg_add_u32(b, "score", eval_probe_metric(probe_array[k]));
                blobmsg_close_table(b, ap_list);
            }
            blobmsg_close_table(b, client_list);
        }
        blobmsg_close_table(b, ssid_list);
    }
    pthread_mutex_unlock(&probe_array_mutex);
    return 0;
}

int build_network_overview(struct blob_buf *b) {
    void *client_list, *ap_list, *ssid_list;
    char ap_mac_buf[20];
    char client_mac_buf[20];
    struct hostapd_sock_entry *sub;

    blob_buf_init(b, 0);
    int m;
    for (m = 0; m <= ap_entry_last; m++) {
        bool add_ssid = false;
        bool close_ssid = false;

        if (m == 0 || strcmp((char *) ap_array[m].ssid, (char *) ap_array[m - 1].ssid) != 0) {
            add_ssid = true;
        }

        if (m >= ap_entry_last || strcmp((char *) ap_array[m].ssid, (char *) ap_array[m + 1].ssid) != 0) {
            close_ssid = true;
        }

        if(add_ssid)
        {
            ssid_list = blobmsg_open_table(b, (char *) ap_array[m].ssid);
        }
        sprintf(ap_mac_buf, MACSTR, MAC2STR(ap_array[m].bssid_addr));
        ap_list = blobmsg_open_table(b, ap_mac_buf);

        blobmsg_add_u32(b, "freq", ap_array[m].freq);
        blobmsg_add_u32(b, "channel_utilization", ap_array[m].channel_utilization);
        blobmsg_add_u32(b, "num_sta", ap_array[m].station_count);
        blobmsg_add_u8(b, "ht_support", ap_array[m].ht_support);
        blobmsg_add_u8(b, "vht_support", ap_array[m].vht_support);

        bool local_ap = false;
        list_for_each_entry(sub, &hostapd_sock_list, list)
        {
            if (mac_is_equal(ap_array[m].bssid_addr, sub->bssid_addr)) {
                local_ap = true;
            }
        }
        blobmsg_add_u8(b, "local", local_ap);

        char *nr;
        nr = blobmsg_alloc_string_buffer(b, "neighbor_report", NEIGHBOR_REPORT_LEN);
        sprintf(nr, "%s", ap_array[m].neighbor_report); // TODO: Why not strcpy()
        blobmsg_add_string_buffer(b);

        char *iface;
        iface = blobmsg_alloc_string_buffer(b, "iface", MAX_INTERFACE_NAME);
        sprintf(iface, "%s", ap_array[m].iface);
        blobmsg_add_string_buffer(b);

        char *hostname;
        hostname = blobmsg_alloc_string_buffer(b, "hostname", HOST_NAME_MAX);
        sprintf(hostname, "%s", ap_array[m].hostname);
        blobmsg_add_string_buffer(b);

        int k;
        for (k = 0; k <= client_entry_last; k++) {

            if (mac_is_equal(ap_array[m].bssid_addr, client_array[k].bssid_addr)) {
                sprintf(client_mac_buf, MACSTR, MAC2STR(client_array[k].client_addr));
                client_list = blobmsg_open_table(b, client_mac_buf);

                if(strlen(client_array[k].signature) != 0)
                {
                    char *s;
                    s = blobmsg_alloc_string_buffer(b, "signature", 1024);
                    sprintf(s, "%s", client_array[k].signature);
                    blobmsg_add_string_buffer(b);
                }
                blobmsg_add_u8(b, "ht", client_array[k].ht);
                blobmsg_add_u8(b, "vht", client_array[k].vht);
                blobmsg_add_u32(b, "collision_count", ap_get_collision_count(ap_array[m].collision_domain));

                int n;
                for(n = 0; n <= probe_entry_last; n++)
                {
                    if (mac_is_equal(client_array[k].client_addr, probe_array[n].client_addr) &&
                            mac_is_equal(client_array[k].bssid_addr, probe_array[n].bssid_addr)) {
                        blobmsg_add_u32(b, "signal", probe_array[n].signal);
                        break;
                    }
                }
                blobmsg_close_table(b, client_list);
            }
        }
        blobmsg_close_table(b, ap_list);
        if(close_ssid)
        {
            blobmsg_close_table(b, ssid_list);
        }
    }
    return 0;
}

int ap_get_nr(struct blob_buf *b_local, uint8_t own_bssid_addr[]) {

    pthread_mutex_lock(&ap_array_mutex);
    int i;

    void* nbs = blobmsg_open_array(b_local, "list");

    for (i = 0; i <= ap_entry_last; i++) {
        if (mac_is_equal(own_bssid_addr, ap_array[i].bssid_addr)) {
            continue; //TODO: Skip own entry?!
        }

        void* nr_entry = blobmsg_open_array(b_local, NULL);

        char mac_buf[20];
        sprintf(mac_buf, MACSTRLOWER, MAC2STR(ap_array[i].bssid_addr));
        blobmsg_add_string(b_local, NULL, mac_buf);

        blobmsg_add_string(b_local, NULL, (char *) ap_array[i].ssid);
        blobmsg_add_string(b_local, NULL, ap_array[i].neighbor_report);
        blobmsg_close_array(b_local, nr_entry);

    }
    blobmsg_close_array(b_local, nbs);

    pthread_mutex_unlock(&ap_array_mutex);

    return 0;
}

void uloop_add_data_cbs() {
    uloop_timeout_add(&probe_timeout);  //  callback = remove_probe_array_cb
    uloop_timeout_add(&client_timeout);  //  callback = remove_client_array_cb
    uloop_timeout_add(&ap_timeout);  //  callback = remove_ap_array_cb

    if (dawn_metric.use_driver_recog) {
        uloop_timeout_add(&denied_req_timeout);  //  callback = denied_req_array_cb
    }
}

// TODO: Move mutex handling to remove_??? function to make test harness simpler?
// Or not needed as test harness not threaded?
void remove_probe_array_cb(struct uloop_timeout* t) {
    pthread_mutex_lock(&probe_array_mutex);
    printf("[Thread] : Removing old probe entries!\n");
    remove_old_probe_entries(time(0), timeout_config.remove_probe);
    printf("[Thread] : Removing old entries finished!\n");
    pthread_mutex_unlock(&probe_array_mutex);
    uloop_timeout_set(&probe_timeout, timeout_config.remove_probe * 1000);
}

// TODO: Move mutex handling to remove_??? function to make test harness simpler?
// Or not needed as test harness not threaded?
void remove_client_array_cb(struct uloop_timeout* t) {
    pthread_mutex_lock(&client_array_mutex);
    printf("[Thread] : Removing old client entries!\n");
    remove_old_client_entries(time(0), timeout_config.update_client);
    pthread_mutex_unlock(&client_array_mutex);
    uloop_timeout_set(&client_timeout, timeout_config.update_client * 1000);
}

// TODO: Move mutex handling to remove_??? function to make test harness simpler?
// Or not needed as test harness not threaded?
void remove_ap_array_cb(struct uloop_timeout* t) {
    pthread_mutex_lock(&ap_array_mutex);
    printf("[ULOOP] : Removing old ap entries!\n");
    remove_old_ap_entries(time(0), timeout_config.remove_ap);
    pthread_mutex_unlock(&ap_array_mutex);
    uloop_timeout_set(&ap_timeout, timeout_config.remove_ap * 1000);
}

// TODO: Move mutex handling to (new) remove_??? function to make test harness simpler?
// Or not needed as test harness not threaded?
void denied_req_array_cb(struct uloop_timeout* t) {
    pthread_mutex_lock(&denied_array_mutex);
    printf("[ULOOP] : Processing denied authentication!\n");

    time_t current_time = time(0);

    int i = 0;
    while (i <= denied_req_last) {
        // check counter

        //check timer
        if (denied_req_array[i].time < current_time - timeout_config.denied_req_threshold) {

            // client is not connected for a given time threshold!
            if (!is_connected_somehwere(denied_req_array[i].client_addr)) {
                printf("Client has probably a bad driver!\n");

                // problem that somehow station will land into this list
                // maybe delete again?
                if (insert_to_maclist(denied_req_array[i].client_addr) == 0) {
                    send_add_mac(denied_req_array[i].client_addr);
                    // TODO: File can grow arbitarily large.  Resource consumption risk.
                    // TODO: Consolidate use of file across source: shared resource for name, single point of access?
                    write_mac_to_file("/tmp/dawn_mac_list", denied_req_array[i].client_addr);
                }
            }
            denied_req_array_delete(denied_req_array[i]);
        }
        else
        {
            i++;
        }
    }
    pthread_mutex_unlock(&denied_array_mutex);
    uloop_timeout_set(&denied_req_timeout, timeout_config.denied_req_threshold * 1000);
}



