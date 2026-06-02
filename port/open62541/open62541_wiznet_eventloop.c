#include "open62541.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "socket.h"
#include "wizchip_conf.h"

#define WIZ_UA_SOCKET 0u
#define WIZ_OPCUA_TCP_PORT 4840u
#define WIZ_LISTEN_CONNECTION_ID 1u
#define WIZ_ACTIVE_CONNECTION_ID 2u
#define WIZ_RX_BUFFER_SIZE 8192u
#define WIZ_TIMER_MAX 32u

typedef struct {
    bool active;
    UA_UInt64 id;
    UA_Callback callback;
    void *application;
    void *context;
    UA_DateTime nextTime;
    UA_Double intervalMs;
    UA_TimerPolicy policy;
} WizTimerSlot;

typedef struct {
    UA_EventLoop eventLoop;
    UA_DelayedCallback *delayedCallbacks;
    WizTimerSlot timers[WIZ_TIMER_MAX];
    UA_UInt64 nextTimerId;
} WizEventLoop;

typedef struct {
    UA_ConnectionManager cm;
    UA_ConnectionManager_connectionCallback callback;
    void *application;
    void *listenContext;
    void *activeContext;
    bool listenAnnounced;
    bool activeAnnounced;
    bool pendingListenClose;
    bool pendingActiveClose;
    uint16_t port;
    uint8_t peerIp[4];
    uint16_t peerPort;
    uint8_t rxBuffer[WIZ_RX_BUFFER_SIZE];
} WizTcpConnectionManager;

static UA_DateTime pico_datetime_now(void) {
    return UA_DATETIME_UNIX_EPOCH + (UA_DateTime)(time_us_64() * UA_DATETIME_USEC);
}

UA_DateTime UA_DateTime_now(void) {
    return pico_datetime_now();
}

UA_DateTime UA_DateTime_nowMonotonic(void) {
    return pico_datetime_now();
}

UA_Int64 UA_DateTime_localTimeUtcOffset(void) {
    return 0;
}

static void set_eventloop_state(UA_EventLoop *el, UA_EventLoopState state) {
    *(UA_EventLoopState *)(uintptr_t)&el->state = state;
}

static void wiz_noop_lock(UA_EventLoop *el) {
    (void)el;
}

static void wiz_noop_unlock(UA_EventLoop *el) {
    (void)el;
}

static UA_StatusCode wiz_eventsource_start(UA_EventSource *es) {
    es->state = UA_EVENTSOURCESTATE_STARTED;
    return UA_STATUSCODE_GOOD;
}

static void wiz_eventsource_stop(UA_EventSource *es) {
    es->state = UA_EVENTSOURCESTATE_STOPPED;
}

static UA_StatusCode wiz_cm_free(UA_EventSource *es);
static void wiz_cm_poll(WizTcpConnectionManager *tcp);

static UA_StatusCode wiz_el_register_eventsource(UA_EventLoop *el,
                                                   UA_EventSource *es) {
    if(!el || !es)
        return UA_STATUSCODE_BADINVALIDARGUMENT;

    es->eventLoop = el;
    es->next = el->eventSources;
    el->eventSources = es;

    if(el->state == UA_EVENTLOOPSTATE_STARTED)
        return es->start(es);

    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode wiz_el_deregister_eventsource(UA_EventLoop *el,
                                                     UA_EventSource *es) {
    if(!el || !es)
        return UA_STATUSCODE_BADINVALIDARGUMENT;

    if(es->state == UA_EVENTSOURCESTATE_STARTED)
        es->stop(es);

    UA_EventSource **cursor = &el->eventSources;
    while(*cursor) {
        if(*cursor == es) {
            *cursor = es->next;
            es->next = NULL;
            es->eventLoop = NULL;
            return UA_STATUSCODE_GOOD;
        }
        cursor = &(*cursor)->next;
    }

    return UA_STATUSCODE_BADNOTFOUND;
}

static UA_StatusCode wiz_el_start(UA_EventLoop *el) {
    if(!el)
        return UA_STATUSCODE_BADINVALIDARGUMENT;

    if(el->state == UA_EVENTLOOPSTATE_STARTED)
        return UA_STATUSCODE_GOOD;

    set_eventloop_state(el, UA_EVENTLOOPSTATE_STARTED);

    for(UA_EventSource *es = el->eventSources; es; es = es->next) {
        UA_StatusCode res = es->start(es);
        if(res != UA_STATUSCODE_GOOD)
            return res;
    }

    return UA_STATUSCODE_GOOD;
}

static void wiz_el_stop(UA_EventLoop *el) {
    if(!el)
        return;

    for(UA_EventSource *es = el->eventSources; es; es = es->next)
        es->stop(es);

    set_eventloop_state(el, UA_EVENTLOOPSTATE_STOPPED);
}

static UA_StatusCode wiz_el_free(UA_EventLoop *el) {
    if(!el)
        return UA_STATUSCODE_BADINVALIDARGUMENT;

    if(el->state == UA_EVENTLOOPSTATE_STARTED)
        wiz_el_stop(el);

    UA_EventSource *es = el->eventSources;
    while(es) {
        UA_EventSource *next = es->next;
        es->free(es);
        es = next;
    }

    UA_KeyValueMap_clear(&el->params);
    UA_free(el);
    return UA_STATUSCODE_GOOD;
}

static void wiz_run_delayed_callbacks(WizEventLoop *wel) {
    UA_DelayedCallback *dc = wel->delayedCallbacks;
    wel->delayedCallbacks = NULL;

    while(dc) {
        UA_DelayedCallback *next = dc->next;
        dc->next = NULL;
        if(dc->callback)
            dc->callback(dc->application, dc->context);
        dc = next;
    }
}

static void wiz_run_timers(WizEventLoop *wel) {
    UA_DateTime now = pico_datetime_now();

    for(size_t i = 0; i < WIZ_TIMER_MAX; i++) {
        WizTimerSlot *slot = &wel->timers[i];
        if(!slot->active || slot->nextTime > now)
            continue;

        UA_Callback cb = slot->callback;
        void *application = slot->application;
        void *context = slot->context;

        if(slot->policy == UA_TIMERPOLICY_ONCE) {
            slot->active = false;
        } else if(slot->policy == UA_TIMERPOLICY_BASETIME) {
            UA_DateTime interval =
                (UA_DateTime)(slot->intervalMs * (UA_Double)UA_DATETIME_MSEC);
            do {
                slot->nextTime += interval;
            } while(slot->nextTime <= now);
        } else {
            slot->nextTime = now +
                (UA_DateTime)(slot->intervalMs * (UA_Double)UA_DATETIME_MSEC);
        }

        if(cb)
            cb(application, context);
    }
}

static UA_StatusCode wiz_el_run(UA_EventLoop *el, UA_UInt32 timeout) {
    (void)timeout;
    WizEventLoop *wel = (WizEventLoop *)el;

    if(!el || el->state != UA_EVENTLOOPSTATE_STARTED)
        return UA_STATUSCODE_BADINTERNALERROR;

    wiz_run_delayed_callbacks(wel);
    wiz_run_timers(wel);

    for(UA_EventSource *es = el->eventSources; es; es = es->next) {
        if(es->eventSourceType != UA_EVENTSOURCETYPE_CONNECTIONMANAGER)
            continue;

        UA_ConnectionManager *cm = (UA_ConnectionManager *)es;
        if(cm->sendWithConnection)
            wiz_cm_poll((WizTcpConnectionManager *)cm);
    }

    return UA_STATUSCODE_GOOD;
}

static void wiz_el_cancel(UA_EventLoop *el) {
    (void)el;
}

static UA_DateTime wiz_el_now(UA_EventLoop *el) {
    (void)el;
    return pico_datetime_now();
}

static UA_Int64 wiz_el_utc_offset(UA_EventLoop *el) {
    (void)el;
    return 0;
}

static UA_DateTime wiz_el_next_timer(UA_EventLoop *el) {
    WizEventLoop *wel = (WizEventLoop *)el;
    UA_DateTime next = UA_INT64_MAX;

    if(wel->delayedCallbacks)
        return pico_datetime_now();

    for(size_t i = 0; i < WIZ_TIMER_MAX; i++) {
        if(wel->timers[i].active && wel->timers[i].nextTime < next)
            next = wel->timers[i].nextTime;
    }

    return next;
}

static UA_StatusCode wiz_el_add_timer(UA_EventLoop *el, UA_Callback cb,
                                        void *application, void *data,
                                        UA_Double intervalMs,
                                        UA_DateTime *baseTime,
                                        UA_TimerPolicy timerPolicy,
                                        UA_UInt64 *timerId) {
    WizEventLoop *wel = (WizEventLoop *)el;

    for(size_t i = 0; i < WIZ_TIMER_MAX; i++) {
        WizTimerSlot *slot = &wel->timers[i];
        if(slot->active)
            continue;

        UA_DateTime base = baseTime ? *baseTime : pico_datetime_now();
        UA_DateTime interval =
            (UA_DateTime)(intervalMs * (UA_Double)UA_DATETIME_MSEC);

        slot->active = true;
        slot->id = ++wel->nextTimerId;
        slot->callback = cb;
        slot->application = application;
        slot->context = data;
        slot->intervalMs = intervalMs;
        slot->policy = timerPolicy;
        slot->nextTime = (timerPolicy == UA_TIMERPOLICY_ONCE && baseTime) ?
            base : base + interval;

        if(timerId)
            *timerId = slot->id;

        return UA_STATUSCODE_GOOD;
    }

    return UA_STATUSCODE_BADOUTOFMEMORY;
}

static UA_StatusCode wiz_el_modify_timer(UA_EventLoop *el,
                                           UA_UInt64 timerId,
                                           UA_Double intervalMs,
                                           UA_DateTime *baseTime,
                                           UA_TimerPolicy timerPolicy) {
    WizEventLoop *wel = (WizEventLoop *)el;

    for(size_t i = 0; i < WIZ_TIMER_MAX; i++) {
        WizTimerSlot *slot = &wel->timers[i];
        if(!slot->active || slot->id != timerId)
            continue;

        UA_DateTime base = baseTime ? *baseTime : pico_datetime_now();
        slot->intervalMs = intervalMs;
        slot->policy = timerPolicy;
        slot->nextTime = base +
            (UA_DateTime)(intervalMs * (UA_Double)UA_DATETIME_MSEC);
        return UA_STATUSCODE_GOOD;
    }

    return UA_STATUSCODE_BADNOTFOUND;
}

static void wiz_el_remove_timer(UA_EventLoop *el, UA_UInt64 timerId) {
    WizEventLoop *wel = (WizEventLoop *)el;

    for(size_t i = 0; i < WIZ_TIMER_MAX; i++) {
        if(wel->timers[i].active && wel->timers[i].id == timerId) {
            memset(&wel->timers[i], 0, sizeof(wel->timers[i]));
            return;
        }
    }
}

static void wiz_el_add_delayed(UA_EventLoop *el, UA_DelayedCallback *dc) {
    WizEventLoop *wel = (WizEventLoop *)el;

    dc->next = NULL;
    if(!wel->delayedCallbacks) {
        wel->delayedCallbacks = dc;
        return;
    }

    UA_DelayedCallback *tail = wel->delayedCallbacks;
    while(tail->next)
        tail = tail->next;
    tail->next = dc;
}

static void wiz_el_remove_delayed(UA_EventLoop *el, UA_DelayedCallback *dc) {
    WizEventLoop *wel = (WizEventLoop *)el;
    UA_DelayedCallback **cursor = &wel->delayedCallbacks;

    while(*cursor) {
        if(*cursor == dc) {
            *cursor = dc->next;
            dc->next = NULL;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

UA_EventLoop *UA_EventLoop_new_LWIP(const UA_Logger *logger,
                                    UA_EventLoopConfiguration *config) {
    (void)config;

    WizEventLoop *wel = (WizEventLoop *)UA_calloc(1, sizeof(WizEventLoop));
    if(!wel)
        return NULL;

    UA_EventLoop *el = &wel->eventLoop;
    el->logger = logger;
    set_eventloop_state(el, UA_EVENTLOOPSTATE_FRESH);
    el->start = wiz_el_start;
    el->stop = wiz_el_stop;
    el->free = wiz_el_free;
    el->run = wiz_el_run;
    el->cancel = wiz_el_cancel;
    el->dateTime_now = wiz_el_now;
    el->dateTime_nowMonotonic = wiz_el_now;
    el->dateTime_localTimeUtcOffset = wiz_el_utc_offset;
    el->nextTimer = wiz_el_next_timer;
    el->addTimer = wiz_el_add_timer;
    el->modifyTimer = wiz_el_modify_timer;
    el->removeTimer = wiz_el_remove_timer;
    el->addDelayedCallback = wiz_el_add_delayed;
    el->removeDelayedCallback = wiz_el_remove_delayed;
    el->registerEventSource = wiz_el_register_eventsource;
    el->deregisterEventSource = wiz_el_deregister_eventsource;
    el->lock = wiz_noop_lock;
    el->unlock = wiz_noop_unlock;

    return el;
}

static int wiz_send_all(const uint8_t *buf, uint16_t len) {
    uint16_t sentTotal = 0u;

    while(sentTotal < len) {
        int32_t sent = send(WIZ_UA_SOCKET, (uint8_t *)buf + sentTotal,
                            (uint16_t)(len - sentTotal));
        if(sent <= 0)
            return -1;

        sentTotal = (uint16_t)(sentTotal + (uint16_t)sent);
    }

    return 0;
}

static void wiz_signal_listen(WizTcpConnectionManager *tcp) {
    if(tcp->listenAnnounced || !tcp->callback)
        return;

    UA_String listenAddress = UA_STRING_STATIC("0.0.0.0");
    UA_KeyValuePair params[2];
    params[0].key = UA_QUALIFIEDNAME(0, "listen-address");
    UA_Variant_setScalar(&params[0].value, &listenAddress,
                         &UA_TYPES[UA_TYPES_STRING]);
    params[1].key = UA_QUALIFIEDNAME(0, "listen-port");
    UA_Variant_setScalar(&params[1].value, &tcp->port,
                         &UA_TYPES[UA_TYPES_UINT16]);
    UA_KeyValueMap kvm = {2, params};

    tcp->callback(&tcp->cm, WIZ_LISTEN_CONNECTION_ID, tcp->application,
                  &tcp->listenContext, UA_CONNECTIONSTATE_ESTABLISHED,
                  &kvm, UA_BYTESTRING_NULL);
    tcp->listenAnnounced = true;
}

static void wiz_signal_active_open(WizTcpConnectionManager *tcp) {
    if(tcp->activeAnnounced || !tcp->callback)
        return;

    getSn_DIPR(WIZ_UA_SOCKET, tcp->peerIp);
    tcp->peerPort = getSn_DPORT(WIZ_UA_SOCKET);

    char remote[24];
    snprintf(remote, sizeof(remote), "%u.%u.%u.%u",
             tcp->peerIp[0], tcp->peerIp[1], tcp->peerIp[2], tcp->peerIp[3]);

    UA_String remoteAddress = UA_STRING(remote);
    UA_KeyValuePair kvp;
    kvp.key = UA_QUALIFIEDNAME(0, "remote-address");
    UA_Variant_setScalar(&kvp.value, &remoteAddress, &UA_TYPES[UA_TYPES_STRING]);
    UA_KeyValueMap kvm = {1, &kvp};

    tcp->activeContext = tcp->listenContext;
    tcp->callback(&tcp->cm, WIZ_ACTIVE_CONNECTION_ID, tcp->application,
                  &tcp->activeContext, UA_CONNECTIONSTATE_ESTABLISHED,
                  &kvm, UA_BYTESTRING_NULL);
    tcp->activeAnnounced = true;
}

static void wiz_queue_active_close(WizTcpConnectionManager *tcp) {
    tcp->pendingActiveClose = true;
}

static void wiz_process_pending_close(WizTcpConnectionManager *tcp) {
    if(tcp->pendingActiveClose) {
        tcp->pendingActiveClose = false;
        if(tcp->activeAnnounced && tcp->callback) {
            tcp->callback(&tcp->cm, WIZ_ACTIVE_CONNECTION_ID, tcp->application,
                          &tcp->activeContext, UA_CONNECTIONSTATE_CLOSING,
                          &UA_KEYVALUEMAP_NULL, UA_BYTESTRING_NULL);
        }
        tcp->activeAnnounced = false;
        tcp->activeContext = NULL;
        close(WIZ_UA_SOCKET);
        return;
    }

    if(tcp->pendingListenClose) {
        tcp->pendingListenClose = false;
        if(tcp->listenAnnounced && tcp->callback) {
            tcp->callback(&tcp->cm, WIZ_LISTEN_CONNECTION_ID, tcp->application,
                          &tcp->listenContext, UA_CONNECTIONSTATE_CLOSING,
                          &UA_KEYVALUEMAP_NULL, UA_BYTESTRING_NULL);
        }
        tcp->listenAnnounced = false;
        tcp->listenContext = NULL;
        close(WIZ_UA_SOCKET);
    }
}

static void wiz_open_listen_socket(WizTcpConnectionManager *tcp) {
    uint8_t sr = getSn_SR(WIZ_UA_SOCKET);

    switch(sr) {
    case SOCK_CLOSED:
        if(socket(WIZ_UA_SOCKET, Sn_MR_TCP, tcp->port, SF_TCP_NODELAY) < 0)
            return;
        break;

    case SOCK_INIT:
        if(listen(WIZ_UA_SOCKET) == SOCK_OK)
            wiz_signal_listen(tcp);
        break;

    case SOCK_LISTEN:
        wiz_signal_listen(tcp);
        break;

    default:
        break;
    }
}

static void wiz_cm_poll(WizTcpConnectionManager *tcp) {
    if(tcp->cm.eventSource.state != UA_EVENTSOURCESTATE_STARTED)
        return;

    wiz_process_pending_close(tcp);
    if(tcp->pendingActiveClose || tcp->pendingListenClose)
        return;

    uint8_t sr = getSn_SR(WIZ_UA_SOCKET);

    if(sr == SOCK_ESTABLISHED) {
        wiz_signal_active_open(tcp);

        uint16_t rxSize = getSn_RX_RSR(WIZ_UA_SOCKET);
        if(rxSize > 0u) {
            if(rxSize > sizeof(tcp->rxBuffer))
                rxSize = sizeof(tcp->rxBuffer);

            int32_t received = recv(WIZ_UA_SOCKET, tcp->rxBuffer, rxSize);
            if(received > 0 && tcp->activeAnnounced && tcp->callback) {
                UA_ByteString msg;
                msg.length = (size_t)received;
                msg.data = tcp->rxBuffer;
                tcp->callback(&tcp->cm, WIZ_ACTIVE_CONNECTION_ID,
                              tcp->application, &tcp->activeContext,
                              UA_CONNECTIONSTATE_ESTABLISHED,
                              &UA_KEYVALUEMAP_NULL, msg);
            } else if(received < 0) {
                wiz_queue_active_close(tcp);
            }
        }
        return;
    }

    if(sr == SOCK_CLOSE_WAIT) {
        disconnect(WIZ_UA_SOCKET);
        wiz_queue_active_close(tcp);
        return;
    }

    if(tcp->activeAnnounced &&
       (sr == SOCK_CLOSED || sr == SOCK_INIT || sr == SOCK_LISTEN)) {
        wiz_queue_active_close(tcp);
        return;
    }

    wiz_open_listen_socket(tcp);
}

static UA_StatusCode wiz_cm_start(UA_EventSource *es) {
    es->state = UA_EVENTSOURCESTATE_STARTED;
    return UA_STATUSCODE_GOOD;
}

static void wiz_cm_stop(UA_EventSource *es) {
    WizTcpConnectionManager *tcp = (WizTcpConnectionManager *)es;

    tcp->pendingActiveClose = tcp->activeAnnounced;
    tcp->pendingListenClose = tcp->listenAnnounced;
    wiz_process_pending_close(tcp);
    es->state = UA_EVENTSOURCESTATE_STOPPED;
}

static UA_StatusCode wiz_cm_free(UA_EventSource *es) {
    if(!es)
        return UA_STATUSCODE_BADINVALIDARGUMENT;

    if(es->state == UA_EVENTSOURCESTATE_STARTED)
        wiz_cm_stop(es);

    UA_String_clear(&es->name);
    UA_KeyValueMap_clear(&es->params);
    UA_free(es);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
wiz_cm_open_connection(UA_ConnectionManager *cm, const UA_KeyValueMap *params,
                         void *application, void *context,
                         UA_ConnectionManager_connectionCallback callback) {
    WizTcpConnectionManager *tcp = (WizTcpConnectionManager *)cm;
    const UA_UInt16 *port = (const UA_UInt16 *)UA_KeyValueMap_getScalar(
        params, UA_QUALIFIEDNAME(0, "port"), &UA_TYPES[UA_TYPES_UINT16]);
    const UA_Boolean *listenFlag = (const UA_Boolean *)UA_KeyValueMap_getScalar(
        params, UA_QUALIFIEDNAME(0, "listen"), &UA_TYPES[UA_TYPES_BOOLEAN]);

    if(!port || !listenFlag || !*listenFlag)
        return UA_STATUSCODE_BADNOTIMPLEMENTED;

    tcp->port = *port;
    tcp->application = application;
    tcp->listenContext = context;
    tcp->activeContext = NULL;
    tcp->callback = callback;
    tcp->listenAnnounced = false;
    tcp->activeAnnounced = false;
    tcp->pendingListenClose = false;
    tcp->pendingActiveClose = false;

    close(WIZ_UA_SOCKET);
    wiz_open_listen_socket(tcp);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
wiz_cm_send(UA_ConnectionManager *cm, uintptr_t connectionId,
              const UA_KeyValueMap *params, UA_ByteString *buf) {
    (void)params;
    WizTcpConnectionManager *tcp = (WizTcpConnectionManager *)cm;
    UA_StatusCode result = UA_STATUSCODE_GOOD;

    if(connectionId != WIZ_ACTIVE_CONNECTION_ID || !buf || !buf->data) {
        result = UA_STATUSCODE_BADCONNECTIONCLOSED;
        goto out;
    }

    if(wiz_send_all(buf->data, (uint16_t)buf->length) != 0) {
        result = UA_STATUSCODE_BADCONNECTIONCLOSED;
        wiz_queue_active_close(tcp);
    }

out:
    if(buf)
        UA_ByteString_clear(buf);
    return result;
}

static UA_StatusCode wiz_cm_close(UA_ConnectionManager *cm,
                                    uintptr_t connectionId) {
    WizTcpConnectionManager *tcp = (WizTcpConnectionManager *)cm;

    if(connectionId == WIZ_ACTIVE_CONNECTION_ID) {
        disconnect(WIZ_UA_SOCKET);
        wiz_queue_active_close(tcp);
        return UA_STATUSCODE_GOOD;
    }

    if(connectionId == WIZ_LISTEN_CONNECTION_ID) {
        tcp->pendingListenClose = true;
        return UA_STATUSCODE_GOOD;
    }

    return UA_STATUSCODE_BADNOTFOUND;
}

static UA_StatusCode
wiz_cm_alloc_buffer(UA_ConnectionManager *cm, uintptr_t connectionId,
                      UA_ByteString *buf, size_t bufSize) {
    (void)cm;
    (void)connectionId;
    return UA_ByteString_allocBuffer(buf, bufSize);
}

static void wiz_cm_free_buffer(UA_ConnectionManager *cm,
                                 uintptr_t connectionId,
                                 UA_ByteString *buf) {
    (void)cm;
    (void)connectionId;
    UA_ByteString_clear(buf);
}

UA_ConnectionManager *
UA_ConnectionManager_new_LWIP_TCP(const UA_String eventSourceName) {
    WizTcpConnectionManager *tcp =
        (WizTcpConnectionManager *)UA_calloc(1, sizeof(WizTcpConnectionManager));
    if(!tcp)
        return NULL;

    UA_ConnectionManager *cm = &tcp->cm;
    UA_String_copy(&eventSourceName, &cm->eventSource.name);
    cm->eventSource.eventSourceType = UA_EVENTSOURCETYPE_CONNECTIONMANAGER;
    cm->eventSource.state = UA_EVENTSOURCESTATE_FRESH;
    cm->eventSource.start = wiz_cm_start;
    cm->eventSource.stop = wiz_cm_stop;
    cm->eventSource.free = wiz_cm_free;
    cm->protocol = UA_STRING("tcp");
    cm->openConnection = wiz_cm_open_connection;
    cm->sendWithConnection = wiz_cm_send;
    cm->closeConnection = wiz_cm_close;
    cm->allocNetworkBuffer = wiz_cm_alloc_buffer;
    cm->freeNetworkBuffer = wiz_cm_free_buffer;
    tcp->port = WIZ_OPCUA_TCP_PORT;

    return cm;
}

UA_ConnectionManager *
UA_ConnectionManager_new_LWIP_UDP(const UA_String eventSourceName) {
    (void)eventSourceName;
    return NULL;
}
