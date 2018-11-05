/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include <libcouchbase/couchbase.h>
#include <libcouchbase/analytics.h>
#include <jsparse/parser.h>
#include "internal.h"
#include "auth-priv.h"
#include "http/http.h"
#include "logging.h"
#include "contrib/lcb-jsoncpp/lcb-jsoncpp.h"
#include <map>
#include <string>
#include <list>

#define LOGFMT "(NR=%p) "
#define LOGID(req) static_cast< const void * >(req)
#define LOGARGS(req, lvl) req->instance->settings, "analytics", LCB_LOG_##lvl, __FILE__, __LINE__

struct lcb_CMDANALYTICS_st {
    Json::Value root;
    std::string encoded;
    lcb_ANALYTICSCALLBACK callback;
    lcb_ANALYTICSHANDLE handle;

    lcb_CMDANALYTICS_st() : root(Json::objectValue), callback(NULL), handle(NULL) {}

    bool encode()
    {
        encoded = Json::FastWriter().write(root);
        return true;
    }
};

LIBCOUCHBASE_API
lcb_CMDANALYTICS *lcb_analytics_new(void)
{
    return new lcb_CMDANALYTICS;
}

LIBCOUCHBASE_API
void lcb_analytics_reset(lcb_CMDANALYTICS *cmd)
{
    cmd->encoded.clear();
    cmd->root.clear();
}

LIBCOUCHBASE_API
void lcb_analytics_free(lcb_CMDANALYTICS *cmd)
{
    delete cmd;
}

LIBCOUCHBASE_API
lcb_ANALYTICSHANDLE lcb_analytics_gethandle(lcb_CMDANALYTICS *cmd)
{
    return cmd->handle;
}

LIBCOUCHBASE_API
lcb_error_t lcb_analytics_setcallback(lcb_CMDANALYTICS *cmd, lcb_ANALYTICSCALLBACK callback)
{
    if (cmd) {
        cmd->callback = callback;
        return LCB_SUCCESS;
    }
    return LCB_EINVAL;
}

#define fix_strlen(s, n)                                                                                               \
    if (n == (size_t)-1) {                                                                                             \
        n = strlen(s);                                                                                                 \
    }

LIBCOUCHBASE_API
lcb_error_t lcb_analytics_setquery(lcb_CMDANALYTICS *cmd, const char *qstr, size_t nqstr)
{
    fix_strlen(qstr, nqstr);
    Json::Value value;
    if (!Json::Reader().parse(qstr, qstr + nqstr, value)) {
        return LCB_EINVAL;
    }
    cmd->root = value;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API
lcb_error_t lcb_analytics_setopt(lcb_CMDANALYTICS *cmd, const char *k, size_t nk, const char *v, size_t nv)
{
    fix_strlen(v, nv);
    fix_strlen(k, nk);
    Json::Value value;
    if (!Json::Reader().parse(v, v + nv, value)) {
        return LCB_EINVAL;
    }
    cmd->root[std::string(k, nk)] = value;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API
lcb_error_t lcb_analytics_setstatement(lcb_CMDANALYTICS *cmd, const char *sstr, size_t nsstr)
{
    fix_strlen(sstr, nsstr);
    cmd->root["statement"] = std::string(sstr, nsstr);
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API
lcb_error_t lcb_analytics_namedparam(lcb_CMDANALYTICS *cmd, const char *name, size_t nname, const char *value,
                                     size_t nvalue)
{
    return lcb_analytics_setopt(cmd, name, nname, value, nvalue);
}

LIBCOUCHBASE_API
lcb_error_t lcb_analytics_posparam(lcb_CMDANALYTICS *cmd, const char *value, size_t nvalue)
{
    fix_strlen(value, nvalue);
    Json::Value jval;
    if (!Json::Reader().parse(value, value + nvalue, jval)) {
        return LCB_EINVAL;
    }
    cmd->root["args"].append(jval);
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API
lcb_error_t lcb_analytics_setdeferred(lcb_CMDANALYTICS *cmd, int deferred)
{
    if (deferred) {
        cmd->root["mode"] = std::string("async");
    } else {
        cmd->root.removeMember("mode");
    }
    return LCB_SUCCESS;
}

struct lcb_ANALYTICSDEFERREDHANDLE_st {
    std::string status;
    std::string handle;
    lcb_ANALYTICSCALLBACK callback;

    lcb_ANALYTICSDEFERREDHANDLE_st(std::string status_, std::string handle_) : status(status_), handle(handle_) {}
};

lcb_ANALYTICSDEFERREDHANDLE *lcb_analytics_defhnd_extract(const lcb_RESPANALYTICS *resp)
{
    if (resp == NULL || resp->rc != LCB_SUCCESS || ((resp->rflags & (LCB_RESP_F_FINAL | LCB_RESP_F_EXTDATA)) == 0) ||
        resp->nrow == 0 || resp->row == NULL) {
        return NULL;
    }
    Json::Value payload;
    if (!Json::Reader().parse(resp->row, resp->row + resp->nrow, payload)) {
        return NULL;
    }
    if (!payload.isObject()) {
        return NULL;
    }
    Json::Value status = payload["status"];
    Json::Value handle = payload["handle"];
    if (status.isString() && handle.isString()) {
        return new lcb_ANALYTICSDEFERREDHANDLE_st(status.asString(), handle.asString());
    }
    return NULL;
}

void lcb_analytics_defhnd_free(lcb_ANALYTICSDEFERREDHANDLE *handle)
{
    if (handle == NULL) {
        return;
    }
    delete handle;
}

const char *lcb_analytics_defhnd_status(lcb_ANALYTICSDEFERREDHANDLE *handle)
{
    if (handle == NULL) {
        return NULL;
    }
    return handle->status.c_str();
}

lcb_error_t lcb_analytics_defhnd_setcallback(lcb_ANALYTICSDEFERREDHANDLE *handle, lcb_ANALYTICSCALLBACK callback)
{
    if (handle) {
        handle->callback = callback;
        return LCB_SUCCESS;
    }
    return LCB_EINVAL;
}

typedef struct lcb_ANALYTICSREQ : lcb::jsparse::Parser::Actions {
    const lcb_RESPHTTP *cur_htresp;
    struct lcb_http_request_st *htreq;
    lcb::jsparse::Parser *parser;
    const void *cookie;
    lcb_ANALYTICSCALLBACK callback;
    lcb_t instance;
    lcb_error_t lasterr;
    lcb_U32 timeout;
    // How many rows were received. Used to avoid parsing the meta
    size_t nrows;

    /** Request body as received from the application */
    Json::Value json;
    const Json::Value &json_const() const
    {
        return json;
    }

    /** String of the original statement. Cached here to avoid jsoncpp lookups */
    std::string statement;

    /** Whether we're retrying this */
    bool was_retried;

    /** Non-empty if this is deferred query check/fetch */
    std::string deferred_handle;
#ifdef LCB_TRACING
    lcbtrace_SPAN *span;
#endif

    /**
     * Issues the HTTP request for the query
     * @param payload The body to send
     * @return Error code from lcb's http subsystem
     */
    inline lcb_error_t issue_htreq(const std::string &payload);

    lcb_error_t issue_htreq()
    {
        std::string s = Json::FastWriter().write(json);
        return issue_htreq(s);
    }

    /**
     * Attempt to retry the query. This will inspect the meta (if present)
     * for any errors indicating that a failure might be a result of a stale
     * plan, and if this query was retried already.
     * @return true if the retry was successful.
     */
    inline bool maybe_retry();

    /**
     * Returns true if payload matches retry conditions.
     */
    inline bool has_retriable_error(const Json::Value &root);

    /**
     * Pass a row back to the application
     * @param resp The response. This is populated with state information
     *  from the current query
     * @param is_last Whether this is the last row. If this is the last, then
     *  the RESP_F_FINAL flag is set, and no further callbacks will be invoked
     */
    inline void invoke_row(lcb_RESPANALYTICS *resp, bool is_last);

    inline lcb_ANALYTICSREQ(lcb_t obj, const void *user_cookie, lcb_CMDANALYTICS *cmd);
    inline lcb_ANALYTICSREQ(lcb_t obj, const void *user_cookie, lcb_ANALYTICSDEFERREDHANDLE *handle);
    inline ~lcb_ANALYTICSREQ();

    // Parser overrides:
    void JSPARSE_on_row(const lcb::jsparse::Row &row)
    {
        lcb_RESPANALYTICS resp = {0};
        resp.row = static_cast< const char * >(row.row.iov_base);
        resp.nrow = row.row.iov_len;
        nrows++;
        invoke_row(&resp, false);
    }
    void JSPARSE_on_error(const std::string &)
    {
        lasterr = LCB_PROTOCOL_ERROR;
    }
    void JSPARSE_on_complete(const std::string &)
    {
        // Nothing
    }

} ANALYTICSREQ;

static bool parse_json(const char *s, size_t n, Json::Value &res)
{
    return Json::Reader().parse(s, s + n, res);
}

bool ANALYTICSREQ::has_retriable_error(const Json::Value &root)
{
    if (!root.isObject()) {
        return false;
    }
    const Json::Value &errors = root["errors"];
    if (!errors.isArray()) {
        return false;
    }
    Json::Value::const_iterator ii;
    for (ii = errors.begin(); ii != errors.end(); ++ii) {
        const Json::Value &cur = *ii;
        if (!cur.isObject()) {
            continue; // eh?
        }
        const Json::Value &jcode = cur["code"];
        unsigned code = 0;
        if (jcode.isNumeric()) {
            code = jcode.asUInt();
            switch (code) {
                case 23000:
                case 23003:
                case 23007:
                    lcb_log(LOGARGS(this, TRACE), LOGFMT "Will retry request. code: %d", LOGID(this), code);
                    return true;
                default:
                    break;
            }
        }
    }
    return false;
}

bool ANALYTICSREQ::maybe_retry()
{
    // Examines the buffer to determine the type of error
    Json::Value root;
    lcb_IOV meta;

    if (callback == NULL) {
        // Cancelled
        return false;
    }

    if (nrows) {
        // Has results:
        return false;
    }

    if (was_retried) {
        return false;
    }

    was_retried = true;
    parser->get_postmortem(meta);
    if (!parse_json(static_cast< const char * >(meta.iov_base), meta.iov_len, root)) {
        return false; // Not JSON
    }
    if (has_retriable_error(root)) {
        return true;
    }

    return false;
}

void ANALYTICSREQ::invoke_row(lcb_RESPANALYTICS *resp, bool is_last)
{
    resp->cookie = const_cast< void * >(cookie);
    resp->htresp = cur_htresp;

    if (is_last) {
        lcb_IOV meta;
        resp->rflags |= LCB_RESP_F_FINAL;
        resp->rc = lasterr;
        parser->get_postmortem(meta);
        resp->row = static_cast< const char * >(meta.iov_base);
        resp->nrow = meta.iov_len;
        if (!deferred_handle.empty()) {
            /* signal that response might have deferred handle */
            resp->rflags |= LCB_RESP_F_EXTDATA;
        }
    }

    if (callback) {
        callback(instance, LCB_CALLBACK_ANALYTICS, resp);
    }
    if (is_last) {
        callback = NULL;
    }
}

lcb_ANALYTICSREQ::~lcb_ANALYTICSREQ()
{
    if (htreq) {
        lcb_cancel_http_request(instance, htreq);
        htreq = NULL;
    }

    if (callback) {
        lcb_RESPANALYTICS resp = {0};
        invoke_row(&resp, 1);
    }

#ifdef LCB_TRACING
    if (span) {
        if (htreq) {
            lcbio_CTX *ctx = htreq->ioctx;
            if (ctx) {
                std::string remote;
                if (htreq->ipv6) {
                    remote = "[" + std::string(htreq->host) + "]:" + std::string(htreq->port);
                } else {
                    remote = std::string(htreq->host) + ":" + std::string(htreq->port);
                }
                lcbtrace_span_add_tag_str(span, LCBTRACE_TAG_PEER_ADDRESS, remote.c_str());
                lcbtrace_span_add_tag_str(span, LCBTRACE_TAG_LOCAL_ADDRESS,
                                          lcbio__inet_ntop(&ctx->sock->info->sa_local).c_str());
            }
        }
        lcbtrace_span_finish(span, LCBTRACE_NOW);
        span = NULL;
    }
#endif

    if (parser) {
        delete parser;
    }
}

static void chunk_callback(lcb_t instance, int ign, const lcb_RESPBASE *rb)
{
    const lcb_RESPHTTP *rh = (const lcb_RESPHTTP *)rb;
    ANALYTICSREQ *req = static_cast< ANALYTICSREQ * >(rh->cookie);

    (void)ign;
    (void)instance;

    req->cur_htresp = rh;
    if (rh->rc != LCB_SUCCESS || rh->htstatus != 200) {
        if (req->lasterr == LCB_SUCCESS || rh->htstatus != 200) {
            req->lasterr = rh->rc ? rh->rc : LCB_HTTP_ERROR;
        }
    }

    if (rh->rflags & LCB_RESP_F_FINAL) {
        req->htreq = NULL;
        if (!req->maybe_retry()) {
            delete req;
        }
        return;
    } else if (req->callback == NULL) {
        /* Cancelled. Similar to the block above, except the http request
         * should remain alive (so we can cancel it later on) */
        delete req;
        return;
    }
    req->parser->feed(static_cast< const char * >(rh->body), rh->nbody);
}

lcb_error_t ANALYTICSREQ::issue_htreq(const std::string &body)
{
    lcb_CMDHTTP htcmd = {0};
    htcmd.body = body.c_str();
    htcmd.nbody = body.size();

    htcmd.content_type = "application/json";
    if (deferred_handle.empty()) {
        htcmd.method = LCB_HTTP_METHOD_POST;
    } else {
        htcmd.method = LCB_HTTP_METHOD_GET;
        htcmd.host = deferred_handle.c_str();
    }

    htcmd.type = LCB_HTTP_TYPE_CBAS;

    htcmd.cmdflags = LCB_CMDHTTP_F_STREAM | LCB_CMDHTTP_F_CASTMO;
    htcmd.reqhandle = &htreq;
    htcmd.cas = timeout;

    lcb_error_t rc = lcb_http3(instance, this, &htcmd);
    if (rc == LCB_SUCCESS) {
        htreq->set_callback(chunk_callback);
    }
    return rc;
}

lcb_U32 lcb_analyticsreq_parsetmo(const std::string &s)
{
    double num;
    int nchars, rv;

    rv = sscanf(s.c_str(), "%lf%n", &num, &nchars);
    if (rv != 1) {
        return 0;
    }
    std::string mults = s.substr(nchars);

    // Get the actual timeout value in microseconds. Note we can't use the macros
    // since they will truncate the double value.
    if (mults == "s") {
        return num * static_cast< double >(LCB_S2US(1));
    } else if (mults == "ms") {
        return num * static_cast< double >(LCB_MS2US(1));
    } else if (mults == "h") {
        return num * static_cast< double >(LCB_S2US(3600));
    } else if (mults == "us") {
        return num;
    } else if (mults == "m") {
        return num * static_cast< double >(LCB_S2US(60));
    } else if (mults == "ns") {
        return LCB_NS2US(num);
    } else {
        return 0;
    }
}

lcb_ANALYTICSREQ::lcb_ANALYTICSREQ(lcb_t obj, const void *user_cookie, lcb_CMDANALYTICS *cmd)
    : cur_htresp(NULL), htreq(NULL), parser(new lcb::jsparse::Parser(lcb::jsparse::Parser::MODE_ANALYTICS, this)),
      cookie(user_cookie), callback(cmd->callback), instance(obj), lasterr(LCB_SUCCESS), timeout(0), nrows(0),
      was_retried(false), deferred_handle("")
#ifdef LCB_TRACING
      ,
      span(NULL)
#endif
{
    if (cmd->handle) {
        cmd->handle = this;
    }

    if (!parse_json(cmd->encoded.c_str(), cmd->encoded.size(), json)) {
        lasterr = LCB_EINVAL;
        return;
    }

    const Json::Value &j_statement = json_const()["statement"];
    if (j_statement.isString()) {
        statement = j_statement.asString();
    } else if (!j_statement.isNull()) {
        lasterr = LCB_EINVAL;
        return;
    }

    Json::Value &tmoval = json["timeout"];
    if (tmoval.isNull()) {
        // Set the default timeout as the server-side query timeout if no
        // other timeout is used.
        char buf[64] = {0};
        sprintf(buf, "%uus", LCBT_SETTING(obj, n1ql_timeout));
        tmoval = buf;
        /* FIXME: use separate timeout for analytics */
        timeout = LCBT_SETTING(obj, n1ql_timeout);
    } else if (tmoval.isString()) {
        timeout = lcb_analyticsreq_parsetmo(tmoval.asString());
    } else {
        // Timeout is not a string!
        lasterr = LCB_EINVAL;
        return;
    }

#ifdef LCB_TRACING
    if (instance->settings->tracer) {
        char id[20] = {0};
        snprintf(id, sizeof(id), "%p", (void *)this);
        span = lcbtrace_span_start(instance->settings->tracer, LCBTRACE_OP_DISPATCH_TO_SERVER, LCBTRACE_NOW, NULL);
        lcbtrace_span_add_tag_str(span, LCBTRACE_TAG_OPERATION_ID, id);
        lcbtrace_span_add_system_tags(span, instance->settings, LCBTRACE_TAG_SERVICE_ANALYTICS);
    }
#endif
}

lcb_ANALYTICSREQ::lcb_ANALYTICSREQ(lcb_t obj, const void *user_cookie, lcb_ANALYTICSDEFERREDHANDLE *handle)
    : cur_htresp(NULL), htreq(NULL),
      parser(new lcb::jsparse::Parser(lcb::jsparse::Parser::MODE_ANALYTICS_DEFERRED, this)), cookie(user_cookie),
      callback(handle->callback), instance(obj), lasterr(LCB_SUCCESS), timeout(0), nrows(0), was_retried(false),
      deferred_handle(handle->handle)
#ifdef LCB_TRACING
      ,
      span(NULL)
#endif
{
    /* FIXME: use separate timeout for analytics */
    timeout = LCBT_SETTING(obj, n1ql_timeout);

#ifdef LCB_TRACING
    if (instance->settings->tracer) {
        char id[20] = {0};
        snprintf(id, sizeof(id), "%p", (void *)this);
        span = lcbtrace_span_start(instance->settings->tracer, LCBTRACE_OP_DISPATCH_TO_SERVER, LCBTRACE_NOW, NULL);
        lcbtrace_span_add_tag_str(span, LCBTRACE_TAG_OPERATION_ID, id);
        lcbtrace_span_add_system_tags(span, instance->settings, LCBTRACE_TAG_SERVICE_ANALYTICS);
    }
#endif
}

LIBCOUCHBASE_API
lcb_error_t lcb_analytics_query(lcb_t instance, const void *cookie, lcb_CMDANALYTICS *cmd)
{
    lcb_error_t err;
    ANALYTICSREQ *req = NULL;

    if (cmd->callback == NULL) {
        return LCB_EINVAL;
    }
    if (!cmd->encode()) {
        return LCB_EINVAL;
    }

    req = new lcb_ANALYTICSREQ(instance, cookie, cmd);
    if (!req) {
        err = LCB_CLIENT_ENOMEM;
        goto GT_DESTROY;
    }
    if ((err = req->lasterr) != LCB_SUCCESS) {
        goto GT_DESTROY;
    }

    if ((err = req->issue_htreq()) != LCB_SUCCESS) {
        goto GT_DESTROY;
    }

    return LCB_SUCCESS;

GT_DESTROY:
    if (cmd->handle) {
        cmd->handle = NULL;
    }

    if (req) {
        req->callback = NULL;
        delete req;
    }
    return err;
}

lcb_error_t lcb_analytics_defhnd_poll(lcb_t instance, const void *cookie, lcb_ANALYTICSDEFERREDHANDLE *handle)
{
    lcb_error_t err;
    ANALYTICSREQ *req = NULL;

    if (handle->callback == NULL || handle->handle.empty()) {
        return LCB_EINVAL;
    }

    req = new lcb_ANALYTICSREQ(instance, cookie, handle);
    if (!req) {
        err = LCB_CLIENT_ENOMEM;
        goto GT_DESTROY;
    }
    if ((err = req->lasterr) != LCB_SUCCESS) {
        goto GT_DESTROY;
    }

    if ((err = req->issue_htreq()) != LCB_SUCCESS) {
        goto GT_DESTROY;
    }

    return LCB_SUCCESS;

GT_DESTROY:
    if (req) {
        req->callback = NULL;
        delete req;
    }
    return err;
}

LIBCOUCHBASE_API
void lcb_analytics_cancel(lcb_t, lcb_ANALYTICSHANDLE handle)
{
    handle->callback = NULL;
}

#ifdef LCB_TRACING

LIBCOUCHBASE_API
void lcb_analytics_set_parent_span(lcb_t, lcb_ANALYTICSHANDLE handle, lcbtrace_SPAN *span)
{
    if (handle) {
        lcbtrace_span_set_parent(handle->span, span);
    }
}

#endif