/*
 * Description: 
 *     History: yang@haipo.me, 2017/04/21, create
 */

# include <stdbool.h>

# include "ut_log.h"
# include "ut_misc.h"
# include "ut_http_svr.h"

# define CLIENT_MAX_IDLE_TIME   3600

struct clt_info {
    nw_ses *ses;
    double last_activity;
    struct http_parser parser;
    sds field;
    sds value;
    http_request_t *request;
};

int on_message_begin(http_parser* parser)
{
    struct clt_info *info = parser->data;
    info->request = http_request_new();
    if (info->request == NULL) {
        return -__LINE__;
    }

    return 0;
}

int on_message_complete(http_parser* parser)
{
    struct clt_info *info = parser->data;
    http_svr *svr = http_svr_from_ses(info->ses);
    int ret = svr->on_request(info->ses, info->request);
    http_request_release(info->request);
    info->request = NULL;

    return ret;
}

int on_url(http_parser* parser, const char* at, size_t length)
{
    struct clt_info *info = parser->data;
    info->request->url = sdsnewlen(at, length);

    return 0;
}

int on_header_field(http_parser* parser, const char* at, size_t length)
{
    struct clt_info *info = parser->data;
    info->field = sdsnewlen(at, length);

    return 0;
}

int on_header_value(http_parser* parser, const char* at, size_t length)
{
    struct clt_info *info = parser->data;
    info->value = sdsnewlen(at, length);
    if (info->field && info->value) {
        http_request_set_header(info->request, info->field, info->value);
        sdsfree(info->field);
        sdsfree(info->value);
        info->field = NULL;
        info->value = NULL;
    }

    return 0;
}

int on_body(http_parser* parser, const char* at, size_t length)
{
    struct clt_info *info = parser->data;
    info->request->body = sdsnewlen(at, length);

    return 0;
}

static int decode_pkg(nw_ses *ses, void *data, size_t max)
{
    return max;
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    log_error("peer: %s: %s", nw_sock_human_addr(&ses->peer_addr), msg);
}

static void on_new_connection(nw_ses *ses)
{
    log_trace("new connection from: %s", nw_sock_human_addr(&ses->peer_addr));
    struct clt_info *info = ses->privdata;
    memset(info, 0, sizeof(struct clt_info));
    info->ses = ses;
    info->last_activity = current_timestamp();
    http_parser_init(&info->parser, HTTP_REQUEST);
    info->parser.data = info;
}

static void on_connection_close(nw_ses *ses)
{
    log_trace("connection %s close", nw_sock_human_addr(&ses->peer_addr));
}

static void *on_privdata_alloc(void *svr)
{
    http_svr *h_svr = ((nw_svr *)svr)->privdata;
    return nw_cache_alloc(h_svr->privdata_cache);
}

static void on_privdata_free(void *svr, void *privdata)
{
    struct clt_info *info = privdata;
    if (info->field) {
        sdsfree(info->field);
    }
    if (info->value) {
        sdsfree(info->value);
    }
    if (info->request) {
        http_request_release(info->request);
    }
    http_svr *h_svr = ((nw_svr *)svr)->privdata;
    return nw_cache_free(h_svr->privdata_cache, privdata);
}

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    struct clt_info *info = ses->privdata;
    info->last_activity = current_timestamp();
    http_svr *svr = http_svr_from_ses(ses);
    size_t nparsed = http_parser_execute(&info->parser, svr->settings, data, size);
    if (nparsed != size) {
        log_error("peer: %s http parse error: %s (%s)", nw_sock_human_addr(&ses->peer_addr),
                http_errno_description(HTTP_PARSER_ERRNO(&info->parser)),
                http_errno_name(HTTP_PARSER_ERRNO(&info->parser)));
        nw_svr_close_clt(svr->raw_svr, ses);
    }
}

static void on_timer(nw_timer *timer, void *privdata)
{
   http_svr *svr = privdata;
   double now = current_timestamp();

   nw_ses *curr = svr->raw_svr->clt_list_head;
   nw_ses *next;
   while (curr) {
       next = curr->next;
       struct clt_info *info = curr->privdata;
       if (now - info->last_activity > CLIENT_MAX_IDLE_TIME) {
           log_error("peer: %s: last_activity: %f, idle too long", nw_sock_human_addr(&curr->peer_addr), info->last_activity);
           nw_svr_close_clt(svr->raw_svr, curr);
       }
       curr = next;
   }
}

http_svr *http_svr_create(http_svr_cfg *cfg, http_request_callback on_request)
{
    http_svr *svr = malloc(sizeof(http_svr));
    memset(svr, 0, sizeof(http_svr));

    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.on_error_msg = on_error_msg;
    type.decode_pkg = decode_pkg;
    type.on_new_connection = on_new_connection;
    type.on_connection_close = on_connection_close;
    type.on_recv_pkg = on_recv_pkg;
    type.on_privdata_alloc = on_privdata_alloc;
    type.on_privdata_free = on_privdata_free;

    svr->raw_svr = nw_svr_create(cfg, &type, svr);
    if (svr->raw_svr == NULL) {
        free(svr);
        return NULL;
    }

    http_parser_settings *settings = malloc(sizeof(http_parser_settings));
    memset(settings, 0, sizeof(http_parser_settings));
    settings->on_message_begin = on_message_begin;
    settings->on_url = on_url;
    settings->on_header_field = on_header_field;
    settings->on_header_value = on_header_value;
    settings->on_body = on_body;
    settings->on_message_complete = on_message_complete;

    svr->settings = settings;
    svr->privdata_cache = nw_cache_create(sizeof(struct clt_info));
    svr->on_request = on_request;
    nw_timer_set(&svr->timer, 60, true, on_timer, svr);

    return svr;
}

int http_svr_start(http_svr *svr)
{
    int ret = nw_svr_start(svr->raw_svr);
    if (ret < 0)
        return ret;
    nw_timer_start(&svr->timer);

    return 0;
}

int http_svr_stop(http_svr *svr)
{
    int ret = nw_svr_stop(svr->raw_svr);
    if (ret < 0)
        return ret;
    nw_timer_stop(&svr->timer);

    return 0;
}

int send_http_response(nw_ses *ses, http_response_t *response)
{
    sds msg = http_response_encode(response);
    if (msg == NULL)
        return -__LINE__;
    int ret = nw_ses_send(ses, msg, sdslen(msg));
    sdsfree(msg);

    return ret;
}

int send_http_response_simple(nw_ses *ses, uint32_t status, sds content)
{
    http_response_t *response = http_response_new();
    if (response == NULL)
        return -__LINE__;
    response->status = status;
    response->content = content;
    int ret = send_http_response(ses, response);
    response->content = NULL;
    http_response_release(response);

    return ret;
}

http_svr *http_svr_from_ses(nw_ses *ses)
{
    return ((nw_svr *)ses->svr)->privdata;
}

void http_svr_release(http_svr *svr)
{
    nw_svr_release(svr->raw_svr);
    nw_timer_stop(&svr->timer);
    nw_cache_release(svr->privdata_cache);
    free(svr->settings);
    free(svr);
}

