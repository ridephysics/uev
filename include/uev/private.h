/* libuEv - Private methods and data types, do not use directly!
 *
 * Copyright (c) 2012       Flemming Madsen <flemming!madsen()madsensoft!dk>
 * Copyright (c) 2013-2019  Joachim Nilsson <troglobit()gmail!com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef LIBUEV_PRIVATE_H_
#define LIBUEV_PRIVATE_H_

#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <stdatomic.h>

/*
 * List functions.
 */
#define _UEV_FOREACH(node, list)		\
	for (typeof (node) next, node = list;	\
	     node && (next = node->next, 1);	\
	     node = next)

#define _UEV_INSERT(node, list) do {		\
	typeof (node) next;			\
	next       = list;			\
	list       = node;			\
	if (next)				\
		next->prev = node;		\
	node->next = next;			\
	node->prev = NULL;			\
} while (0)

#define _UEV_REMOVE(node, list) do {		\
	typeof (node) prev, next;		\
	prev = node->prev;			\
	next = node->next;			\
	if (prev)				\
		prev->next = next;		\
	if (next)				\
		next->prev = prev;		\
	node->prev = NULL;			\
	node->next = NULL;			\
	if (list == node)			\
		list = next;			\
} while (0)

/* I/O, timer, or signal watcher */
typedef enum {
	UEV_IO_TYPE = 1,
	UEV_TIMER_TYPE,
	UEV_TIMER_TS_TYPE,
	UEV_EVENT_TYPE,
} uev_type_t;

struct uev_list_node {
    struct uev_list_node *prev;
    struct uev_list_node *next;
};

/* Event mask, used internally only. */
#define UEV_EVENT_MASK  (UEV_ERROR | UEV_READ | UEV_WRITE)

/* eventgroup flags */
#define UEV_EG_BIT_IO (1 << 0)
#define UEV_EG_BIT_EVENT (1 << 1)
#define UEV_EG_BIT_TIMER (1 << 2)
#define UEV_EG_MASK (UEV_EG_BIT_IO | UEV_EG_BIT_EVENT | UEV_EG_BIT_TIMER)

/* Main libuEv context type */
typedef struct {
	atomic_int         running;
	EventGroupHandle_t egh;
#if configSUPPORT_STATIC_ALLOCATION
	StaticEventGroup_t egb;
#endif
	struct uev     *watchers;
	int             watchers_changed;
} uev_ctx_t;

/* Forward declare due to dependencys, don't try this at home kids. */
struct uev;

/* This is used to hide all private data members in uev_t */
#define uev_private_t                                           \
	struct uev     *next, *prev;				\
	struct {						\
		struct uev_list_node node;			\
		atomic_uint events;				\
	} iot;							\
								\
	int             active;                                 \
	int             events;                                 \
								\
	/* Watcher callback with optional argument */           \
	void          (*cb)(struct uev *, void *, int);         \
	void           *arg;                                    \
								\
								\
	/* Arguments for different watchers */			\
	union {							\
		/* Timer watchers, time in milliseconds */	\
		struct {					\
			int timeout;				\
			int period;				\
			uint64_t deadline;			\
		} t;						\
								\
		/* Event watchers */	\
		struct {					\
			atomic_int posted;			\
		} e;						\
	} u;							\
								\
	/* Watcher type */					\
	uev_type_t

/* Internal API for dealing with generic watchers */
int _uev_watcher_init  (uev_ctx_t *ctx, struct uev *w, uev_type_t type,
			void (*cb)(struct uev *, void *, int), void *arg,
			int fd, int events);
int _uev_watcher_start (struct uev *w);
int _uev_watcher_stop  (struct uev *w);
int _uev_watcher_active(struct uev *w);
int _uev_watcher_rearm (struct uev *w);

/* Internal iothread API */
void _uev_iothread_watcher_add(struct uev *w);
void _uev_iothread_watcher_remove(struct uev *w);
void _uev_iothread_interrupt(void);

/* Internal timer API */
uint64_t _uev_timer_now(void);
int _uev_timer_stop(struct uev *w);

/* Internal API for locks */
void _uev_critical_enter(void);
void _uev_critical_exit(void);

/* Internal API for setting flags */
void _uev_set_flags(uev_ctx_t *ctx, const EventBits_t bits);

#endif /* LIBUEV_PRIVATE_H_ */

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
