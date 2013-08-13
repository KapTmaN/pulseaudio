/***
    This file is part of PulseAudio.

    Copyright 2013 Alexander Couzens

    PulseAudio is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License,
    or (at your option) any later version.

    PulseAudio is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with PulseAudio; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
    USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/context.h>
#include <pulse/timeval.h>
#include <pulse/xmalloc.h>
#include <pulse/stream.h>
#include <pulse/mainloop.h>
#include <pulse/subscribe.h>
#include <pulse/introspect.h>
#include <pulse/error.h>

#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/i18n.h>
#include <pulsecore/source.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/poll.h>
#include <pulsecore/proplist-util.h>

#include "module-tunnel-source-new-symdef.h"

PA_MODULE_AUTHOR("Alexander Couzens");
PA_MODULE_DESCRIPTION("Create a network source which connects via a stream to a remote PulseAudio server");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
        "server=<address> "
        "source=<name of the remote source> "
        "source_name=<name for the local source> "
        "source_properties=<properties for the local source> "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "channel_map=<channel map>"
        );

#define TUNNEL_THREAD_FAILED_MAINLOOP 1

/* libpulse callbacks */
static void stream_state_callback(pa_stream *stream, void *userdata);
static void context_state_callback(pa_context *c, void *userdata);
static void source_update_requested_latency_cb(pa_source *s);

struct userdata {
    pa_module *module;
    pa_source *source;
    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_mainloop *thread_mainloop;
    pa_mainloop_api *thread_mainloop_api;

    /* libpulse context */
    pa_context *context;
    pa_stream *stream;

    pa_buffer_attr bufferattr;

    bool connected;

    char *remote_server;
    char *remote_source_name;
};

static const char* const valid_modargs[] = {
    "source_name",
    "source_properties",
    "server",
    "source",
    "format",
    "channels",
    "rate",
    "channel_map",
    "cookie", /* unimplemented */
    "reconnect", /* reconnect if server comes back again - unimplemented*/
    NULL,
};

static pa_proplist* tunnel_new_proplist(struct userdata *u) {
    pa_proplist *proplist = pa_proplist_new();
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_NAME, "PulseAudio");
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_ID, "org.PulseAudio.PulseAudio");
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_VERSION, PACKAGE_VERSION);
    pa_init_proplist(proplist);

    return proplist;
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;
    pa_proplist *proplist;

    void *p;
    size_t readable = 0;
    size_t read = 0;

    pa_assert(u);

    pa_log_debug("Thread starting up");
    pa_thread_mq_install(&u->thread_mq);



    proplist = tunnel_new_proplist(u);
    /* init libpulse */
    u->context = pa_context_new_with_proplist(pa_mainloop_get_api(u->thread_mainloop),
                                              "PulseAudio",
                                              proplist);
    pa_proplist_free(proplist);

    if (!u->context) {
        pa_log("Failed to create libpulse context");
        goto fail;
    }

    pa_context_set_state_callback(u->context, context_state_callback, u);
    if (pa_context_connect(u->context,
                           u->remote_server,
                           PA_CONTEXT_NOFAIL | PA_CONTEXT_NOAUTOSPAWN,
                           NULL) < 0) {
        pa_log("Failed to connect libpulse context");
        goto fail;
    }

    for (;;) {
        int ret;
        pa_memchunk memchunk;
        pa_memchunk_reset(&memchunk);


        if (pa_mainloop_iterate(u->thread_mainloop, 1, &ret) < 0) {
            if (ret == 0)
                goto finish;
            else
                goto fail;
        }

        if (u->connected &&
                PA_STREAM_IS_GOOD(pa_stream_get_state(u->stream))) {

            if (pa_stream_is_corked(u->stream)) {
                pa_stream_cork(u->stream, 0, NULL, NULL);
                continue;
            }

            readable = pa_stream_readable_size(u->stream);
            if (readable > 0) {
                /* we have new data to read */
                if (pa_stream_peek(u->stream, (const void**) &p, &read) != 0) {
                    pa_log(_("pa_stream_peek() failed: %s"), pa_strerror(pa_context_errno(u->context)));
                    goto fail;
                }

                memchunk.memblock = pa_memblock_new_fixed(u->module->core->mempool, p, read, true);
                memchunk.length = read;
                memchunk.index = 0;

                pa_source_post(u->source, &memchunk);
                pa_memblock_unref_fixed(memchunk.memblock);

                pa_stream_drop(u->stream);

                if (ret != 0) {
                    /* TODO: we should consider a state change or is that already done ? */
                    pa_log_warn("Could not write data into the stream ... ret = %i", ret);
                }
            }
        }
    }
fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN
     *
     * Note: is this a race condition? When a PA_MESSAGE_SHUTDOWN already within the queue?
     */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->module->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    if (u->stream) {
        pa_stream_disconnect(u->stream);
        pa_stream_unref(u->stream);
        u->stream = NULL;
    }

    if (u->context) {
        pa_context_disconnect(u->context);
        pa_context_unref(u->context);
        u->context = NULL;
    }

    pa_log_debug("Thread shutting down");
}

static void stream_state_callback(pa_stream *stream, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    switch (pa_stream_get_state(stream)) {
        case PA_STREAM_FAILED:
            pa_log_error("Stream failed.");
            u->connected = false;
            u->thread_mainloop_api->quit(u->thread_mainloop_api, TUNNEL_THREAD_FAILED_MAINLOOP);
            break;
        case PA_STREAM_TERMINATED:
            pa_log_debug("Stream terminated.");
            break;
        default:
            break;
    }
}

static void context_state_callback(pa_context *c, void *userdata) {
    struct userdata *u = userdata;
    int c_errno;

    pa_assert(u);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;
        case PA_CONTEXT_READY: {
            pa_proplist *proplist;
            const char *username = pa_get_user_name_malloc();
            const char *hostname = pa_get_host_name_malloc();
            /* TODO: old tunnel say 'Null-Output' */
            char *stream_name = pa_sprintf_malloc("%s for %s@%s", "Tunnel", username, hostname);

            pa_log_debug("Connection successful. Creating stream.");
            pa_assert(!u->stream);

            proplist = tunnel_new_proplist(u);
            pa_proplist_sets(proplist, PA_PROP_MEDIA_ROLE, "sound");
            pa_assert(proplist);

            u->stream = pa_stream_new_with_proplist(u->context,
                                                    stream_name,
                                                    &u->source->sample_spec,
                                                    &u->source->channel_map,
                                                    proplist);
            pa_proplist_free(proplist);
            pa_xfree(stream_name);

            if(!u->stream) {
                pa_log_error("Could not create a stream.");
                u->thread_mainloop_api->quit(u->thread_mainloop_api, TUNNEL_THREAD_FAILED_MAINLOOP);
                return;
            }


            pa_context_subscribe(u->context, PA_SUBSCRIPTION_MASK_SINK_INPUT, NULL, NULL);

            pa_stream_set_state_callback(u->stream, stream_state_callback, userdata);
            if (pa_stream_connect_record(u->stream,
                                         u->remote_source_name,
                                         &u->bufferattr,
                                         PA_STREAM_AUTO_TIMING_UPDATE) < 0) {
                /* TODO fail */
            }
            u->connected = true;
            break;
        }
        case PA_CONTEXT_FAILED:
            c_errno = pa_context_errno(u->context);
            pa_log_debug("Context failed with err %d.", c_errno);
            u->connected = false;
            u->thread_mainloop_api->quit(u->thread_mainloop_api, TUNNEL_THREAD_FAILED_MAINLOOP);
            break;
        case PA_CONTEXT_TERMINATED:
            pa_log_debug("Context terminated.");
            u->connected = false;
            u->thread_mainloop_api->quit(u->thread_mainloop_api, TUNNEL_THREAD_FAILED_MAINLOOP);
            break;
        default:
            break;
    }
}

static void source_update_requested_latency_cb(pa_source *s) {
    struct userdata *u;
    size_t nbytes;
    pa_usec_t block_usec;

    pa_source_assert_ref(s);
    pa_assert_se(u = s->userdata);

    block_usec = pa_source_get_requested_latency_within_thread(s);

    if (block_usec == (pa_usec_t) -1)
        block_usec = s->thread_info.max_latency;

    nbytes = pa_usec_to_bytes(block_usec, &s->sample_spec);
    pa_source_set_max_rewind_within_thread(s, nbytes);

    if (block_usec != (pa_usec_t) -1) {
        u->bufferattr.fragsize = nbytes;
    }

    if (u->stream && PA_STREAM_IS_GOOD(pa_stream_get_state(u->stream))) {
        pa_stream_set_buffer_attr(u->stream, &u->bufferattr, NULL, NULL);
    }
}

static int source_process_msg_cb(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SOURCE(o)->userdata;

    switch (code) {
        case PA_SOURCE_MESSAGE_GET_LATENCY: {
            int negative;
            pa_usec_t remote_latency;

            if (!PA_SOURCE_IS_LINKED(u->source->thread_info.state)) {
                *((pa_usec_t*) data) = 0;
                return 0;
            }

            if (!u->stream) {
                *((pa_usec_t*) data) = 0;
                return 0;
            }

            if (!PA_STREAM_IS_GOOD(pa_stream_get_state(u->stream))) {
                *((pa_usec_t*) data) = 0;
                return 0;
            }

            if (pa_stream_get_latency(u->stream, &remote_latency, &negative) < 0) {
                *((pa_usec_t*) data) = 0;
                return 0;
            }

            *((pa_usec_t*) data) =
                /* Add the latency from libpulse */
                remote_latency;
                /* do we have to add more latency here ? */
            return 0;
        }
    }
    return pa_source_process_msg(o, code, data, offset, chunk);
}

int pa__init(pa_module *m) {
    struct userdata *u = NULL;
    pa_modargs *ma = NULL;
    pa_source_new_data source_data;
    pa_sample_spec ss;
    pa_channel_map map;
    const char *remote_server = NULL;
    const char *source_name = NULL;
    char *default_source_name = NULL;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    ss = m->core->default_sample_spec;
    map = m->core->default_channel_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    remote_server = pa_modargs_get_value(ma, "server", NULL);
    if (!remote_server) {
        pa_log("No server given!");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->module = m;
    m->userdata = u;
    u->remote_server = pa_xstrdup(remote_server);
    u->thread_mainloop = pa_mainloop_new();
    if (u->thread_mainloop == NULL) {
        pa_log("Failed to create mainloop");
        goto fail;
    }
    u->thread_mainloop_api = pa_mainloop_get_api(u->thread_mainloop);

    u->remote_source_name = pa_xstrdup(pa_modargs_get_value(ma, "source", NULL));

    u->bufferattr.maxlength = (uint32_t) -1;
    u->bufferattr.minreq = (uint32_t) -1;
    u->bufferattr.prebuf = (uint32_t) -1;
    u->bufferattr.tlength = (uint32_t) -1;

    pa_thread_mq_init_thread_mainloop(&u->thread_mq, m->core->mainloop, pa_mainloop_get_api(u->thread_mainloop));

    /* Create source */
    pa_source_new_data_init(&source_data);
    source_data.driver = __FILE__;
    source_data.module = m;

    default_source_name = pa_sprintf_malloc("tunnel-source-new.%s", remote_server);
    source_name = pa_modargs_get_value(ma, "source_name", default_source_name);

    pa_source_new_data_set_name(&source_data, source_name);
    pa_source_new_data_set_sample_spec(&source_data, &ss);
    pa_source_new_data_set_channel_map(&source_data, &map);

    pa_proplist_sets(source_data.proplist, PA_PROP_DEVICE_CLASS, "sound");
    pa_proplist_setf(source_data.proplist,
                     PA_PROP_DEVICE_DESCRIPTION,
                     _("Tunnel to %s/%s"),
                     remote_server,
                     pa_strempty(u->remote_source_name));

    if (pa_modargs_get_proplist(ma, "source_properties", source_data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_source_new_data_done(&source_data);
        goto fail;
    }
    /* TODO: check PA_SINK_LATENCY + PA_SINK_DYNAMIC_LATENCY */
    if (!(u->source = pa_source_new(m->core, &source_data, (PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY|PA_SINK_NETWORK)))) {
        pa_log("Failed to create source.");
        pa_source_new_data_done(&source_data);
        goto fail;
    }

    pa_source_new_data_done(&source_data);
    u->source->userdata = u;

    /* source callbacks */
    u->source->parent.process_msg = source_process_msg_cb;
    u->source->update_requested_latency = source_update_requested_latency_cb;

    /* set thread queue */
    pa_source_set_asyncmsgq(u->source, u->thread_mq.inq);

    if (!(u->thread = pa_thread_new("tunnel-source", thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_source_put(u->source);
    pa_modargs_free(ma);
    pa_xfree(default_source_name);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    if (default_source_name)
        pa_xfree(default_source_name);

    pa__done(m);

    return -1;
}

void pa__done(pa_module *m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->source)
        pa_source_unlink(u->source);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->thread_mainloop)
        pa_mainloop_free(u->thread_mainloop);

    if (u->remote_source_name)
        pa_xfree(u->remote_source_name);

    if (u->remote_server)
        pa_xfree(u->remote_server);

    if (u->source)
        pa_source_unref(u->source);

    pa_xfree(u);
}
