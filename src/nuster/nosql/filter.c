/*
 * Cache filter related variables and functions.
 *
 * Copyright (C) [Jiang Wenyuan](https://github.com/jiangwenyuan), < koubunen AT gmail DOT com >
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <common/cfgparse.h>
#include <common/standard.h>

#include <proto/filters.h>
#include <proto/log.h>
#include <proto/stream.h>
#include <proto/proto_http.h>
#include <proto/stream_interface.h>

#include <nuster/nuster.h>
#include <nuster/nosql.h>

static int _nst_nosql_filter_init(struct proxy *px, struct flt_conf *fconf) {
    return 0;
}

static void _nst_nosql_filter_deinit(struct proxy *px, struct flt_conf *fconf) {
}

static int _nst_nosql_filter_check(struct proxy *px, struct flt_conf *fconf) {
    return 0;
}

static int _nst_nosql_filter_attach(struct stream *s, struct filter *filter) {
    if(global.nuster.nosql.status != NUSTER_STATUS_ON) {
        return 0;
    }
    if(!filter->ctx) {
        struct nst_nosql_ctx *ctx = pool_alloc(global.nuster.nosql.pool.ctx);
        if(ctx == NULL ) {
            return 0;
        }
        ctx->state   = NST_NOSQL_CTX_STATE_INIT;
        ctx->rule    = NULL;
        ctx->stash   = NULL;
        ctx->entry   = NULL;
        ctx->data    = NULL;
        ctx->element = NULL;
        ctx->pid     = -1;
        filter->ctx  = ctx;
    }
    register_data_filter(s, &s->req, filter);
    //register_data_filter(s, &s->res, filter);
    return 1;
}

static void _nst_nosql_filter_detach(struct stream *s, struct filter *filter) {
}

static int _nst_nosql_filter_http_headers(struct stream *s, struct filter *filter,
        struct http_msg *msg) {
    struct stream_interface *si = &s->si[1];
    struct nst_nosql_ctx *ctx   = filter->ctx;
    struct nuster_rule *rule    = NULL;
    struct proxy *px            = s->be;
    char *key                   = NULL;
    uint64_t hash               = 0;
    struct appctx *appctx       = si_appctx(si);
    struct http_txn *txn        = s->txn;

    if((msg->chn->flags & CF_ISRESP)) {
        return 1;
    }

    if(txn->meth != HTTP_METH_GET && txn->meth != HTTP_METH_POST) {
        appctx->st0 = NST_NOSQL_APPCTX_STATE_ERROR_NOT_ALLOWED;
        return 0;
    }

    if(ctx->state == NST_NOSQL_CTX_STATE_INIT) {
        if(!nst_nosql_prebuild_key(ctx, s, msg)) {
            appctx->st0 = NST_NOSQL_APPCTX_STATE_ERROR;
            return 0;
        }

        list_for_each_entry(rule, &px->nuster.rules, list) {
            nuster_debug("[NOSQL] Checking rule: %s\n", rule->name);
            /* build key */
            key = nst_nosql_build_key(ctx, rule->key, s, msg);
            if(!key) {
                appctx->st0 = NST_NOSQL_APPCTX_STATE_ERROR;
                return 0;
            }
            nuster_debug("[NOSQL] Got key: %s\n", key);
            hash = nst_nosql_hash_key(key);

            if(s->txn->meth == HTTP_METH_GET) {
                ctx->data = nst_nosql_exists(key, hash);
                if(ctx->data) {
                    nuster_debug("EXIST\n[NOSQL] Hit\n");
                    /* OK, nosql exists */
                    appctx->st0 = NST_NOSQL_APPCTX_STATE_HIT;
                    appctx->ctx.nuster.nosql_engine.data = ctx->data;
                    appctx->ctx.nuster.nosql_engine.element = ctx->data->element;
                    return 1;
                }

                nuster_debug("NOT EXIST\n");
                appctx->st0 = NST_NOSQL_APPCTX_STATE_NOT_FOUND;
                return 0;
            } else {
                nuster_debug("[NOSQL] Checking if rule pass: ");
                if(nst_cache_test_rule(rule, s, msg->chn->flags & CF_ISRESP)) {
                    nuster_debug("PASS\n");
                    nst_nosql_create(ctx, key, hash);
                    break;
                }
                nuster_debug("FAIL\n");
            }
        }
    }

    if(ctx->state == NST_NOSQL_CTX_STATE_WAIT) {
        appctx->st0 = NST_NOSQL_APPCTX_STATE_WAIT;
        return 0;
    }

    if(ctx->state == NST_NOSQL_CTX_STATE_INVALID) {
        appctx->st0 = NST_NOSQL_APPCTX_STATE_ERROR;
        return 0;
    }

    return 1;
}

static int _nst_nosql_filter_http_forward_data(struct stream *s, struct filter *filter,
        struct http_msg *msg, unsigned int len) {

    struct stream_interface *si = &s->si[1];
    struct appctx *appctx       = si_appctx(si);
    struct nst_nosql_ctx *ctx   = filter->ctx;

    if(ctx->state == NST_NOSQL_CTX_STATE_CREATE && !(msg->chn->flags & CF_ISRESP)) {
        if(!nst_nosql_update(ctx, msg, len)) {
            ctx->entry->state = NST_NOSQL_ENTRY_STATE_INVALID;
            appctx->st0       = NST_NOSQL_APPCTX_STATE_ERROR;
        }
    }
    return len;
}

static int _nst_nosql_filter_http_end(struct stream *s, struct filter *filter,
        struct http_msg *msg) {
    struct nst_nosql_ctx *ctx = filter->ctx;

    if(ctx->state == NST_NOSQL_CTX_STATE_CREATE && (msg->chn->flags & CF_ISRESP)) {
        nst_nosql_finish(ctx);
    }
    return 1;
}

struct flt_ops nst_nosql_filter_ops = {
    /* Manage cache filter, called for each filter declaration */
    .init   = _nst_nosql_filter_init,
    .deinit = _nst_nosql_filter_deinit,
    .check  = _nst_nosql_filter_check,

    .attach = _nst_nosql_filter_attach,
    .detach = _nst_nosql_filter_detach,

    /* Filter HTTP requests and responses */
    .http_headers      = _nst_nosql_filter_http_headers,
    .http_forward_data = _nst_nosql_filter_http_forward_data,
    .http_end          = _nst_nosql_filter_http_end,

};
