/*
 * nuster cache engine functions.
 *
 * Copyright (C) [Jiang Wenyuan](https://github.com/jiangwenyuan), < koubunen AT gmail DOT com >
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <proto/log.h>
#include <proto/proto_http.h>
#include <proto/raw_sock.h>
#include <proto/stream_interface.h>
#include <proto/acl.h>
#include <proto/proxy.h>

#ifdef USE_OPENSSL
#include <proto/ssl_sock.h>
#include <types/ssl_sock.h>
#endif

#include <nuster/memory.h>
#include <nuster/shctx.h>
#include <nuster/nuster.h>
#include <nuster/http.h>
#include <nuster/file.h>

static char key_holder[] = {0x1A, '\0'};
static char key_rs[]     = {0x1E, '\0'};
static char key_us[]     = {0x1F, '\0'};

/*
 * The cache applet acts like the backend to send cached http data
 */
static void nst_cache_engine_handler(struct appctx *appctx) {
    struct nst_cache_element *element = NULL;
    struct stream_interface *si       = appctx->owner;
    struct channel *res               = si_ic(si);
    /* struct stream *s                  = si_strm(si); */
    int ret;

    if(unlikely(si->state == SI_ST_DIS || si->state == SI_ST_CLO)) {
        appctx->ctx.nuster.cache_engine.data->clients--;
        return;
    }

    /* Check if the input buffer is avalaible. */
    if(res->buf.size == 0) {
        si_rx_room_blk(si);
        return;
    }

    /* check that the output is not closed */
    if(res->flags & (CF_SHUTW|CF_SHUTW_NOW)) {
        appctx->ctx.nuster.cache_engine.element = NULL;
    }

    if(appctx->ctx.nuster.cache_engine.element) {
        /*
        if(appctx->ctx.nuster.cache_engine.element == appctx->ctx.nuster.cache_engine.data->element) {
            s->res.analysers = 0;
            s->res.analysers |= (AN_RES_WAIT_HTTP | AN_RES_HTTP_PROCESS_BE | AN_RES_HTTP_XFER_BODY);
        }
        */
        element = appctx->ctx.nuster.cache_engine.element;

        ret = ci_putblk(res, element->msg.data, element->msg.len);
        if(ret >= 0) {
            appctx->ctx.nuster.cache_engine.element = element->next;
        } else if(ret == -2) {
            appctx->ctx.nuster.cache_engine.data->clients--;
            si_shutr(si);
            res->flags |= CF_READ_NULL;
        }
    } else {
        co_skip(si_oc(si), co_data(si_oc(si)));
        si_shutr(si);
        res->flags |= CF_READ_NULL;
        appctx->ctx.nuster.cache_engine.data->clients--;
    }
}

/*
 * Cache the keys which calculated in request for response use
 */
struct nuster_rule_stash *nst_cache_stash_rule(struct nst_cache_ctx *ctx,
        struct nuster_rule *rule, char *key, uint64_t hash) {

    struct nuster_rule_stash *stash = pool_alloc(global.nuster.cache.pool.stash);

    if(stash) {
        stash->rule = rule;
        stash->key  = key;
        stash->hash = hash;
        if(ctx->stash) {
            stash->next = ctx->stash;
        } else {
            stash->next = NULL;
        }
        ctx->stash = stash;
    }
    return stash;
}

static char *_string_append(char *dst, int *dst_len, int *dst_size,
        char *src, int src_len) {

    int left     = *dst_size - *dst_len;
    int need     = src_len + 1;
    int old_size = *dst_size;

    if(left < need) {
        *dst_size += ((need - left) / NST_CACHE_DEFAULT_KEY_SIZE + 1)  * NST_CACHE_DEFAULT_KEY_SIZE;
    }

    if(old_size != *dst_size) {
        char *new_dst = realloc(dst, *dst_size);
        if(!new_dst) {
            free(dst);
            return NULL;
        }
        dst = new_dst;
    }

    memcpy(dst + *dst_len, src, src_len);
    *dst_len += src_len;
    dst[*dst_len] = '\0';
    return dst;
}

/*
 * TODO: rewrite in h19 version
 */
static char *_nst_cache_key_append(char *dst, int *dst_len, int *dst_size,
        char *src, int src_len) {
    char *key = _string_append(dst, dst_len, dst_size, src, src_len);
    if(key) {
        return _string_append(key, dst_len, dst_size, key_rs, 1);
    }
    return NULL;
}

static char *_nst_cache_key_append2(char *dst, int *dst_len, int *dst_size,
        char *src, int src_len) {
    char *key = _string_append(dst, dst_len, dst_size, src, src_len);
    if(key) {
        return _string_append(key, dst_len, dst_size, key_us, 1);
    }
    return NULL;
}

void *nst_cache_memory_alloc(struct pool_head *pool, int size) {
    if(global.nuster.cache.share) {
        return nuster_memory_alloc(global.nuster.cache.memory, size);
    } else {
        return pool_alloc(pool);
    }
}

void nst_cache_memory_free(struct pool_head *pool, void *p) {
    if(global.nuster.cache.share) {
        return nuster_memory_free(global.nuster.cache.memory, p);
    } else {
        return pool_free(pool, p);
    }
}

int nst_cache_check_uri(struct http_msg *msg) {
    const char *uri = ci_head(msg->chn) + msg->sl.rq.u;

    if(!global.nuster.cache.uri) {
        return 0;
    }

    if(strlen(global.nuster.cache.uri) != msg->sl.rq.u_l) {
        return 0;
    }

    if(memcmp(uri, global.nuster.cache.uri, msg->sl.rq.u_l) != 0) {
        return 0;
    }

    return 1;
}

/*
 * create a new nst_cache_data and insert it to cache->data list
 */
struct nst_cache_data *nst_cache_data_new() {

    struct nst_cache_data *data = nst_cache_memory_alloc(global.nuster.cache.pool.data, sizeof(*data));

    nuster_shctx_lock(nuster.cache);
    if(data) {
        data->clients  = 0;
        data->invalid  = 0;
        data->element  = NULL;

        if(nuster.cache->data_head == NULL) {
            nuster.cache->data_head = data;
            nuster.cache->data_tail = data;
            data->next              = data;
        } else {
            if(nuster.cache->data_head == nuster.cache->data_tail) {
                nuster.cache->data_head->next = data;
                data->next                    = nuster.cache->data_head;
                nuster.cache->data_tail       = data;
            } else {
                data->next                    = nuster.cache->data_head;
                nuster.cache->data_tail->next = data;
                nuster.cache->data_tail       = data;
            }
        }
    }
    nuster_shctx_unlock(nuster.cache);
    return data;
}

/*
 * Append partial http response data
 */
static struct nst_cache_element *_nst_cache_data_append(struct http_msg *msg, long msg_len) {

    struct nst_cache_element *element = nst_cache_memory_alloc(global.nuster.cache.pool.element, sizeof(*element));

    if(element) {
        char *data = b_orig(&msg->chn->buf);
        char *p    = ci_head(msg->chn);
        int size   = msg->chn->buf.size;

        char *msg_data = nst_cache_memory_alloc(global.nuster.cache.pool.chunk, msg_len);
        if(!msg_data) {
            nst_cache_memory_free(global.nuster.cache.pool.element, element);
            return NULL;
        }

        if(p - data + msg_len > size) {
            int right = data + size - p;
            int left  = msg_len - right;
            memcpy(msg_data, p, right);
            memcpy(msg_data + right, data, left);
        } else {
            memcpy(msg_data, p, msg_len);
        }

        element->msg.data = msg_data;
        element->msg.len  = msg_len;
        element->next     = NULL;
        nst_cache_stats_update_used_mem(msg_len);
    }
    return element;
}


static int _nst_cache_data_invalid(struct nst_cache_data *data) {
    if(data->invalid) {
        if(!data->clients) {
            return 1;
        }
    }
    return 0;
}

/*
 * free invalid nst_cache_data
 */
static void _nst_cache_data_cleanup() {
    struct nst_cache_data *data = NULL;

    if(nuster.cache->data_head) {
        if(nuster.cache->data_head == nuster.cache->data_tail) {
            if(_nst_cache_data_invalid(nuster.cache->data_head)) {
                data                    = nuster.cache->data_head;
                nuster.cache->data_head = NULL;
                nuster.cache->data_tail = NULL;
            }
        } else {
            if(_nst_cache_data_invalid(nuster.cache->data_head)) {
                data                          = nuster.cache->data_head;
                nuster.cache->data_tail->next = nuster.cache->data_head->next;
                nuster.cache->data_head       = nuster.cache->data_head->next;
            } else {
                nuster.cache->data_tail = nuster.cache->data_head;
                nuster.cache->data_head = nuster.cache->data_head->next;
            }
        }
    }

    if(data) {
        struct nst_cache_element *element = data->element;
        while(element) {
            struct nst_cache_element *tmp = element;
            element                       = element->next;

            if(tmp->msg.data) {
                nst_cache_stats_update_used_mem(-tmp->msg.len);
                nst_cache_memory_free(global.nuster.cache.pool.chunk, tmp->msg.data);
            }
            nst_cache_memory_free(global.nuster.cache.pool.element, tmp);
        }
        nst_cache_memory_free(global.nuster.cache.pool.data, data);
    }
}

void nst_cache_housekeeping() {
    if(global.nuster.cache.status == NUSTER_STATUS_ON) {
        nst_cache_dict_rehash();
        nuster_shctx_lock(&nuster.cache->dict[0]);
        nst_cache_dict_cleanup();
        nuster_shctx_unlock(&nuster.cache->dict[0]);
        nuster_shctx_lock(nuster.cache);
        _nst_cache_data_cleanup();
        nuster_shctx_unlock(nuster.cache);
    }
}

void nst_cache_init() {

    nuster.applet.cache_engine.fct = nst_cache_engine_handler;

    if(global.nuster.cache.status == NUSTER_STATUS_ON) {
        if(global.nuster.cache.share == NUSTER_STATUS_UNDEFINED) {
            if(global.nbproc == 1) {
                global.nuster.cache.share = NUSTER_STATUS_OFF;
            } else {
                global.nuster.cache.share = NUSTER_STATUS_ON;
            }
        }

        if(global.nuster.cache.directory) {
            if(!nuster_create_path(global.nuster.cache.directory)) {
                ha_alert("Create `%s` failed\n", global.nuster.cache.directory);
                exit(1);
            }
            global.nuster.cache.pool.ctx   = create_pool("cp.path", strlen(global.nuster.cache.directory) + strlen("/0/00/") + 17, MEM_F_SHARED);
        }

        global.nuster.cache.pool.stash = create_pool("cp.stash", sizeof(struct nuster_rule_stash), MEM_F_SHARED);
        global.nuster.cache.pool.ctx   = create_pool("cp.ctx", sizeof(struct nst_cache_ctx), MEM_F_SHARED);

        if(global.nuster.cache.share) {
            global.nuster.cache.memory = nuster_memory_create("cache.shm", global.nuster.cache.dict_size + global.nuster.cache.data_size, global.tune.bufsize, NST_CACHE_DEFAULT_CHUNK_SIZE);
            if(!global.nuster.cache.memory) {
                goto shm_err;
            }
            if(!nuster_shctx_init(global.nuster.cache.memory)) {
                goto shm_err;
            }
            nuster.cache = nuster_memory_alloc(global.nuster.cache.memory, sizeof(struct nst_cache));
        } else {
            global.nuster.cache.memory = nuster_memory_create("cache.shm", NST_CACHE_DEFAULT_SIZE, 0, 0);
            if(!global.nuster.cache.memory) {
                goto shm_err;
            }
            if(!nuster_shctx_init(global.nuster.cache.memory)) {
                goto shm_err;
            }
            global.nuster.cache.pool.data    = create_pool("cp.data", sizeof(struct nst_cache_data), MEM_F_SHARED);
            global.nuster.cache.pool.element = create_pool("cp.element", sizeof(struct nst_cache_element), MEM_F_SHARED);
            global.nuster.cache.pool.chunk   = create_pool("cp.chunk", global.tune.bufsize, MEM_F_SHARED);
            global.nuster.cache.pool.entry   = create_pool("cp.entry", sizeof(struct nst_cache_entry), MEM_F_SHARED);

            nuster.cache = malloc(sizeof(struct nst_cache));
        }
        if(!nuster.cache) {
            goto err;
        }
        nuster.cache->dict[0].entry = NULL;
        nuster.cache->dict[0].used  = 0;
        nuster.cache->dict[1].entry = NULL;
        nuster.cache->dict[1].used  = 0;
        nuster.cache->data_head     = NULL;
        nuster.cache->data_tail     = NULL;
        nuster.cache->rehash_idx    = -1;
        nuster.cache->cleanup_idx   = 0;

        if(!nuster_shctx_init(nuster.cache)) {
            goto shm_err;
        }

        if(!nst_cache_dict_init()) {
            goto err;
        }

        if(!nst_cache_stats_init()) {
            goto err;
        }

        if(!nst_cache_manager_init()) {
            goto err;
        }

        nuster_debug("[CACHE] on, data_size=%llu\n", global.nuster.cache.data_size);
    }
    return;
err:
    ha_alert("Out of memory when initializing cache.\n");
    exit(1);
shm_err:
    ha_alert("Error when initializing cache.\n");
    exit(1);
}

int nst_cache_prebuild_key(struct nst_cache_ctx *ctx, struct stream *s, struct http_msg *msg) {

    struct http_txn *txn = s->txn;

    char *uri_begin, *uri_end;
    struct hdr_ctx hdr;

    ctx->req.scheme = SCH_HTTP;
#ifdef USE_OPENSSL
    if(s->sess->listener->bind_conf->is_ssl) {
        ctx->req.scheme = SCH_HTTPS;
    }
#endif

    ctx->req.host.data = NULL;
    ctx->req.host.len  = 0;
    hdr.idx            = 0;
    if(http_find_header2("Host", 4, ci_head(msg->chn), &txn->hdr_idx, &hdr)) {
        ctx->req.host.data = nst_cache_memory_alloc(global.nuster.cache.pool.chunk, hdr.vlen);
        if(!ctx->req.host.data) {
            return 0;
        }
        ctx->req.host.len  = hdr.vlen;
        memcpy(ctx->req.host.data, hdr.line + hdr.val, hdr.vlen);
    }

    uri_begin          = http_txn_get_path(txn);
    uri_end            = NULL;
    ctx->req.path.data = NULL;
    ctx->req.path.len  = 0;
    ctx->req.uri.data  = NULL;
    ctx->req.uri.len   = 0;
    if(uri_begin) {
        char *ptr = uri_begin;
        uri_end   = ci_head(msg->chn) + msg->sl.rq.u + msg->sl.rq.u_l;
        while(ptr < uri_end && *ptr != '?') {
            ptr++;
        }
        ctx->req.path.len = ptr - uri_begin;
        ctx->req.uri.data = uri_begin;
        ctx->req.uri.len  = uri_end - uri_begin;

        /* extra 1 char as required by regex_exec_match2 */
        ctx->req.path.data = nst_cache_memory_alloc(global.nuster.cache.pool.chunk, ctx->req.path.len + 1);
        if(!ctx->req.path.data) {
            return 0;
        }
        memcpy(ctx->req.path.data, uri_begin, ctx->req.path.len);
    }

    ctx->req.query.data = NULL;
    ctx->req.query.len  = 0;
    ctx->req.delimiter  = 0;
    if(ctx->req.uri.data) {
        ctx->req.query.data = memchr(ctx->req.uri.data, '?', uri_end - ctx->req.uri.data);
        if(ctx->req.query.data) {
            ctx->req.query.data++;
            ctx->req.query.len = uri_end - ctx->req.query.data;
            if(ctx->req.query.len) {
                ctx->req.delimiter = 1;
            }
        }
    }

    ctx->req.cookie.data = NULL;
    ctx->req.cookie.len  = 0;
    hdr.idx              = 0;
    if(http_find_header2("Cookie", 6, ci_head(msg->chn), &txn->hdr_idx, &hdr)) {
        ctx->req.cookie.data = hdr.line + hdr.val;
        ctx->req.cookie.len  = hdr.vlen;
    }

    return 1;
}

/*
 * TODO: rewrite in h19 version
 */
char *nst_cache_build_key(struct nst_cache_ctx *ctx, struct nuster_rule_key **pck, struct stream *s,
        struct http_msg *msg) {

    struct http_txn *txn = s->txn;

    struct hdr_ctx hdr;

    struct nuster_rule_key *ck = NULL;
    int key_len          = 0;
    int key_size         = NST_CACHE_DEFAULT_KEY_SIZE;
    char *key            = malloc(key_size);
    if(!key) {
        return NULL;
    }

    nuster_debug("[CACHE] Calculate key: ");
    while((ck = *pck++)) {
        switch(ck->type) {
            case NUSTER_RULE_KEY_METHOD:
                nuster_debug("method.");
                key = _nst_cache_key_append(key, &key_len, &key_size, http_known_methods[txn->meth].ptr, http_known_methods[txn->meth].len);
                break;
            case NUSTER_RULE_KEY_SCHEME:
                nuster_debug("scheme.");
                key = _nst_cache_key_append(key, &key_len, &key_size, ctx->req.scheme == SCH_HTTPS ? "HTTPS" : "HTTP", ctx->req.scheme == SCH_HTTPS ? 5 : 4);
                break;
            case NUSTER_RULE_KEY_HOST:
                nuster_debug("host.");
                if(ctx->req.host.data) {
                    key = _nst_cache_key_append(key, &key_len, &key_size, ctx->req.host.data, ctx->req.host.len);
                } else {
                    key = _nst_cache_key_append(key, &key_len, &key_size, key_holder, 1);
                }
                break;
            case NUSTER_RULE_KEY_URI:
                nuster_debug("uri.");
                if(ctx->req.uri.data) {
                    key = _nst_cache_key_append(key, &key_len, &key_size, ctx->req.uri.data, ctx->req.uri.len);
                } else {
                    key = _nst_cache_key_append(key, &key_len, &key_size, key_holder, 1);
                }
                break;
            case NUSTER_RULE_KEY_PATH:
                nuster_debug("path.");
                if(ctx->req.path.data) {
                    key = _nst_cache_key_append(key, &key_len, &key_size, ctx->req.path.data, ctx->req.path.len);
                } else {
                    key = _nst_cache_key_append(key, &key_len, &key_size, key_holder, 1);
                }
                break;
            case NUSTER_RULE_KEY_DELIMITER:
                nuster_debug("delimiter.");
                if(ctx->req.delimiter) {
                    key = _nst_cache_key_append(key, &key_len, &key_size, "?", 1);
                } else {
                    key = _nst_cache_key_append(key, &key_len, &key_size, key_holder, 1);
                }
                break;
            case NUSTER_RULE_KEY_QUERY:
                nuster_debug("query.");
                if(ctx->req.query.data && ctx->req.query.len) {
                    key = _nst_cache_key_append(key, &key_len, &key_size, ctx->req.query.data, ctx->req.query.len);
                } else {
                    key = _nst_cache_key_append(key, &key_len, &key_size, key_holder, 1);
                }
                break;
            case NUSTER_RULE_KEY_PARAM:
                nuster_debug("param_%s.", ck->data);
                if(ctx->req.query.data && ctx->req.query.len) {
                    char *v = NULL;
                    int v_l = 0;
                    if(nuster_req_find_param(ctx->req.query.data, ctx->req.query.data + ctx->req.query.len, ck->data, &v, &v_l)) {
                        key = _nst_cache_key_append(key, &key_len, &key_size, v, v_l);
                    } else {
                        key = _nst_cache_key_append(key, &key_len, &key_size, key_holder, 1);
                    }
                } else {
                    key = _nst_cache_key_append(key, &key_len, &key_size, key_holder, 1);
                }
                break;
            case NUSTER_RULE_KEY_HEADER:
                hdr.idx = 0;
                nuster_debug("header_%s.", ck->data);
                while (http_find_header2(ck->data, strlen(ck->data), ci_head(msg->chn), &txn->hdr_idx, &hdr)) {
                    key = _nst_cache_key_append2(key, &key_len, &key_size, hdr.line + hdr.val, hdr.vlen);
                }
                if(hdr.idx == 0) {
                    key = _nst_cache_key_append(key, &key_len, &key_size, key_holder, 1);
                } else {
                    key = _string_append(key, &key_len, &key_size, key_rs, 1);
                }
                break;
            case NUSTER_RULE_KEY_COOKIE:
                nuster_debug("cookie_%s.", ck->data);
                if(ctx->req.cookie.data) {
                    char *v = NULL;
                    size_t v_l = 0;
                    if(http_extract_cookie_value(ctx->req.cookie.data, ctx->req.cookie.data + ctx->req.cookie.len, ck->data, strlen(ck->data), 1, &v, &v_l)) {
                        key = _nst_cache_key_append(key, &key_len, &key_size, v, v_l);
                    } else {
                        key = _nst_cache_key_append(key, &key_len, &key_size, key_holder, 1);
                    }
                } else {
                    key = _nst_cache_key_append(key, &key_len, &key_size, key_holder, 1);
                }
                break;
            case NUSTER_RULE_KEY_BODY:
                nuster_debug("body.");
                if(txn->meth == HTTP_METH_POST || txn->meth == HTTP_METH_PUT) {
                    if((s->be->options & PR_O_WREQ_BODY) && ci_data(msg->chn) - msg->sov > 0) {
                        key = _nst_cache_key_append(key, &key_len, &key_size, ci_head(msg->chn) + msg->sov, ci_data(msg->chn) - msg->sov);
                    } else {
                        key = _nst_cache_key_append(key, &key_len, &key_size, key_holder, 1);
                    }
                }
                break;
            default:
                break;
        }
        if(!key) return NULL;
    }
    nuster_debug("\n");
    return key;
}

char *nst_cache_build_purge_key(struct stream *s, struct http_msg *msg) {
    struct http_txn *txn = s->txn;
    int https;
    char *path_beg, *url_end;
    struct hdr_ctx ctx;
    int key_len  = 0;

    /* method.scheme.host.uri */
    int key_size = NST_CACHE_DEFAULT_KEY_SIZE;
    char *key    = malloc(key_size);
    if(!key) {
        return NULL;
    }

    key = _nst_cache_key_append(key, &key_len, &key_size, "GET", 3);

    https = 0;
#ifdef USE_OPENSSL
    if(s->sess->listener->bind_conf->is_ssl) {
        https = 1;
    }
#endif

    key = _nst_cache_key_append(key, &key_len, &key_size, https ? "HTTPS": "HTTP", strlen(https ? "HTTPS": "HTTP"));
    if(!key) return NULL;

    ctx.idx  = 0;
    if(http_find_header2("Host", 4, ci_head(msg->chn), &txn->hdr_idx, &ctx)) {
        key = _nst_cache_key_append(key, &key_len, &key_size, ctx.line + ctx.val, ctx.vlen);
        if(!key) return NULL;
    }

    path_beg = http_txn_get_path(txn);
    url_end  = NULL;
    if(path_beg) {
        url_end = ci_head(msg->chn) + msg->sl.rq.u + msg->sl.rq.u_l;
        key     = _nst_cache_key_append(key, &key_len, &key_size, path_beg, url_end - path_beg);
        if(!key) return NULL;
    }
    return key;
}

/*
 * Check if valid cache exists
 */
struct nst_cache_data *nst_cache_exists(const char *key, uint64_t hash) {
    struct nst_cache_entry *entry = NULL;
    struct nst_cache_data  *data  = NULL;

    if(!key) return NULL;

    nuster_shctx_lock(&nuster.cache->dict[0]);
    entry = nst_cache_dict_get(key, hash);
    if(entry && entry->state == NST_CACHE_ENTRY_STATE_VALID) {
        data = entry->data;
        data->clients++;
    }
    nuster_shctx_unlock(&nuster.cache->dict[0]);

    return data;
}

/*
 * Start to create cache,
 * if cache does not exist, add a new nst_cache_entry
 * if cache exists but expired, add a new nst_cache_data to the entry
 * otherwise, set the corresponding state: bypass, wait
 */
void nst_cache_create(struct nst_cache_ctx *ctx, char *key, uint64_t hash) {
    struct nst_cache_entry *entry = NULL;

    nuster_shctx_lock(&nuster.cache->dict[0]);
    entry = nst_cache_dict_get(key, hash);
    if(entry) {
        if(entry->state == NST_CACHE_ENTRY_STATE_CREATING) {
            ctx->state = NST_CACHE_CTX_STATE_WAIT;
        } else if(entry->state == NST_CACHE_ENTRY_STATE_VALID) {
            ctx->state = NST_CACHE_CTX_STATE_HIT;
        } else if(entry->state == NST_CACHE_ENTRY_STATE_EXPIRED || entry->state == NST_CACHE_ENTRY_STATE_INVALID) {
            entry->state = NST_CACHE_ENTRY_STATE_CREATING;
            entry->data = nst_cache_data_new();
            if(!entry->data) {
                entry->state = NST_CACHE_ENTRY_STATE_INVALID;
                ctx->state   = NST_CACHE_CTX_STATE_BYPASS;
                ctx->full    = 1;
            } else {
                ctx->state   = NST_CACHE_CTX_STATE_CREATE;
                ctx->entry   = entry;
                ctx->data    = entry->data;
                ctx->element = entry->data->element;
            }
        } else {
            ctx->state = NST_CACHE_CTX_STATE_BYPASS;
        }
    } else {
        entry = nst_cache_dict_set(key, hash, ctx);
        if(entry) {
            ctx->state   = NST_CACHE_CTX_STATE_CREATE;
            ctx->entry   = entry;
            ctx->data    = entry->data;
            ctx->element = entry->data->element;
        } else {
            ctx->state = NST_CACHE_CTX_STATE_BYPASS;
            ctx->full  = 1;
        }
    }

    nuster_shctx_unlock(&nuster.cache->dict[0]);

    if(ctx->rule->disk) {
        char buf[21];
        sprintf(buf, "%" PRIu64, hash);
        int dir_len = strlen(global.nuster.cache.directory);
        ctx->disk.filename = nuster_memory_alloc(global.nuster.cache.memory, dir_len + strlen("/0/00/") + 20 + 1 + 17 );
        memcpy(ctx->disk.filename, global.nuster.cache.directory, dir_len);
        memcpy(ctx->disk.filename + dir_len, "/", 1);
        memcpy(ctx->disk.filename + dir_len + 1, buf, 1);
        memcpy(ctx->disk.filename + dir_len + 2, "/", 1);
        memcpy(ctx->disk.filename + dir_len + 3, buf, 2);
        memcpy(ctx->disk.filename + dir_len + 5, "/", 1);
        memcpy(ctx->disk.filename + dir_len + 6, buf, 20);
    }
}

/*
 * Add partial http data to nst_cache_data
 */
int nst_cache_update(struct nst_cache_ctx *ctx, struct http_msg *msg, long msg_len) {
    struct nst_cache_element *element = _nst_cache_data_append(msg, msg_len);

    if(element) {
        if(ctx->element) {
            ctx->element->next = element;
        } else {
            ctx->data->element = element;
        }
        ctx->element = element;
        return 1;
    } else {
        ctx->full = 1;
        return 0;
    }
}

/*
 * cache done
 */
void nst_cache_finish(struct nst_cache_ctx *ctx) {
    ctx->state = NST_CACHE_CTX_STATE_DONE;
    ctx->entry->state = NST_CACHE_ENTRY_STATE_VALID;
    if(*ctx->rule->ttl == 0) {
        ctx->entry->expire = 0;
    } else {
        ctx->entry->expire = get_current_timestamp() / 1000 + *ctx->rule->ttl;
    }
}

void nst_cache_abort(struct nst_cache_ctx *ctx) {
    ctx->entry->state = NST_CACHE_ENTRY_STATE_INVALID;
}

/*
 * Create cache applet to handle the request
 */
void nst_cache_hit(struct stream *s, struct stream_interface *si, struct channel *req,
        struct channel *res, struct nst_cache_data *data) {

    struct appctx *appctx = NULL;

    /*
     * set backend to nuster.applet.cache_engine
     */
    s->target = &nuster.applet.cache_engine.obj_type;
    if(unlikely(!si_register_handler(si, objt_applet(s->target)))) {
        /* return to regular process on error */
        data->clients--;
        s->target = NULL;
    } else {
        appctx = si_appctx(si);
        memset(&appctx->ctx.nuster.cache_engine, 0, sizeof(appctx->ctx.nuster.cache_engine));
        appctx->ctx.nuster.cache_engine.data    = data;
        appctx->ctx.nuster.cache_engine.element = data->element;

        req->analysers &= ~AN_REQ_FLT_HTTP_HDRS;
        req->analysers &= ~AN_REQ_FLT_XFER_DATA;

        req->analysers |= AN_REQ_FLT_END;
        req->analyse_exp = TICK_ETERNITY;

        res->flags |= CF_NEVER_WAIT;
    }
}

