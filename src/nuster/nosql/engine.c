/*
 * nuster nosql engine functions.
 *
 * Copyright (C) [Jiang Wenyuan](https://github.com/jiangwenyuan), < koubunen AT gmail DOT com >
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <nuster/memory.h>
#include <nuster/shctx.h>
#include <nuster/nuster.h>

#include <types/global.h>
#include <types/stream.h>
#include <types/channel.h>
#include <types/proxy.h>

#include <proto/stream_interface.h>
#include <proto/proto_http.h>
#include <proto/log.h>

static const char HTTP_100[] =
"HTTP/1.1 100 Continue\r\n\r\n";

static struct chunk http_100_chunk = {
    .str = (char *)&HTTP_100,
    .len = sizeof(HTTP_100)-1
};

static void nst_nosql_engine_handler(struct appctx *appctx) {
    struct stream_interface *si = appctx->owner;
    struct stream *s            = si_strm(si);
    struct channel *res         = si_ic(si);

    if(s->txn->req.msg_state == HTTP_MSG_DATA) {
        co_skip(si_oc(si), si_ob(si)->o);
    }
    if(s->txn->req.msg_state == HTTP_MSG_DONE) {
        ci_putblk(res, nuster_http_msgs[NUSTER_HTTP_200], strlen(nuster_http_msgs[NUSTER_HTTP_200]));
        co_skip(si_oc(si), si_ob(si)->o);
        si_shutr(si);
        res->flags |= CF_READ_NULL;
    }
}

static int _nst_nosql_dict_alloc(uint64_t size) {
    int i, entry_size = sizeof(struct nst_nosql_entry*);

    nuster.nosql->dict[0].size  = size / entry_size;
    nuster.nosql->dict[0].used  = 0;
    nuster.nosql->dict[0].entry = nuster_memory_alloc(global.nuster.nosql.memory, global.nuster.nosql.memory->block_size);
    if(!nuster.nosql->dict[0].entry) return 0;

    for(i = 1; i < size / global.nuster.nosql.memory->block_size; i++) {
        if(!nuster_memory_alloc(global.nuster.nosql.memory, global.nuster.nosql.memory->block_size)) return 0;
    }
    for(i = 0; i < nuster.nosql->dict[0].size; i++) {
        nuster.nosql->dict[0].entry[i] = NULL;
    }
    return nuster_shctx_init((&nuster.nosql->dict[0]));
}

struct nst_nosql_data *nst_nosql_data_new() {

    struct nst_nosql_data *data = nuster_memory_alloc(global.nuster.nosql.memory, sizeof(*data));

    nuster_shctx_lock(nuster.nosql);
    if(data) {
        data->clients = 0;
        data->invalid = 0;
        data->element = NULL;

        if(nuster.nosql->data_head == NULL) {
            nuster.nosql->data_head = data;
            nuster.nosql->data_tail = data;
            data->next              = data;
        } else {
            if(nuster.nosql->data_head == nuster.nosql->data_tail) {
                nuster.nosql->data_head->next = data;
                data->next                    = nuster.nosql->data_head;
                nuster.nosql->data_tail       = data;
            } else {
                data->next                    = nuster.nosql->data_head;
                nuster.nosql->data_tail->next = data;
                nuster.nosql->data_tail       = data;
            }
        }
    }
    nuster_shctx_unlock(nuster.nosql);
    return data;
}

void nst_nosql_init() {
    nuster.applet.nosql_engine.fct = nst_nosql_engine_handler;

    if(global.nuster.nosql.status == NUSTER_STATUS_ON) {
        global.nuster.nosql.pool.ctx   = create_pool("np.ctx", sizeof(struct nst_nosql_ctx), MEM_F_SHARED);
        global.nuster.nosql.memory = nuster_memory_create("nosql.shm", global.nuster.nosql.data_size, global.tune.bufsize, NST_NOSQL_DEFAULT_CHUNK_SIZE);
        if(!global.nuster.nosql.memory) {
            goto shm_err;
        }
        if(!nuster_shctx_init(global.nuster.nosql.memory)) {
            goto shm_err;
        }
        nuster.nosql = nuster_memory_alloc(global.nuster.nosql.memory, sizeof(struct nst_nosql));
        if(!nuster.nosql) {
            goto err;
        }

        nuster.nosql->dict[0].entry = NULL;
        nuster.nosql->dict[0].used  = 0;
        nuster.nosql->dict[1].entry = NULL;
        nuster.nosql->dict[1].used  = 0;
        nuster.nosql->data_head     = NULL;
        nuster.nosql->data_tail     = NULL;
        nuster.nosql->rehash_idx    = -1;
        nuster.nosql->cleanup_idx   = 0;

        if(!nuster_shctx_init(nuster.nosql)) {
            goto shm_err;
        }

        if(!nst_nosql_dict_init()) {
            goto err;
        }
    }
    return;
err:
    ha_alert("Out of memory when initializing nuster nosql.\n");
    exit(1);
shm_err:
    ha_alert("Error when initializing nuster nosql memory.\n");
    exit(1);
}

/*
 * return 1 if the request is done, otherwise 0
 */
int nst_nosql_check_applet(struct stream *s, struct channel *req, struct proxy *px) {
    if(global.nuster.nosql.status == NUSTER_STATUS_ON && px->nuster.mode == NUSTER_MODE_NOSQL) {
        struct stream_interface *si = &s->si[1];
        struct http_txn *txn        = s->txn;
        struct http_msg *msg        = &txn->req;
        struct appctx *appctx       = NULL;

        s->target = &nuster.applet.nosql_engine.obj_type;
        if(unlikely(!stream_int_register_handler(si, objt_applet(s->target)))) {
            txn->status = 500;
            nuster_response(s, &nuster_http_msg_chunks[NUSTER_HTTP_500]);
            return 1;
        } else {
            appctx      = si_appctx(si);
            appctx->st0 = NST_NOSQL_APPCTX_STATE_INIT;
            appctx->st1 = 0;
            appctx->st2 = 0;

            if(msg->msg_state < HTTP_MSG_CHUNK_SIZE) {
                if(msg->msg_state < HTTP_MSG_100_SENT) {
                    if(msg->flags & HTTP_MSGF_VER_11) {
                        struct hdr_ctx ctx;
                        ctx.idx = 0;
                        if(http_find_header2("Expect", 6, req->buf->p, &txn->hdr_idx, &ctx) &&
                                unlikely(ctx.vlen == 12 && strncasecmp(ctx.line+ctx.val, "100-continue", 12) == 0)) {
                            co_inject(&s->res, http_100_chunk.str, http_100_chunk.len);
                            http_remove_header2(&txn->req, &txn->hdr_idx, &ctx);
                        }
                    }
                    msg->msg_state = HTTP_MSG_100_SENT;
                }
                msg->next = msg->sov;

                if(msg->flags & HTTP_MSGF_TE_CHNK) {
                    msg->msg_state = HTTP_MSG_CHUNK_SIZE;
                } else {
                    msg->msg_state = HTTP_MSG_DATA;
                }
            }

            req->analysers &= (AN_REQ_HTTP_BODY | AN_REQ_FLT_HTTP_HDRS | AN_REQ_FLT_END);
            req->analysers &= ~AN_REQ_FLT_XFER_DATA;
            req->analysers |= AN_REQ_HTTP_XFER_BODY;

        }
    }
    return 0;
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
static char *_nst_nosql_key_append(char *dst, int *dst_len, int *dst_size,
        char *src, int src_len) {
    char *key = _string_append(dst, dst_len, dst_size, src, src_len);
    if(key) {
        return _string_append(dst, dst_len, dst_size, ".", 1);
    }
    return NULL;
}

int nst_nosql_prebuild_key(struct nst_nosql_ctx *ctx, struct stream *s, struct http_msg *msg) {

    struct http_txn *txn = s->txn;

    char *url_end;
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
    if(http_find_header2("Host", 4, msg->chn->buf->p, &txn->hdr_idx, &hdr)) {
        ctx->req.host.data = nuster_memory_alloc(global.nuster.nosql.memory, hdr.vlen);
        if(!ctx->req.host.data) {
            return 0;
        }
        ctx->req.host.len  = hdr.vlen;
        memcpy(ctx->req.host.data, hdr.line + hdr.val, hdr.vlen);
    }

    ctx->req.path.data = http_get_path(txn);
    ctx->req.path.len  = 0;
    ctx->req.uri.data  = ctx->req.path.data;
    ctx->req.uri.len   = 0;
    url_end            = NULL;
    if(ctx->req.path.data) {
        char *ptr = ctx->req.path.data;
        url_end   = msg->chn->buf->p + msg->sl.rq.u + msg->sl.rq.u_l;
        while(ptr < url_end && *ptr != '?') {
            ptr++;
        }
        ctx->req.path.len = ptr - ctx->req.path.data;
        ctx->req.uri.len  = url_end - ctx->req.uri.data;
    }
    /* extra 1 char as required by regex_exec_match2 */
    ctx->req.path.data = nuster_memory_alloc(global.nuster.nosql.memory, ctx->req.path.len + 1);
    if(!ctx->req.path.data) {
        return 0;
    }
    memcpy(ctx->req.path.data, ctx->req.uri.data, ctx->req.path.len);

    ctx->req.query.data = NULL;
    ctx->req.query.len  = 0;
    ctx->req.delimiter  = 0;
    if(ctx->req.uri.data) {
        ctx->req.query.data = memchr(ctx->req.uri.data, '?', url_end - ctx->req.uri.data);
        if(ctx->req.query.data) {
            ctx->req.query.data++;
            ctx->req.query.len = url_end - ctx->req.query.data;
            if(ctx->req.query.len) {
                ctx->req.delimiter = 1;
            }
        }
    }

    ctx->req.cookie.data = NULL;
    ctx->req.cookie.len  = 0;
    hdr.idx              = 0;
    if(http_find_header2("Cookie", 6, msg->chn->buf->p, &txn->hdr_idx, &hdr)) {
        ctx->req.cookie.data = hdr.line + hdr.val;
        ctx->req.cookie.len  = hdr.vlen;
    }

    return 1;
}

static int _nst_nosql_find_param_value_by_name(char *query_beg, char *query_end,
        char *name, char **value, int *value_len) {

    char equal   = '=';
    char and     = '&';
    char *ptr    = query_beg;
    int name_len = strlen(name);

    while(ptr + name_len + 1 < query_end) {
        if(!memcmp(ptr, name, name_len) && *(ptr + name_len) == equal) {
            if(ptr == query_beg || *(ptr - 1) == and) {
                ptr    = ptr + name_len + 1;
                *value = ptr;
                while(ptr < query_end && *ptr != and) {
                    (*value_len)++;
                    ptr++;
                }
                return 1;
            }
        }
        ptr++;
    }
    return 0;
}

char *nst_nosql_build_key(struct nst_nosql_ctx *ctx, struct nuster_rule_key **pck, struct stream *s,
        struct http_msg *msg) {

    struct http_txn *txn = s->txn;

    struct hdr_ctx hdr;

    struct nuster_rule_key *ck = NULL;
    int key_len          = 0;
    int key_size         = NST_NOSQL_DEFAULT_KEY_SIZE;
    char *key            = malloc(key_size);
    if(!key) {
        return NULL;
    }

    nuster_debug("[CACHE] Calculate key: ");
    while((ck = *pck++)) {
        switch(ck->type) {
            case NUSTER_RULE_KEY_METHOD:
                nuster_debug("method.");
                key = _nst_nosql_key_append(key, &key_len, &key_size, http_known_methods[txn->meth].name, strlen(http_known_methods[txn->meth].name));
                break;
            case NUSTER_RULE_KEY_SCHEME:
                nuster_debug("scheme.");
                key = _nst_nosql_key_append(key, &key_len, &key_size, ctx->req.scheme == SCH_HTTPS ? "HTTPS" : "HTTP", ctx->req.scheme == SCH_HTTPS ? 5 : 4);
                break;
            case NUSTER_RULE_KEY_HOST:
                nuster_debug("host.");
                if(ctx->req.host.data) {
                    key = _nst_nosql_key_append(key, &key_len, &key_size, ctx->req.host.data, ctx->req.host.len);
                }
                break;
            case NUSTER_RULE_KEY_URI:
                nuster_debug("uri.");
                if(ctx->req.uri.data) {
                    key = _nst_nosql_key_append(key, &key_len, &key_size, ctx->req.uri.data, ctx->req.uri.len);
                }
                break;
            case NUSTER_RULE_KEY_PATH:
                nuster_debug("path.");
                if(ctx->req.path.data) {
                    key = _nst_nosql_key_append(key, &key_len, &key_size, ctx->req.path.data, ctx->req.path.len);
                }
                break;
            case NUSTER_RULE_KEY_DELIMITER:
                nuster_debug("delimiter.");
                key = _nst_nosql_key_append(key, &key_len, &key_size, ctx->req.delimiter ? "?": "", ctx->req.delimiter);
                break;
            case NUSTER_RULE_KEY_QUERY:
                nuster_debug("query.");
                if(ctx->req.query.data && ctx->req.query.len) {
                    key = _nst_nosql_key_append(key, &key_len, &key_size, ctx->req.query.data, ctx->req.query.len);
                }
                break;
            case NUSTER_RULE_KEY_PARAM:
                nuster_debug("param_%s.", ck->data);
                if(ctx->req.query.data && ctx->req.query.len) {
                    char *v = NULL;
                    int v_l = 0;
                    if(_nst_nosql_find_param_value_by_name(ctx->req.query.data, ctx->req.query.data + ctx->req.query.len, ck->data, &v, &v_l)) {
                        key = _nst_nosql_key_append(key, &key_len, &key_size, v, v_l);
                    }

                }
                break;
            case NUSTER_RULE_KEY_HEADER:
                hdr.idx = 0;
                nuster_debug("header_%s.", ck->data);
                if(http_find_header2(ck->data, strlen(ck->data), msg->chn->buf->p, &txn->hdr_idx, &hdr)) {
                    key = _nst_nosql_key_append(key, &key_len, &key_size, hdr.line + hdr.val, hdr.vlen);
                }
                break;
            case NUSTER_RULE_KEY_COOKIE:
                nuster_debug("header_%s.", ck->data);
                if(ctx->req.cookie.data) {
                    char *v = NULL;
                    int v_l = 0;
                    if(extract_cookie_value(ctx->req.cookie.data, ctx->req.cookie.data + ctx->req.cookie.len, ck->data, strlen(ck->data), 1, &v, &v_l)) {
                        key = _nst_nosql_key_append(key, &key_len, &key_size, v, v_l);
                    }
                }
                break;
            case NUSTER_RULE_KEY_BODY:
                nuster_debug("body.");
                if(txn->meth == HTTP_METH_POST || txn->meth == HTTP_METH_PUT) {
                    if((s->be->options & PR_O_WREQ_BODY) && msg->body_len > 0 ) {
                        key = _nst_nosql_key_append(key, &key_len, &key_size, msg->chn->buf->p + msg->sov, msg->body_len);
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

uint64_t nst_nosql_hash_key(const char *key) {
    return XXH64(key, strlen(key), 0);
}

void nst_nosql_hit(struct stream *s, struct stream_interface *si, struct channel *req,
        struct channel *res, struct nst_nosql_data *data) {
}

void nst_nosql_create(struct nst_nosql_ctx *ctx, char *key, uint64_t hash) {
    struct nst_nosql_entry *entry = NULL;

    ///* Check if nosql is full */
    //if(nst_nosql_stats_full()) {
    //    ctx->state = NST_nosql_CTX_STATE_FULL;
    //    return;
    //}

    nuster_shctx_lock(&nuster.nosql->dict[0]);
    entry = nst_nosql_dict_get(key, hash);
    if(entry) {
        if(entry->state == NST_NOSQL_ENTRY_STATE_CREATING) {
            ctx->state = NST_NOSQL_CTX_STATE_WAIT;
        } else {
            entry->state = NST_NOSQL_ENTRY_STATE_CREATING;
            if(entry->data) {
                entry->data->invalid = 1;
            }
            entry->data = nst_nosql_data_new();
        }
    } else {
        entry = nst_nosql_dict_set(key, hash, ctx);
    }
    nuster_shctx_unlock(&nuster.nosql->dict[0]);

    if(!entry && !entry->data) {
        ctx->state   = NST_NOSQL_CTX_STATE_INVALID;
    } else {
        ctx->state   = NST_NOSQL_CTX_STATE_CREATE;
        ctx->entry   = entry;
        ctx->data    = entry->data;
        ctx->element = entry->data->element;
    }
}

static struct nst_nosql_element *_nst_nosql_data_append(struct nst_nosql_element *tail,
        struct http_msg *msg, long msg_len) {

    struct nst_nosql_element *element = nuster_memory_alloc(global.nuster.nosql.memory, sizeof(*element));

    if(element) {
        char *data = msg->chn->buf->data;
        char *p    = msg->chn->buf->p;
        int size   = msg->chn->buf->size;

        element->msg.data = nuster_memory_alloc(global.nuster.nosql.memory, msg_len);
        if(!element->msg.data) return NULL;

        if(p - data + msg_len > size) {
            int right = data + size - p;
            int left  = msg_len - right;
            memcpy(element->msg.data, p, right);
            memcpy(element->msg.data + right, data, left);
        } else {
            memcpy(element->msg.data, p, msg_len);
        }
        element->msg.len = msg_len;
        element->next    = NULL;
        if(tail == NULL) {
            tail = element;
        } else {
            tail->next = element;
        }
        //nst_nosql_stats_update_used_mem(msg_len);
    }
    return element;
}

int nst_nosql_update(struct nst_nosql_ctx *ctx, struct http_msg *msg, long msg_len) {
    struct nst_nosql_element *element = _nst_nosql_data_append(ctx->element, msg, msg_len);

    if(element) {
        if(!ctx->element) {
            ctx->data->element = element;
        }
        ctx->element = element;
        return 1;
    } else {
        return 0;
    }
}
struct nst_nosql_data *nst_nosql_exists(const char *key, uint64_t hash) {
    struct nst_nosql_entry *entry = NULL;
    struct nst_nosql_data  *data  = NULL;

    if(!key) return NULL;

    nuster_shctx_lock(&nuster.nosql->dict[0]);
    entry = nst_nosql_dict_get(key, hash);
    if(entry && entry->state == NST_NOSQL_ENTRY_STATE_VALID) {
        data = entry->data;
        data->clients++;
    }
    nuster_shctx_unlock(&nuster.nosql->dict[0]);

    return data;
}

void nst_nosql_finish(struct nst_nosql_ctx *ctx) {
    ctx->state = NST_NOSQL_CTX_STATE_DONE;
    ctx->entry->state = NST_NOSQL_ENTRY_STATE_VALID;
    if(*ctx->rule->ttl == 0) {
        ctx->entry->expire = 0;
    } else {
        ctx->entry->expire = get_current_timestamp() / 1000 + *ctx->rule->ttl;
    }
}

