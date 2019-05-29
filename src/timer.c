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

#include <uev/uev.h>

#ifdef CONFIG_TARGET_PLATFORM_ESP8266
uint64_t esp8266_get_time_since_boot(void);
#endif

/**
 * Get the uptime of the system
 *
 * @return the uptime in microseconds.
 */
uint64_t _uev_timer_now(void) {
#ifdef CONFIG_TARGET_PLATFORM_ESP8266
	return esp8266_get_time_since_boot();
#else
	int64_t now = esp_timer_get_time();
	if (now <= 0)
		return 0;
	return (uint64_t) now;
#endif
}


/**
 * Create and start a timer watcher
 * @param ctx         A valid libuEv context
 * @param w           Pointer to an uev_t watcher
 * @param cb          Callback function
 * @param arg         Optional callback argument
 * @param timeout     Timeout in milliseconds before @param cb is called
 * @param period      For periodic timers this is the period time that @param timeout is reset to
 * @param threadsafe  Make this timer threadsafe. The processing will be slower.
 *
 * For one-shot timers you set @param period to zero and only use @param
 * timeout.  For periodic timers you likely set @param timeout to either
 * zero, to call it as soon as the event loop starts, or to the same
 * value as @param period.  When the timer expires, the @param cb
 * is called, with the optional @param arg argument.  A non-periodic
 * timer ends its life there, while a periodic task's @param timeout is
 * reset to the @param period and restarted.
 *
 * A timer is automatically started if the event loop is already
 * running, otherwise it is kept on hold until triggered by calling
 * uev_run().
 *
 * @return POSIX OK(0) or non-zero with @param errno set on error.
 */
int uev_timer_init2(uev_ctx_t *ctx, uev_t *w, uev_cb_t *cb, void *arg, int timeout, int period, int threadsafe)
{
	if (timeout < 0 || period < 0) {
		errno = ERANGE;
		return -1;
	}

	_uev_lock_init(&w->u.t.lock);

	if (_uev_watcher_init(ctx, w, threadsafe ? UEV_TIMER_TS_TYPE : UEV_TIMER_TYPE, cb, arg, -1, UEV_READ))
		return -1;

	if (uev_timer_set(w, timeout, period)) {
		_uev_watcher_stop(w);
		return -1;
	}

	return 0;
}

/**
 * Create and start a timer watcher
 * @param ctx         A valid libuEv context
 * @param w           Pointer to an uev_t watcher
 * @param cb          Callback function
 * @param arg         Optional callback argument
 * @param timeout     Timeout in milliseconds before @param cb is called
 * @param period      For periodic timers this is the period time that @param timeout is reset to
 *
 * This function calls @func uev_timer_init2() with @param threadsafe set to 0
 *
 * @return POSIX OK(0) or non-zero with @param errno set on error.
 */
int uev_timer_init(uev_ctx_t *ctx, uev_t *w, uev_cb_t *cb, void *arg, int timeout, int period) {
	return uev_timer_init2(ctx, w, cb, arg, timeout, period, 0);
}

/**
 * Reset a timer
 * @param w        Watcher to reset
 * @param timeout  Timeout in milliseconds before @param cb is called, zero disarms timer
 * @param period   For periodic timers this is the period time that @param timeout is reset to
 *
 * Note, the @param timeout value must be non-zero.  Setting it to zero
 * will disarm the timer.  This is the underlying Linux function @func
 * timerfd_settimer() which has this behavior.
 *
 * @return POSIX OK(0) or non-zero with @param errno set on error.
 */
int uev_timer_set(uev_t *w, int timeout, int period)
{
	/* Every watcher must be registered to a context */
	if (!w || !w->ctx) {
		errno = EINVAL;
		return -1;
	}

	if (timeout < 0 || period < 0) {
		errno = ERANGE;
		return -1;
	}

	uint64_t now = _uev_timer_now();

	if (w->type == UEV_TIMER_TS_TYPE) {
		_uev_lock(&w->u.t.lock);
	}

	w->u.t.timeout = timeout;
	w->u.t.period  = period;

	if (atomic_load(&w->ctx->running) && w->u.t.timeout) {
		w->u.t.deadline = now / 1000 + timeout;
	}
	else {
		w->u.t.deadline  = 0;
	}

	if (w->type == UEV_TIMER_TS_TYPE) {
		_uev_unlock(&w->u.t.lock);
	}

	// in case this is run from another thread or an ISR, we need to wake up
	// the event loop to recalculate the timers
	_uev_set_flags(w->ctx, UEV_EG_BIT_TIMER);

	return _uev_watcher_start(w);
}

/**
 * Start a stopped timer watcher
 * @param w  Watcher to start (again)
 *
 * @return POSIX OK(0) or non-zero with @param errno set on error.
 */
int uev_timer_start(uev_t *w)
{
	if (!w) {
		errno = EINVAL;
		return -1;
	}

	if (-1 != w->fd)
		_uev_watcher_stop(w);

	return uev_timer_set(w, w->u.t.timeout, w->u.t.period);
}

/* Private to libuEv, do not use directly! */
int _uev_timer_stop(uev_t *w)
{
	if (!_uev_watcher_active(w))
		return 0;

	if (_uev_watcher_stop(w))
		return -1;

	return 0;
}

/**
 * Stop and unregister a timer watcher
 * @param w  Watcher to stop
 *
 * @return POSIX OK(0) or non-zero with @param errno set on error.
 */
int uev_timer_stop(uev_t *w)
{
	int rc;

	rc = _uev_timer_stop(w);
	if (rc)
		return rc;

	/* Remove from internal list */
	_UEV_REMOVE(w, w->ctx->watchers);

	return 0;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
