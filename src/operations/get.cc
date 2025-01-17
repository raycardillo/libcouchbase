/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010-2020 Couchbase, Inc.
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

#include <memory>

#include "internal.h"
#include "collections.h"
#include "trace.h"
#include "defer.h"

#include "capi/cmd_get.hh"
#include "capi/cmd_get_replica.hh"

LIBCOUCHBASE_API lcb_STATUS lcb_respget_status(const lcb_RESPGET *resp)
{
    return resp->ctx.rc;
}

LIBCOUCHBASE_API lcb_STATUS lcb_respget_error_context(const lcb_RESPGET *resp, const lcb_KEY_VALUE_ERROR_CONTEXT **ctx)
{
    *ctx = &resp->ctx;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_respget_cookie(const lcb_RESPGET *resp, void **cookie)
{
    *cookie = resp->cookie;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_respget_cas(const lcb_RESPGET *resp, uint64_t *cas)
{
    *cas = resp->ctx.cas;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_respget_datatype(const lcb_RESPGET *resp, uint8_t *datatype)
{
    *datatype = resp->datatype;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_respget_flags(const lcb_RESPGET *resp, uint32_t *flags)
{
    *flags = resp->itmflags;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_respget_key(const lcb_RESPGET *resp, const char **key, size_t *key_len)
{
    *key = resp->ctx.key.c_str();
    *key_len = resp->ctx.key.size();
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_respget_value(const lcb_RESPGET *resp, const char **value, size_t *value_len)
{
    *value = (const char *)resp->value;
    *value_len = resp->nvalue;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_cmdget_create(lcb_CMDGET **cmd)
{
    *cmd = new lcb_CMDGET{};
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_cmdget_destroy(lcb_CMDGET *cmd)
{
    delete cmd;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_cmdget_timeout(lcb_CMDGET *cmd, uint32_t timeout)
{
    return cmd->timeout_in_microseconds(timeout);
}

LIBCOUCHBASE_API lcb_STATUS lcb_cmdget_parent_span(lcb_CMDGET *cmd, lcbtrace_SPAN *span)
{
    return cmd->parent_span(span);
}

LIBCOUCHBASE_API lcb_STATUS lcb_cmdget_collection(lcb_CMDGET *cmd, const char *scope, size_t scope_len,
                                                  const char *collection, size_t collection_len)
{
    try {
        lcb::collection_qualifier qualifier(scope, scope_len, collection, collection_len);
        return cmd->collection(std::move(qualifier));
    } catch (const std::invalid_argument &) {
        return LCB_ERR_INVALID_ARGUMENT;
    }
}

LIBCOUCHBASE_API lcb_STATUS lcb_cmdget_key(lcb_CMDGET *cmd, const char *key, size_t key_len)
{
    if (key == nullptr || key_len == 0) {
        return LCB_ERR_INVALID_ARGUMENT;
    }
    return cmd->key(std::string(key, key_len));
}

LIBCOUCHBASE_API lcb_STATUS lcb_cmdget_expiry(lcb_CMDGET *cmd, uint32_t expiration)
{
    return cmd->with_touch(expiration);
}

LIBCOUCHBASE_API lcb_STATUS lcb_cmdget_locktime(lcb_CMDGET *cmd, uint32_t duration)
{
    return cmd->with_lock(duration);
}

static lcb_STATUS get_validate(lcb_INSTANCE *instance, const lcb_CMDGET *cmd)
{
    if (cmd->key().empty()) {
        return LCB_ERR_EMPTY_KEY;
    }
    if (!LCBT_SETTING(instance, use_collections) && !cmd->collection().is_default_collection()) {
        /* only allow default collection when collections disabled for the instance */
        return LCB_ERR_SDK_FEATURE_UNAVAILABLE;
    }

    return LCB_SUCCESS;
}

static lcb_STATUS get_schedule(lcb_INSTANCE *instance, std::shared_ptr<lcb_CMDGET> cmd)
{
    mc_PIPELINE *pl;
    mc_PACKET *pkt;
    mc_REQDATA *rdata;
    mc_CMDQUEUE *q = &instance->cmdq;
    lcb_uint8_t extlen = 0;
    lcb_uint8_t opcode = PROTOCOL_BINARY_CMD_GET;
    protocol_binary_request_gat gcmd;
    protocol_binary_request_header *hdr = &gcmd.message.header;
    lcb_STATUS err;

    hdr->request.magic = PROTOCOL_BINARY_REQ;
    if (cmd->with_lock()) {
        extlen = 4;
        opcode = PROTOCOL_BINARY_CMD_GET_LOCKED;
    } else if (cmd->with_touch()) {
        extlen = 4;
        opcode = PROTOCOL_BINARY_CMD_GAT;
    }

    lcb_KEYBUF keybuf{LCB_KV_COPY, {cmd->key().c_str(), cmd->key().size()}};
    err = mcreq_basic_packet(q, &keybuf, cmd->collection().collection_id(), hdr, extlen, 0, &pkt, &pl,
                             MCREQ_BASICPACKET_F_FALLBACKOK);
    if (err != LCB_SUCCESS) {
        return err;
    }

    rdata = &pkt->u_rdata.reqdata;
    rdata->cookie = cmd->cookie();
    rdata->start = cmd->start_time_or_default_in_nanoseconds(gethrtime());
    rdata->deadline =
        rdata->start + cmd->timeout_or_default_in_nanoseconds(LCB_US2NS(LCBT_SETTING(instance, operation_timeout)));

    hdr->request.opcode = opcode;
    hdr->request.datatype = PROTOCOL_BINARY_RAW_BYTES;
    hdr->request.bodylen = htonl(extlen + ntohs(hdr->request.keylen));
    hdr->request.opaque = pkt->opaque;
    hdr->request.cas = 0;

    if (cmd->with_lock()) {
        gcmd.message.body.norm.expiration = htonl(cmd->lock_time());
    }
    if (extlen) {
        gcmd.message.body.norm.expiration = htonl(cmd->expiry());
    }

    if (cmd->is_cookie_callback()) {
        pkt->flags |= MCREQ_F_PRIVCALLBACK;
    }

    memcpy(SPAN_BUFFER(&pkt->kh_span), gcmd.bytes, MCREQ_PKT_BASESIZE + extlen);
    LCB_SCHED_ADD(instance, pl, pkt)
    if (instance->settings->tracer) {
        lcbtrace_REF ref{LCBTRACE_REF_CHILD_OF, cmd->parent_span()};
        auto operation_id = std::to_string(pkt->opaque);
        rdata->span = lcbtrace_span_start(instance->settings->tracer, LCBTRACE_OP_GET, LCBTRACE_NOW, &ref);
        lcbtrace_span_add_tag_str(rdata->span, LCBTRACE_TAG_OPERATION_ID, operation_id.c_str());
        lcbtrace_span_add_system_tags(rdata->span, instance->settings, LCBTRACE_TAG_SERVICE_KV);
    }
    TRACE_GET_BEGIN(instance, hdr, cmd)
    return LCB_SUCCESS;
}

static lcb_STATUS get_execute(lcb_INSTANCE *instance, std::shared_ptr<lcb_CMDGET> cmd)
{
    if (!LCBT_SETTING(instance, use_collections)) {
        /* fast path if collections are not enabled */
        return get_schedule(instance, cmd);
    }

    if (collcache_get(instance, cmd->collection()) == LCB_SUCCESS) {
        return get_schedule(instance, cmd);
    }

    return collcache_resolve(
        instance, cmd,
        [instance](lcb_STATUS status, const lcb_RESPGETCID *resp, std::shared_ptr<lcb_CMDGET> operation) {
            const auto callback_type = LCB_CALLBACK_GET;
            lcb_RESPCALLBACK operation_callback = lcb_find_callback(instance, callback_type);
            lcb_RESPGET response{};
            response.ctx.key = operation->key();
            response.cookie = operation->cookie();
            if (status == LCB_ERR_SHEDULE_FAILURE || resp == nullptr) {
                response.ctx.rc = LCB_ERR_TIMEOUT;
                operation_callback(instance, callback_type, &response);
                return;
            }
            if (resp->ctx.rc != LCB_SUCCESS) {
                response.ctx = resp->ctx;
                operation_callback(instance, callback_type, &response);
                return;
            }
            response.ctx.rc = get_schedule(instance, operation);
            if (response.ctx.rc != LCB_SUCCESS) {
                operation_callback(instance, callback_type, &response);
            }
        });
}

LIBCOUCHBASE_API
lcb_STATUS lcb_get(lcb_INSTANCE *instance, void *cookie, const lcb_CMDGET *command)
{
    lcb_STATUS rc;

    rc = get_validate(instance, command);
    if (rc != LCB_SUCCESS) {
        return rc;
    }

    auto cmd = std::make_shared<lcb_CMDGET>(*command);
    cmd->cookie(cookie);

    if (instance->cmdq.config == nullptr) {
        cmd->start_time_in_nanoseconds(gethrtime());
        return lcb::defer_operation(instance, [instance, cmd](lcb_STATUS status) {
            const auto callback_type = LCB_CALLBACK_GET;
            lcb_RESPCALLBACK operation_callback = lcb_find_callback(instance, callback_type);
            lcb_RESPGET response{};
            response.ctx.key = cmd->key();
            response.cookie = cmd->cookie();
            if (status == LCB_ERR_REQUEST_CANCELED) {
                response.ctx.rc = status;
                operation_callback(instance, callback_type, &response);
                return;
            }
            response.ctx.rc = get_execute(instance, cmd);
            if (response.ctx.rc != LCB_SUCCESS) {
                operation_callback(instance, callback_type, &response);
            }
        });
    }
    return get_execute(instance, cmd);
}

LIBCOUCHBASE_API lcb_STATUS lcb_respgetreplica_status(const lcb_RESPGETREPLICA *resp)
{
    return resp->ctx.rc;
}

LIBCOUCHBASE_API lcb_STATUS lcb_respgetreplica_error_context(const lcb_RESPGETREPLICA *resp,
                                                             const lcb_KEY_VALUE_ERROR_CONTEXT **ctx)
{
    *ctx = &resp->ctx;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_respgetreplica_cookie(const lcb_RESPGETREPLICA *resp, void **cookie)
{
    *cookie = resp->cookie;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_respgetreplica_cas(const lcb_RESPGETREPLICA *resp, uint64_t *cas)
{
    *cas = resp->ctx.cas;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_respgetreplica_datatype(const lcb_RESPGETREPLICA *resp, uint8_t *datatype)
{
    *datatype = resp->datatype;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_respgetreplica_flags(const lcb_RESPGETREPLICA *resp, uint32_t *flags)
{
    *flags = resp->itmflags;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_respgetreplica_key(const lcb_RESPGETREPLICA *resp, const char **key, size_t *key_len)
{
    *key = resp->ctx.key.c_str();
    *key_len = resp->ctx.key.size();
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_respgetreplica_value(const lcb_RESPGETREPLICA *resp, const char **value,
                                                     size_t *value_len)
{
    *value = (const char *)resp->value;
    *value_len = resp->nvalue;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API int lcb_respgetreplica_is_final(const lcb_RESPGETREPLICA *resp)
{
    return resp->rflags & LCB_RESP_F_FINAL;
}

LIBCOUCHBASE_API lcb_STATUS lcb_cmdgetreplica_create(lcb_CMDGETREPLICA **cmd, lcb_REPLICA_MODE mode)
{
    lcb_CMDGETREPLICA *res = (lcb_CMDGETREPLICA *)calloc(1, sizeof(lcb_CMDGETREPLICA));
    switch (mode) {
        case LCB_REPLICA_MODE_ANY:
            res->strategy = LCB_REPLICA_FIRST;
            break;
        case LCB_REPLICA_MODE_ALL:
            res->strategy = LCB_REPLICA_ALL;
            break;
        case LCB_REPLICA_MODE_IDX0:
        case LCB_REPLICA_MODE_IDX1:
        case LCB_REPLICA_MODE_IDX2:
            res->strategy = LCB_REPLICA_SELECT;
            res->index = mode - LCB_REPLICA_MODE_IDX0;
            break;
        default:
            free(res);
            return LCB_ERR_INVALID_ARGUMENT;
    }
    *cmd = res;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_cmdgetreplica_clone(const lcb_CMDGETREPLICA *cmd, lcb_CMDGETREPLICA **copy)
{
    LCB_CMD_CLONE(lcb_CMDGETREPLICA, cmd, copy);
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_cmdgetreplica_destroy(lcb_CMDGETREPLICA *cmd)
{
    LCB_CMD_DESTROY_CLONE(cmd);
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_cmdgetreplica_timeout(lcb_CMDGETREPLICA *cmd, uint32_t timeout)
{
    cmd->timeout = timeout;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_cmdgetreplica_parent_span(lcb_CMDGETREPLICA *cmd, lcbtrace_SPAN *span)
{
    cmd->pspan = span;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_cmdgetreplica_collection(lcb_CMDGETREPLICA *cmd, const char *scope, size_t scope_len,
                                                         const char *collection, size_t collection_len)
{
    cmd->scope = scope;
    cmd->nscope = scope_len;
    cmd->collection = collection;
    cmd->ncollection = collection_len;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API lcb_STATUS lcb_cmdgetreplica_key(lcb_CMDGETREPLICA *cmd, const char *key, size_t key_len)
{
    LCB_CMD_SET_KEY(cmd, key, key_len);
    return LCB_SUCCESS;
}

struct RGetCookie : mc_REQDATAEX {
    RGetCookie(void *cookie, lcb_INSTANCE *instance, lcb_replica_t, int vb);
    void decref()
    {
        if (!--remaining) {
            delete this;
        }
    }

    unsigned r_cur;
    unsigned r_max;
    int remaining;
    int vbucket;
    lcb_replica_t strategy;
    lcb_INSTANCE *instance;
};

static void rget_dtor(mc_PACKET *pkt)
{
    static_cast<RGetCookie *>(pkt->u_rdata.exdata)->decref();
}

static void rget_callback(mc_PIPELINE *, mc_PACKET *pkt, lcb_STATUS err, const void *arg)
{
    RGetCookie *rck = static_cast<RGetCookie *>(pkt->u_rdata.exdata);
    lcb_RESPGETREPLICA *resp = reinterpret_cast<lcb_RESPGETREPLICA *>(const_cast<void *>(arg));
    lcb_RESPCALLBACK callback;
    lcb_INSTANCE *instance = rck->instance;

    callback = lcb_find_callback(instance, LCB_CALLBACK_GETREPLICA);

    /** Figure out what the strategy is.. */
    if (rck->strategy == LCB_REPLICA_SELECT || rck->strategy == LCB_REPLICA_ALL) {
        /** Simplest */
        if (rck->strategy == LCB_REPLICA_SELECT || rck->remaining == 1) {
            resp->rflags |= LCB_RESP_F_FINAL;
        }
        callback(instance, LCB_CALLBACK_GETREPLICA, (const lcb_RESPBASE *)resp);
    } else {
        mc_CMDQUEUE *cq = &instance->cmdq;
        mc_PIPELINE *nextpl = nullptr;

        /** FIRST */
        do {
            int nextix;
            rck->r_cur++;
            nextix = lcbvb_vbreplica(cq->config, rck->vbucket, rck->r_cur);
            if (nextix > -1 && nextix < (int)cq->npipelines) {
                /* have a valid next index? */
                nextpl = cq->pipelines[nextix];
                break;
            }
        } while (rck->r_cur < rck->r_max);

        if (err == LCB_SUCCESS || rck->r_cur == rck->r_max || nextpl == nullptr) {
            resp->rflags |= LCB_RESP_F_FINAL;
            callback(instance, LCB_CALLBACK_GETREPLICA, (lcb_RESPBASE *)resp);
            /* refcount=1 . Free this now */
            rck->remaining = 1;
        } else if (err != LCB_SUCCESS) {
            mc_PACKET *newpkt = mcreq_renew_packet(pkt);
            newpkt->flags &= ~MCREQ_STATE_FLAGS;
            mcreq_sched_add(nextpl, newpkt);
            /* Use this, rather than lcb_sched_leave(), because this is being
             * invoked internally by the library. */
            mcreq_sched_leave(cq, 1);
            /* wait */
            rck->remaining = 2;
        }
    }
    rck->decref();
}

static mc_REQDATAPROCS rget_procs = {rget_callback, rget_dtor};

RGetCookie::RGetCookie(void *cookie_, lcb_INSTANCE *instance_, lcb_replica_t strategy_, int vbucket_)
    : mc_REQDATAEX(cookie_, rget_procs, gethrtime()), r_cur(0), r_max(LCBT_NREPLICAS(instance_)), remaining(0),
      vbucket(vbucket_), strategy(strategy_), instance(instance_)
{
}

static lcb_STATUS getreplica_validate(lcb_INSTANCE *instance, const lcb_CMDGETREPLICA *cmd)
{
    auto err = lcb_is_collection_valid(instance, cmd->scope, cmd->nscope, cmd->collection, cmd->ncollection);
    if (err != LCB_SUCCESS) {
        return err;
    }
    if (LCB_KEYBUF_IS_EMPTY(&cmd->key)) {
        return LCB_ERR_EMPTY_KEY;
    }
    if (!instance->cmdq.config) {
        return LCB_ERR_NO_CONFIGURATION;
    }
    if (!LCBT_NREPLICAS(instance)) {
        return LCB_ERR_NO_MATCHING_SERVER;
    }

    mc_CMDQUEUE *cq = &instance->cmdq;
    int vbid, ixtmp;
    unsigned r0, r1 = 0;

    mcreq_map_key(cq, &cmd->key, MCREQ_PKT_BASESIZE, &vbid, &ixtmp);
    if (cmd->strategy == LCB_REPLICA_SELECT) {
        r0 = r1 = cmd->index;
        int idx = lcbvb_vbreplica(cq->config, vbid, r0);
        if (idx < 0) {
            return LCB_ERR_NO_MATCHING_SERVER;
        }

    } else if (cmd->strategy == LCB_REPLICA_ALL) {
        unsigned ii;
        r0 = 0;
        r1 = LCBT_NREPLICAS(instance);
        /* Make sure they're all online */
        for (ii = 0; ii < LCBT_NREPLICAS(instance); ii++) {
            if (lcbvb_vbreplica(cq->config, vbid, ii) < 0) {
                return LCB_ERR_NO_MATCHING_SERVER;
            }
        }
    } else {
        for (r0 = 0; r0 < LCBT_NREPLICAS(instance); r0++) {
            if (lcbvb_vbreplica(cq->config, vbid, r0) > -1) {
                r1 = r0;
                break;
            }
        }
        if (r0 == LCBT_NREPLICAS(instance)) {
            return LCB_ERR_NO_MATCHING_SERVER;
        }
    }

    if (r1 < r0 || r1 >= cq->npipelines) {
        return LCB_ERR_NO_MATCHING_SERVER;
    }

    return LCB_SUCCESS;
}

LIBCOUCHBASE_API
lcb_STATUS lcb_getreplica(lcb_INSTANCE *instance, void *cookie, const lcb_CMDGETREPLICA *command)
{
    lcb_STATUS rc;

    rc = getreplica_validate(instance, command);
    if (rc != LCB_SUCCESS) {
        return rc;
    }

    auto operation = [instance, cookie](const lcb_RESPGETCID *resp, const lcb_CMDGETREPLICA *cmd) {
        if (resp && resp->ctx.rc != LCB_SUCCESS) {
            lcb_RESPCALLBACK cb = lcb_find_callback(instance, LCB_CALLBACK_GETREPLICA);
            lcb_RESPGETREPLICA rget{};
            rget.ctx = resp->ctx;
            rget.ctx.key.assign(static_cast<const char *>(cmd->key.contig.bytes), cmd->key.contig.nbytes);
            rget.cookie = cookie;
            cb(instance, LCB_CALLBACK_GETREPLICA, reinterpret_cast<const lcb_RESPBASE *>(&rget));
            return resp->ctx.rc;
        }
        /**
         * Because we need to direct these commands to specific servers, we can't
         * just use the 'basic_packet()' function.
         */
        mc_CMDQUEUE *cq = &instance->cmdq;
        int vbid, ixtmp;
        protocol_binary_request_header req;
        unsigned r0, r1 = 0;

        mcreq_map_key(cq, &cmd->key, MCREQ_PKT_BASESIZE, &vbid, &ixtmp);

        /* The following blocks will also validate that the entire index range is
         * valid. This is in order to ensure that we don't allocate the cookie
         * if there aren't enough replicas online to satisfy the requirements */

        if (cmd->strategy == LCB_REPLICA_SELECT) {
            r0 = r1 = cmd->index;
            if ((ixtmp = lcbvb_vbreplica(cq->config, vbid, r0)) < 0) {
                return LCB_ERR_NO_MATCHING_SERVER;
            }

        } else if (cmd->strategy == LCB_REPLICA_ALL) {
            r0 = 0;
            r1 = LCBT_NREPLICAS(instance);
            /* Make sure they're all online */
            for (unsigned ii = 0; ii < LCBT_NREPLICAS(instance); ii++) {
                if ((ixtmp = lcbvb_vbreplica(cq->config, vbid, ii)) < 0) {
                    return LCB_ERR_NO_MATCHING_SERVER;
                }
            }
        } else {
            for (r0 = 0; r0 < LCBT_NREPLICAS(instance); r0++) {
                if ((ixtmp = lcbvb_vbreplica(cq->config, vbid, r0)) > -1) {
                    r1 = r0;
                    break;
                }
            }
            if (r0 == LCBT_NREPLICAS(instance)) {
                return LCB_ERR_NO_MATCHING_SERVER;
            }
        }

        if (r1 < r0 || r1 >= cq->npipelines) {
            return LCB_ERR_NO_MATCHING_SERVER;
        }

        /* Initialize the cookie */
        RGetCookie *rck = new RGetCookie(cookie, instance, cmd->strategy, vbid);
        rck->deadline = rck->start + LCB_US2NS(cmd->timeout ? cmd->timeout : LCBT_SETTING(instance, operation_timeout));

        /* Initialize the packet */
        req.request.magic = PROTOCOL_BINARY_REQ;
        req.request.opcode = PROTOCOL_BINARY_CMD_GET_REPLICA;
        req.request.datatype = PROTOCOL_BINARY_RAW_BYTES;
        req.request.vbucket = htons((lcb_uint16_t)vbid);
        req.request.cas = 0;
        req.request.extlen = 0;

        rck->r_cur = r0;
        do {
            int curix;
            mc_PIPELINE *pl;
            mc_PACKET *pkt;

            curix = lcbvb_vbreplica(cq->config, vbid, r0);
            /* XXX: this is always expected to be in range. For the FIRST mode
             * it will seek to the first valid index (checked above), and for the
             * ALL mode, it will fail if not all replicas are already online
             * (also checked above) */
            pl = cq->pipelines[curix];
            pkt = mcreq_allocate_packet(pl);
            if (!pkt) {
                delete rck;
                return LCB_ERR_NO_MEMORY;
            }

            pkt->u_rdata.exdata = rck;
            pkt->flags |= MCREQ_F_REQEXT;

            mcreq_reserve_key(pl, pkt, sizeof(req.bytes), &cmd->key, cmd->cid);
            size_t nkey = pkt->kh_span.size - MCREQ_PKT_BASESIZE + pkt->extlen;
            req.request.keylen = htons((uint16_t)nkey);
            req.request.bodylen = htonl((uint32_t)nkey);
            req.request.opaque = pkt->opaque;
            rck->remaining++;
            mcreq_write_hdr(pkt, &req);
            mcreq_sched_add(pl, pkt);
        } while (++r0 < r1);

        MAYBE_SCHEDLEAVE(instance);

        return LCB_SUCCESS;
    };

    if (!LCBT_SETTING(instance, use_collections)) {
        /* fast path if collections are not enabled */
        return operation(nullptr, command);
    }

    uint32_t cid = 0;
    if (collcache_get(instance, command->scope, command->nscope, command->collection, command->ncollection, &cid) ==
        LCB_SUCCESS) {
        lcb_CMDGETREPLICA clone = *command; /* shallow clone */
        clone.cid = cid;
        return operation(nullptr, &clone);
    } else {
        return collcache_resolve(instance, command, operation, lcb_cmdgetreplica_clone, lcb_cmdgetreplica_destroy);
    }
}
