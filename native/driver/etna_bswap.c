/*
 * Copyright (c) 2012-2013 Etnaviv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include "etna_bswap.h"

#include "viv.h"
#include "etna.h"
#include "etna_state.h"
#include "state.xml.h"
#include "state_3d.xml.h"

#include <stdio.h>
#include <assert.h>

static int etna_bswap_init_buffer(struct viv_conn *conn, etna_bswap_buffer *buf)
{
    pthread_mutex_init(&buf->available_mutex, NULL);
    pthread_cond_init(&buf->available_cond, NULL);
    buf->is_available = 1;
    if(viv_user_signal_create(conn, 0, &buf->sig_id_ready) != 0)
    {
#ifdef DEBUG
        fprintf(stderr, "Cannot create user signal for framebuffer sync\n");
#endif
        return ETNA_INTERNAL_ERROR;
    }
    return ETNA_OK;
}

static void etna_bswap_destroy_buffer(struct viv_conn *conn, etna_bswap_buffer *buf)
{
    (void)pthread_mutex_destroy(&buf->available_mutex);
    (void)pthread_cond_destroy(&buf->available_cond);
    (void)viv_user_signal_destroy(conn, buf->sig_id_ready);
}

static void etna_bswap_thread(etna_bswap_buffers *bufs)
{
    int cur = 0;
    while(!bufs->terminate)
    {
        /* wait for "buffer ready" signal for buffer X (and clear it) */
        if(viv_user_signal_wait(bufs->conn, bufs->buf[cur].sig_id_ready, SIG_WAIT_INDEFINITE) != 0)
        {
#ifdef DEBUG
            fprintf(stderr, "Error waiting for framebuffer sync signal\n");
#endif
            return; // ?
        }
        /* switch buffers */
        bufs->set_buffer(bufs->userptr, cur);
        bufs->frontbuffer = cur;
        /* X = (X+1)%buffers */
        cur = (cur+1) % ETNA_BSWAP_NUM_BUFFERS;
        /* set "buffer available" for buffer X, signal condition */
        pthread_mutex_lock(&bufs->buf[cur].available_mutex);
        bufs->buf[cur].is_available = 1;
        pthread_cond_signal(&bufs->buf[cur].available_cond);
        pthread_mutex_unlock(&bufs->buf[cur].available_mutex);
    }
}

int etna_bswap_create(struct etna_ctx *ctx, etna_bswap_buffers **bufs_out, 
        etna_set_buffer_cb_t set_buffer, 
        etna_copy_buffer_cb_t copy_buffer,
        void *userptr)
{
    if(ctx == NULL || bufs_out == NULL)
        return ETNA_INVALID_ADDR;
    etna_bswap_buffers *bufs = ETNA_NEW(etna_bswap_buffers);
    if(bufs == NULL)
        return ETNA_INTERNAL_ERROR;
    bufs->conn = ctx->conn;
    bufs->ctx = ctx;
    bufs->set_buffer = set_buffer;
    bufs->copy_buffer = copy_buffer;
    bufs->userptr = userptr;
    bufs->frontbuffer = 0;
    bufs->backbuffer = 1;
    bufs->terminate = false;
    bufs->set_buffer(bufs->userptr, bufs->frontbuffer);

    for(int idx=0; idx<ETNA_BSWAP_NUM_BUFFERS; ++idx)
        etna_bswap_init_buffer(bufs->conn, &bufs->buf[idx]);
    pthread_create(&bufs->thread, NULL, (void * (*)(void *))&etna_bswap_thread, bufs);

    *bufs_out = bufs;
    return ETNA_OK;
}

int etna_bswap_free(etna_bswap_buffers *bufs)
{
    bufs->terminate = true;
    /* signal ready signals, to prevent thread from waiting forever for buffer to become ready */
    for(int idx=0; idx<ETNA_BSWAP_NUM_BUFFERS; ++idx)
        (void)viv_user_signal_signal(bufs->conn, bufs->buf[idx].sig_id_ready, 1); 
    (void)pthread_join(bufs->thread, NULL);
    for(int idx=0; idx<ETNA_BSWAP_NUM_BUFFERS; ++idx)
        etna_bswap_destroy_buffer(bufs->conn, &bufs->buf[idx]);
    ETNA_FREE(bufs);
    return ETNA_OK;
}

/* wait until current backbuffer is available to render to */
int etna_bswap_wait_available(etna_bswap_buffers *bufs)
{
    etna_bswap_buffer *buf = &bufs->buf[bufs->backbuffer];
    /* Wait until buffer buf is available */
    pthread_mutex_lock(&buf->available_mutex);
    if(!buf->is_available) /* if we're going to wait anyway, flush so that GPU is not idle */
    {
        etna_flush(bufs->ctx);
    }
    while(!buf->is_available)
    {
        pthread_cond_wait(&buf->available_cond, &buf->available_mutex);
    }
    buf->is_available = 0;
    pthread_mutex_unlock(&buf->available_mutex);
    return ETNA_OK;
}

/* queue buffer swap when GPU ready with rendering to buf */
int etna_bswap_queue_swap(etna_bswap_buffers *bufs)
{
    etna_flush(bufs->ctx); /* must flush before swap to make sure signal happens after all current commands processed */
    if(viv_event_queue_signal(bufs->conn, bufs->buf[bufs->backbuffer].sig_id_ready, gcvKERNEL_PIXEL) != 0)
    {
#ifdef DEBUG
        fprintf(stderr, "Unable to queue framebuffer sync signal\n");
#endif
        return ETNA_INTERNAL_ERROR;
    }
    bufs->backbuffer = (bufs->backbuffer + 1) % ETNA_BSWAP_NUM_BUFFERS;
    return ETNA_OK;
}

int etna_swap_buffers(etna_bswap_buffers *bufs)
{
    assert(bufs->copy_buffer);
    /* copy to screen */
    etna_bswap_wait_available(bufs);

    /*  this flush is really needed, otherwise some quads will have pieces undrawn */
    etna_set_state(bufs->ctx, VIVS_GL_FLUSH_CACHE, VIVS_GL_FLUSH_CACHE_COLOR | VIVS_GL_FLUSH_CACHE_DEPTH);

    bufs->copy_buffer(bufs->userptr, bufs->ctx, bufs->backbuffer);

    etna_bswap_queue_swap(bufs);
    return 0;
}


