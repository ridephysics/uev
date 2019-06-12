#include <uev/uev.h>
#include "list.h"

#include <lwip/opt.h>
#include <sys/select.h>
#include <sys/socket.h>

#define CROSSLOG_TAG "uev"
#include <crosslog.h>

static TaskHandle_t task = NULL;
static int fd_local = -1;
static struct sockaddr_in sa_local;
static struct uev_list_node list = LIST_INITIAL_VALUE(list);

static int locsock_create(struct sockaddr_in *sa){
	int fd;
	int rc;
	socklen_t socklen;

	fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) {
		CROSSLOG_ERRNO("socket");
		return -1;
	}

	memset(sa, 0, sizeof(*sa));
	sa->sin_family = AF_INET;
	sa->sin_port = 0;
	sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	rc = bind(fd, (struct sockaddr *)sa, sizeof(*sa));
	if (rc < -1) {
		CROSSLOG_ERRNO("bind");
		close(fd);
		return -1;
	}

	socklen = sizeof(*sa);
	rc = getsockname(fd, (struct sockaddr *)sa, &socklen);
	if (rc < -1) {
		CROSSLOG_ERRNO("getsockname");
		close(fd);
		return -1;
	}

	return fd;
}

void task_fn(void * ctx) {
	int rc;
	int maxfd;
	uev_t *w;
	fd_set readfds;
	fd_set writefds;
	fd_set exceptfds;

	CROSSLOGV("iothread");

	for (;;) {
		FD_ZERO(&readfds);
		FD_ZERO(&writefds);
		FD_ZERO(&exceptfds);

		FD_SET(fd_local, &readfds);
		maxfd = fd_local;

		_uev_critical_enter();
		list_for_every_entry(&list, w, uev_t, iot.node) {
			if (!_uev_watcher_active(w))
				continue;
			if (w->fd < 0)
				continue;
			if (atomic_load(&w->iot.events))
				continue;

			if (w->fd > maxfd)
				maxfd = w->fd;

			if (w->events & UEV_READ)
				FD_SET(w->fd, &readfds);

			if (w->events & UEV_WRITE)
				FD_SET(w->fd, &writefds);

			if (w->events & UEV_ERROR)
				FD_SET(w->fd, &exceptfds);
		}
		_uev_critical_exit();

		rc = select(maxfd + 1, &readfds, &writefds, &exceptfds, NULL);
		if (rc < 0) {
			if (errno == EINTR)
				continue;

			CROSSLOG_ERRNO("select");
			vTaskDelay(1000 / portTICK_RATE_MS);
			continue;
		}

		// we don't use timeouts but let's just go along with it
		if (rc == 0)
			continue;

		if (FD_ISSET(fd_local, &exceptfds)) {
			CROSSLOGE("local socket error");
		}

		if (FD_ISSET(fd_local, &readfds)) {
			uint8_t b;

			for (;;) {
				ssize_t nbytes = read(fd_local, &b, sizeof(b));
				if (nbytes < 0) {
					if (errno == EINTR)
						continue;
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						break;

					CROSSLOG_ERRNO("read");
					goto task_end;
				}
				if (nbytes == 0) {
					CROSSLOGE("local socket got closed");
					goto task_end;
				}

				break;
			}
		}

		_uev_critical_enter();
		list_for_every_entry(&list, w, uev_t, iot.node) {
			unsigned int events = 0;

			if (!_uev_watcher_active(w))
				continue;
			if (w->fd < 0)
				continue;

			if (FD_ISSET(w->fd, &readfds) && w->events & UEV_READ)
				events |= UEV_READ;

			if (FD_ISSET(w->fd, &writefds) && w->events & UEV_WRITE)
				events |= UEV_WRITE;

			if (FD_ISSET(w->fd, &exceptfds) && w->events & UEV_ERROR)
				events |= UEV_ERROR;

			if (events) {
				atomic_fetch_or(&w->iot.events, events);
				_uev_set_flags(w->ctx, UEV_EG_BIT_IO);
			}
		}
		_uev_critical_exit();
	}

task_end:
	if (fd_local >= 0)
		close(fd_local);
	vTaskDelete(NULL);
}

/**
 * Initialize global iothread
 *
 * @return POSIX OK(0) on success, or non-zero on error.
 */
int uev_iothread_init(void) {
	BaseType_t xrc;

	CROSSLOG_ASSERT(!task);

	fd_local = locsock_create(&sa_local);
	if (fd_local < 0) {
		return -1;
	}

	xrc = xTaskCreate(task_fn, "uev_iothread", 4096, NULL, TCPIP_THREAD_PRIO, &task);
	if (xrc != pdPASS) {
		return -1;
	}

	return 0;
}

void _uev_iothread_watcher_add(uev_t *w) {
	_uev_critical_enter();
	list_add_tail(&list, &w->iot.node);
	_uev_critical_exit();

	_uev_iothread_interrupt();
}

void _uev_iothread_watcher_remove(uev_t *w) {
	_uev_critical_enter();
	list_delete(&w->iot.node);
	_uev_critical_exit();

	_uev_iothread_interrupt();
}

void _uev_iothread_interrupt(void) {
	uint8_t b = 0x01;
	sendto(fd_local, &b, sizeof(b), 0, (struct sockaddr *)&sa_local, sizeof(sa_local));
}
