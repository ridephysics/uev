/* libuEv - Micro event loop library
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

#include <errno.h>
#include <string.h>		/* memset() */

#include <uev/uev.h>
#include "list.h"

#ifndef portYIELD_FROM_ISR
#define portYIELD_FROM_ISR portYIELD
#endif

#ifndef CONFIG_TARGET_PLATFORM_ESP8266
static portMUX_TYPE _uev_mux = portMUX_INITIALIZER_UNLOCKED;
#endif

/* Private to libuEv, do not use directly! */
void _uev_critical_enter(void) {
#ifdef CONFIG_TARGET_PLATFORM_ESP8266
	portENTER_CRITICAL();
#else
	portENTER_CRITICAL(&_uev_mux);
#endif
}

/* Private to libuEv, do not use directly! */
void _uev_critical_exit(void) {
#ifdef CONFIG_TARGET_PLATFORM_ESP8266
	portEXIT_CRITICAL();
#else
	portEXIT_CRITICAL(&_uev_mux);
#endif
}

void _uev_set_flags(uev_ctx_t *ctx, const EventBits_t bits) {
	if (xPortInIsrContext()) {
		BaseType_t xHigherPriorityTaskWoken = pdFALSE;
		xEventGroupSetBitsFromISR(ctx->egh, bits, &xHigherPriorityTaskWoken);

		if (xHigherPriorityTaskWoken == pdTRUE) {
			portYIELD_FROM_ISR();
		}
	}
	else {
		xEventGroupSetBits(ctx->egh, bits);
	}
}

/* Private to libuEv, do not use directly! */
int _uev_watcher_init(uev_ctx_t *ctx, uev_t *w, uev_type_t type, uev_cb_t *cb, void *arg, int fd, int events)
{
	if (!ctx || !w) {
		errno = EINVAL;
		return -1;
	}

	w->ctx    = ctx;
	w->type   = type;
	w->active = 0;
	w->fd     = fd;
	w->cb     = cb;
	w->arg    = arg;
	w->events = events;

	atomic_init(&w->iot.events, 0);

	if (w->type == UEV_TIMER_TS_TYPE)
		_UEV_INSERT(w, w->ctx->watchers);

	return 0;
}

/* Private to libuEv, do not use directly! */
int _uev_watcher_start(uev_t *w)
{
	if (!w || !w->ctx) {
		errno = EINVAL;
		return -1;
	}

	if (w->type == UEV_IO_TYPE && w->fd < 0) {
		errno = EINVAL;
		return -1;
	}

	if (_uev_watcher_active(w))
		return 0;

	w->active = 1;

	if (w->type == UEV_IO_TYPE) {
		_uev_iothread_watcher_add(w);
	}

	if (w->type != UEV_TIMER_TS_TYPE) {
		/* Add to internal list for bookkeeping */
		_UEV_INSERT(w, w->ctx->watchers);
	}

	return 0;
}

/* Private to libuEv, do not use directly! */
int _uev_watcher_stop(uev_t *w)
{
	if (!w) {
		errno = EINVAL;
		return -1;
	}

	if (!_uev_watcher_active(w))
		return 0;

	w->active = 0;

	if (w->type == UEV_IO_TYPE) {
		_uev_iothread_watcher_remove(w);
	}

	if (w->type != UEV_TIMER_TS_TYPE) {
		/* Remove from internal list */
		_UEV_REMOVE(w, w->ctx->watchers);
	}

	return 0;
}

/* Private to libuEv, do not use directly! */
int _uev_watcher_active(uev_t *w)
{
	if (!w)
		return 0;

	return w->active > 0;
}

/**
 * Create an event loop context
 * @param ctx  Pointer to an uev_ctx_t context to be initialized
 *
 * @return POSIX OK(0) on success, or non-zero on error.
 */
int uev_init(uev_ctx_t *ctx)
{
	if (!ctx) {
		errno = EINVAL;
		return -1;
	}

	memset(ctx, 0, sizeof(*ctx));

#if configSUPPORT_STATIC_ALLOCATION
	ctx->egh = xEventGroupCreateStatic(&ctx->egb);
#else
	ctx->egh = xEventGroupCreate();
#endif
	configASSERT(ctx->egh);

	atomic_init(&ctx->running, 0);

	return 0;
}

/**
 * Terminate the event loop
 * @param ctx  A valid libuEv context
 *
 * @return POSIX OK(0) or non-zero with @param errno set on error.
 */
int uev_exit(uev_ctx_t *ctx)
{
	uev_t *w;

	if (!ctx) {
		errno = EINVAL;
		return -1;
	}

	_UEV_FOREACH(w, ctx->watchers) {
		/* Remove from internal list */
		_UEV_REMOVE(w, ctx->watchers);

		if (!_uev_watcher_active(w))
			continue;

		switch (w->type) {
		case UEV_IO_TYPE:
			uev_io_stop(w);
			break;

		case UEV_TIMER_TYPE:
		case UEV_TIMER_TS_TYPE:
			uev_timer_stop(w);
			break;

		case UEV_EVENT_TYPE:
			uev_event_stop(w);
			break;
		}
	}

	ctx->watchers = NULL;
	atomic_store(&ctx->running, 0);
	vEventGroupDelete(ctx->egh);

	return 0;
}

/**
 * Start the event loop
 * @param ctx    A valid libuEv context
 * @param flags  A mask of %UEV_ONCE and %UEV_NONBLOCK, or zero
 *
 * With @flags set to %UEV_ONCE the event loop returns after the first
 * event has been served, useful for instance to set a timeout on a file
 * descriptor.  If @flags also has the %UEV_NONBLOCK flag set the event
 * loop will return immediately if no event is pending, useful when run
 * inside another event loop.
 *
 * @return POSIX OK(0) upon successful termination of the event loop, or
 * non-zero on error.
 */
int uev_run(uev_ctx_t *ctx, int flags)
{
	uev_t *w;
	uint64_t next_deadline = 0xffffffffffffffff;

	if (!ctx || !ctx->egh) {
		errno = EINVAL;
		return -1;
	}

	if (flags & UEV_NONBLOCK)
		next_deadline = 0;

	/* Start the event loop */
	atomic_store(&ctx->running, 1);

	/* Start all dormant timers */
	_UEV_FOREACH(w, ctx->watchers) {
		if (w->type == UEV_TIMER_TYPE || w->type == UEV_TIMER_TS_TYPE) {
			uev_timer_set(w, w->u.t.timeout, w->u.t.period);

			if (w->u.t.deadline < next_deadline)
				next_deadline = w->u.t.deadline;
		}
	}

	while (atomic_load(&ctx->running)) {
		uint64_t now = _uev_timer_now() / 1000;
		TickType_t tickstowait;

		if (next_deadline == 0xffffffffffffffff)
			tickstowait = portMAX_DELAY;
		else if (now >= next_deadline)
			tickstowait = 0;
		else
			tickstowait = next_deadline ? ((next_deadline - now) / portTICK_PERIOD_MS) : 0;

		EventBits_t bits = xEventGroupWaitBits(ctx->egh, UEV_EG_MASK, pdTRUE, pdFALSE, tickstowait);
		next_deadline = 0xffffffffffffffff;

		_UEV_FOREACH(w, ctx->watchers) {
			bool runcb = false;
			int events = 0;

			if (!w->active)
				continue;

			switch (w->type) {
			case UEV_EVENT_TYPE:
				if (!(bits & UEV_EG_BIT_EVENT))
					break;

				if (atomic_exchange(&w->u.e.posted, false)) {
					runcb = true;
					events = UEV_READ;
				}
				break;

			case UEV_TIMER_TS_TYPE:
			case UEV_TIMER_TYPE: {
				uint64_t now = _uev_timer_now() / 1000;

				if (w->type == UEV_TIMER_TS_TYPE) {
					_uev_critical_enter();
				}

				if (now > 0 && w->u.t.deadline && now > w->u.t.deadline) {
					runcb = true;
					events = UEV_READ;

					if (!w->u.t.period)
						w->u.t.timeout = 0;

					if (!w->u.t.timeout)
						_uev_timer_stop(w);
					else {
						w->u.t.deadline = now + w->u.t.period;
					}
				}

				if (w->u.t.deadline && w->u.t.deadline < next_deadline)
					next_deadline = w->u.t.deadline;

				if (w->type == UEV_TIMER_TS_TYPE) {
					_uev_critical_exit();
				}
				break;
			}

			case UEV_IO_TYPE: {
				if (!(bits & UEV_EG_BIT_IO))
					break;

				unsigned int ioevents = atomic_load(&w->iot.events);
				if (ioevents) {
					events |= ioevents;
					runcb = true;
				}
				break;
			}

			default:
				continue;
			}

			if (runcb && w->cb) {
				w->cb(w, w->arg, events & UEV_EVENT_MASK);

				if (w->type == UEV_IO_TYPE) {
					atomic_fetch_and(&w->iot.events, ~((unsigned int)events));
					_uev_iothread_interrupt();
				}
			}
		}

		if (flags & UEV_ONCE)
			break;
	}

	return 0;
}
