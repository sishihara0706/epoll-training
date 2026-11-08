#define _GNU_SOURCE

#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/socket.h>
#include<unistd.h>
#include<netinet/in.h>
#include<sys/epoll.h>
#include<errno.h>
#include<signal.h>
#include<sys/timerfd.h>
#include<sys/signalfd.h>
#include<time.h>

#include "log.h"

#define PORT				8080
#define EVENT_NUM			1000
#define BUFF_SIZE			4096
#define LISTEN_NUM			128
#ifndef IDLE_TIMEOUT_SECONDS
#define IDLE_TIMEOUT_SECONDS 60
#endif

#define QUEUE_HIGH_WATER	(64 * 1024)
#define QUEUE_LOW_WATER 	(32 * 1024)
#define QUEUE_MAX_BYTES		(256 * 1024)
#define QUEUE_MAX_MESSAGES	1024

static int active_clients;
static struct st_client *clients;

enum fd_type
{
	FD_LISTENER,
	FD_TIMER,
	FD_SIGNAL,
	FD_CLIENT,
	FD_METRICS_LISTENER,
	FD_HTTP_CLIENT
};

struct fd_info
{
	enum fd_type type;
	int fd;
};

struct st_outmsg {
	char buf[BUFF_SIZE];

	size_t len;
	size_t pos;
	struct st_outmsg *next;
};

struct st_client {
	struct fd_info base;

	char in_buf[BUFF_SIZE];
	size_t in_len;

	struct st_outmsg *head;
	struct st_outmsg *tail;

	size_t queued_bytes;
	size_t queued_messages;
	int read_paused;
	struct timespec last_activity;
	struct st_client *prev;
	struct st_client *next;
};

struct st_timer_info {
	struct fd_info base;
	// unique member
	
};

#define METRICS_PORT 9091
#define HTTP_LIMIT 32
#define HTTP_REQUEST_LIMIT 4096
#define HTTP_TIMEOUT_SECONDS 10

static uint64_t connections_total, bytes_received_total, bytes_sent_total;
static uint64_t queued_bytes_total;

struct http_client {
	struct fd_info base;
	struct http_client *next;
	struct timespec accepted;
	char request[HTTP_REQUEST_LIMIT + 1];
	size_t request_len;
	char response[2048];
	size_t response_len, response_pos;
};
static struct http_client *http_clients;
static unsigned http_count;

static void http_close(struct http_client *client)
{
	struct http_client **p = &http_clients;
	while(*p != client) p = &(*p)->next;
	*p = client->next;
	close(client->base.fd);
	free(client);
	http_count--;
}

static int metrics_listener(void)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if(fd == -1) return -1;
	int opt = 1;
	struct sockaddr_in addr = { .sin_family = AF_INET,
		.sin_port = htons(METRICS_PORT), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1 ||
	   bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1 || listen(fd, LISTEN_NUM) == -1)
	{
		perror("metrics listener"); close(fd); return -1;
	}
	return fd;
}

static void http_accept(int epoll_fd, int fd)
{
	/* Bound work per event even when connections are being rejected. */
	for(int i = 0; i < HTTP_LIMIT; i++)
	{
		int cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if(cfd == -1) { if(errno == EINTR) { i--; continue; } break; }
		if(http_count >= HTTP_LIMIT) { close(cfd); continue; }
		struct http_client *c = calloc(1, sizeof(*c));
		if(!c) { close(cfd); continue; }
		c->base.type = FD_HTTP_CLIENT; c->base.fd = cfd;
		struct epoll_event ev = { .events = EPOLLIN | EPOLLRDHUP, .data.ptr = c };
		if(clock_gettime(CLOCK_MONOTONIC, &c->accepted) == -1 ||
		   epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cfd, &ev) == -1)
		{ close(cfd); free(c); continue; }
		c->next = http_clients; http_clients = c; http_count++;
	}
}

static void http_response(struct http_client *c, int status)
{
	char body[1024];
	int len;
	if(status == 200)
		len = snprintf(body, sizeof(body),
			"# HELP epoll_active_connections Current active echo connections.\n# TYPE epoll_active_connections gauge\nepoll_active_connections %d\n"
			"# HELP epoll_connections_total Total accepted echo connections.\n# TYPE epoll_connections_total counter\nepoll_connections_total %llu\n"
			"# HELP epoll_bytes_received_total Total echo bytes received.\n# TYPE epoll_bytes_received_total counter\nepoll_bytes_received_total %llu\n"
			"# HELP epoll_bytes_sent_total Total echo bytes sent.\n# TYPE epoll_bytes_sent_total counter\nepoll_bytes_sent_total %llu\n"
			"# HELP epoll_queued_bytes Echo bytes waiting to be sent.\n# TYPE epoll_queued_bytes gauge\nepoll_queued_bytes %llu\n",
			active_clients, (unsigned long long)connections_total,
			(unsigned long long)bytes_received_total, (unsigned long long)bytes_sent_total,
			(unsigned long long)queued_bytes_total);
	else len = snprintf(body, sizeof(body), "%s\n", status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : "Bad Request");
	if(len < 0 || (size_t)len >= sizeof(body)) { c->response_len = 0; return; }
	int n = snprintf(c->response, sizeof(c->response),
		"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
		status, status == 200 ? "OK" : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : "Bad Request",
		status == 200 ? "text/plain; version=0.0.4; charset=utf-8" : "text/plain; charset=utf-8", len, body);
	c->response_len = n > 0 && (size_t)n < sizeof(c->response) ? (size_t)n : 0;
}

static int http_handle(int epoll_fd, struct http_client *c, uint32_t events)
{
	if(!c->response_len && (events & EPOLLIN))
	{
		for(;;)
		{
			ssize_t n = read(c->base.fd, c->request + c->request_len, HTTP_REQUEST_LIMIT - c->request_len);
			if(n > 0)
			{
				c->request_len += (size_t)n; c->request[c->request_len] = '\0';
				if(memchr(c->request, '\0', c->request_len)) { http_response(c, 400); break; }
				char *end = strstr(c->request, "\r\n\r\n");
				if(end)
				{
					char method[16], path[1024], version[16], extra;
					char *line = strstr(c->request, "\r\n");
					*line = '\0';
					int fields = sscanf(c->request, "%15s %1023s %15s %c", method, path, version, &extra);
					int valid = fields == 3 && (!strcmp(version, "HTTP/1.1") || !strcmp(version, "HTTP/1.0"));
					for(char *p = line + 2; valid && p < end; )
					{
						char *next = strstr(p, "\r\n");
						if(!next || next > end || !memchr(p, ':', (size_t)(next - p)) || *p == ':' || *p == ' ' || *p == '\t') { valid = 0; break; }
						for(char *q = p; q < next; q++) if((unsigned char)*q < 32 && *q != '\t') valid = 0;
						p = next + 2;
					}
					http_response(c, !valid ? 400 : strcmp(method, "GET") ? 405 : strcmp(path, "/metrics") ? 404 : 200); break;
				}
				if(c->request_len == HTTP_REQUEST_LIMIT) { http_response(c, 400); break; }
				continue;
			}
			if(n == 0) return -1;
			if(errno == EINTR) continue;
			if(errno == EAGAIN || errno == EWOULDBLOCK) break;
			return -1;
		}
		if(c->response_len)
		{
			struct epoll_event ev = { .events = EPOLLOUT | EPOLLRDHUP, .data.ptr = c };
			if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->base.fd, &ev) == -1) return -1;
		}
	}
	if(c->response_len && (events & EPOLLOUT))
	{
		while(c->response_pos < c->response_len)
		{
			ssize_t n = write(c->base.fd, c->response + c->response_pos, c->response_len - c->response_pos);
			if(n > 0) { c->response_pos += (size_t)n; continue; }
			if(n < 0 && errno == EINTR) continue;
			if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
			return -1;
		}
		return -1;
	}
	if(events & (EPOLLERR | EPOLLHUP)) return -1;
	if((events & EPOLLRDHUP) && !c->response_len) return -1;
	return 0;
}

static void http_expire(void)
{
	struct timespec now;
	if(clock_gettime(CLOCK_MONOTONIC, &now) == -1) return;
	for(struct http_client *c = http_clients, *next; c; c = next)
	{
		next = c->next;
		if(now.tv_sec - c->accepted.tv_sec > HTTP_TIMEOUT_SECONDS ||
		   (now.tv_sec - c->accepted.tv_sec == HTTP_TIMEOUT_SECONDS && now.tv_nsec >= c->accepted.tv_nsec)) http_close(c);
	}
}

#if LOG_LEVEL >= LOG_DEBUG

static void dump_client(const struct st_client *client)
{
	if(client == NULL)
		return;

	fprintf(stderr, "\n================ CLIENT DUMP ==================\n");
	fprintf(stderr, "fd						:	%d\n", client->base.fd);
	fprintf(stderr, "in_len					:	%zu / %d\n",
		client->in_len, BUFF_SIZE);
	fprintf(stderr, "queued_bytes			:	%zu\n", client->queued_bytes);
	fprintf(stderr, "queued_messages		:	%zu\n", client->queued_messages);
	fprintf(stderr, "read_paused			:	%s\n",
		client->read_paused ? "YES" : "NO");

	fprintf(stderr, "\n[OUTPUT QUEUE]\n");

	const struct st_outmsg *msg = client->head;
	size_t index = 0;
	size_t remaining_total = 0;

	while (msg != NULL && index < QUEUE_MAX_MESSAGES + 1)
	{
		size_t remaining = 0;

		if(msg->pos <= msg->len && msg->len <= BUFF_SIZE)
		{
			remaining = msg->len - msg->pos;
			fprintf(stderr, "	data	: %.*s\n",
					(int)msg->len,
					msg->buf
				   );

			fprintf(stderr, "	unsent	: %.*s\n",
					(int)remaining,
					msg->buf + msg->pos
				   );
		}
		else
		{
			fprintf(stderr, " WARNING invalid pos/len\n");
		}

		fprintf(stderr,
				"\n [MESSAGE %zu] \n"
				"   address     : %p\n"
				"   len         : %zu\n"
				"   pos         : %zu\n"
				"   remaining   : %zu\n"
				"   next        : %p%s\n",
				index,
				(void *)msg,	
				msg->len,
				msg->pos,
				remaining,
				(void *)msg->next,
				msg == client->tail ? " <-- TAIL" : "");

	
		remaining_total += remaining;
		msg = msg->next;
		index++;
	}

	if(msg != NULL)
		fprintf(stderr, " WARNING: traversal limit reached\n");

	fprintf(stderr, "\n[CHECK]\n");
	fprintf(stderr, "counted_messages	: %zu\n", index);
	fprintf(stderr, "counted_bytes		: %zu\n", remaining_total);

	if(msg == NULL &&
		index == client->queued_messages &&
		remaining_total == client->queued_bytes)
	{
		fprintf(stderr, "queued counters	: OK\n");
	}
	else
	{
		fprintf(stderr, "queue counters		: MISMATCH\n");
	}

	fprintf(stderr, "=========================================\n\n");
}

#endif

static void free_output_queue(struct st_client *client)
{
	struct st_outmsg *msg = client->head;
	while(msg != NULL)
	{
		struct st_outmsg *next = msg->next;
		free(msg);
		msg = next;
	}

	client->head = NULL;
	client->tail = NULL;
	queued_bytes_total -= client->queued_bytes;
	client->queued_bytes = 0;
	client->queued_messages = 0;
}

static void close_client(struct st_client *client)
{
	if(client->prev != NULL)
		client->prev->next = client->next;
	else
		clients = client->next;
	if(client->next != NULL)
		client->next->prev = client->prev;
	close(client->base.fd);
	active_clients--;
	free_output_queue(client);
	free(client);
}

static int record_activity(struct st_client *client)
{
	if(clock_gettime(CLOCK_MONOTONIC, &client->last_activity) == -1)
	{
		perror("clock_gettime");
		return -1;
	}
	return 0;
}

static int expire_idle_clients(void)
{
	struct timespec now;
	if(clock_gettime(CLOCK_MONOTONIC, &now) == -1)
	{
		perror("clock_gettime");
		return -1;
	}
	for(struct st_client *client = clients, *next; client != NULL; client = next)
	{
		next = client->next;
		time_t seconds = now.tv_sec - client->last_activity.tv_sec;
		if(seconds > IDLE_TIMEOUT_SECONDS ||
		   (seconds == IDLE_TIMEOUT_SECONDS && now.tv_nsec >= client->last_activity.tv_nsec))
		{
			LOG(LOG_INFO, "idle timeout fd=%d", client->base.fd);
			close_client(client);
		}
	}
	return 0;
}

static int flush_output(struct st_client *client)
{
	ssize_t nw;

	while(client->head != NULL)
	{
		struct st_outmsg *msg = client->head;

		while(msg->pos < msg->len)
		{
			nw = write(client->base.fd, msg->buf + msg->pos, msg->len - msg->pos);
			if(nw > 0)
			{
				LOG(LOG_DEBUG,
					"%s: fd=%d nw=%zd pos=%zu/%zu queued_bytes=%zu",
					__func__,
					client->base.fd,
					nw,
					msg->pos + nw,
					msg->len,
					client->queued_bytes
				  );

				if(record_activity(client) == -1)
					return -1;
				bytes_sent_total += (uint64_t)nw;
				queued_bytes_total -= (uint64_t)nw;
				msg->pos += nw;
				client->queued_bytes -= nw;
				continue;
			}
			if (nw == -1 && errno == EINTR)
			{
				continue;
			}

			if (nw == -1 && errno == EPIPE)//閉じているソケットに書き込もうとした
			{
				return 1; // client切断扱い
			}

			if (nw == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
			{
				LOG(LOG_DEBUG,
						"%s: EAGAIN fd=%d pos=%zu len=%zu queued_bytes=%zu",
						__func__,
						client->base.fd,
						msg->pos,
						msg->len,
						client->queued_bytes
				   );
				return 0;
			}
			perror("write");
			return -1;
		}

		client->head = msg->next;
		client->queued_messages--;

		free(msg);
		
		if(client->head == NULL)
		{
			client->tail = NULL;
		}
	}
	return 0;
}

static int accept_client(int epoll_fd, int sfd)
{
	struct sockaddr_in peer_addr;
	struct epoll_event ev;
	struct st_client *client;
	int cfd;

	for(;;)
	{
		socklen_t peer_len = sizeof(peer_addr); 
		cfd = accept4(sfd, (struct sockaddr *)&peer_addr, &peer_len, SOCK_NONBLOCK | SOCK_CLOEXEC); 

		if(cfd >= 0)
		{
			int sndbuf = 4096;
			// テスト用
			if(setsockopt(cfd,
						SOL_SOCKET,
						SO_SNDBUF,
						&sndbuf,
						sizeof(sndbuf)) == -1 )
			{
				perror("setsocket SO_SNDBUF");
			}

			client = calloc(1, sizeof(*client));
			if(client == NULL)
			{
				perror("calloc");
				close(cfd);
				return -1;
			}
			client->base.type = FD_CLIENT;
			client->base.fd = cfd;
			if(record_activity(client) == -1)
			{
				close(cfd);
				free(client);
				return -1;
			}

			ev.events = EPOLLIN; 
			ev.data.ptr = client;
 
			if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cfd, &ev)==-1) 
			{ 
				perror("epoll_ctl"); 
				close(client->base.fd); 
				free(client);
				return -1; 
			}
			LOG(LOG_DEBUG, "%s: connected. client fd:%d", __func__, cfd);

			client->next = clients;
			if(clients != NULL)
				clients->prev = client;
			clients = client;
			connections_total++;
			active_clients++; // 現在の接続数を+1
			
			// さらに接続待ちが残っているかもしれないので続ける
			continue;
		}

		if (errno == EINTR) { continue; } 
		if (errno == EAGAIN || errno == EWOULDBLOCK) { break; } 

		perror("accept4"); 
		return -1; 
	}
	return 0;
}

int process_input(struct st_client *client)
{
	char *p;
	while((p = memchr(client->in_buf, '\n', client->in_len)) != NULL)
	{
		size_t line_len = (size_t)(p - client->in_buf) + 1;

		if(client->queued_messages >= QUEUE_MAX_MESSAGES ||
			client->queued_bytes > QUEUE_MAX_BYTES ||
			line_len > QUEUE_MAX_BYTES - client->queued_bytes)
		{
			LOG(LOG_ERROR,
				"%s: output queue limit exceeded fd=%d",
				__func__,
				client->base.fd);
			return -1;
		}

		struct st_outmsg *msg = calloc(1, sizeof(*msg));
		if(msg == NULL)
		{
			return -1;
	}

		memcpy(msg->buf, client->in_buf, line_len);

		msg->len = line_len;
		msg->pos = 0;

		size_t remain = client->in_len - line_len;
		memmove(client->in_buf, p + 1, remain);
		client->in_len = remain;

		if (client->head == NULL || client->tail == NULL)
		{
			client->head = msg;
			client->tail = msg;
		}
		else
		{
			client->tail->next = msg;
			client->tail = msg;
		}

		client->queued_messages++;
		client->queued_bytes += msg->len;
		queued_bytes_total += msg->len;
	}
	return 0;
}

static int process_and_flush(struct st_client *client)
{
	int ret = process_input(client);
	if(ret < 0)
	{
		return ret;
	}

	return flush_output(client);
}

static int handle_client(int epoll_fd, struct st_client *client, uint32_t events)
{
	ssize_t nr;
	uint32_t wanted;
	
	if(events & EPOLLOUT)
	{
		LOG(LOG_DEBUG,"%s: EPOLLOUT fd=%d", __func__, client->base.fd);
		int ret = process_and_flush(client);
		if(ret != 0)
		{
			return ret;
		}
	}

	if((events & EPOLLIN) && !client->read_paused)
	{
		LOG(LOG_DEBUG, "%s: EPOLLIN fd=%d", __func__, client->base.fd);
		for(;;)
		{
			if(client->queued_bytes >= QUEUE_HIGH_WATER) 
			{
				break;
			}

			if(client->in_len == BUFF_SIZE)
			{
				LOG(LOG_ERROR, "%s: input buffer full fd=%d", __func__, client->base.fd);
				return -1;
			}

			nr = read(
				client->base.fd, 
				client->in_buf + client->in_len,
				BUFF_SIZE - client->in_len
			);

			if(nr > 0)
			{
				if(record_activity(client) == -1)
					return -1;
				bytes_received_total += (uint64_t)nr;
				client->in_len += nr;
				int ret = process_and_flush(client);
				if(ret != 0)
				{
					return ret;
				}

				continue;
			}
			if(nr == 0)
			{
				LOG(LOG_DEBUG, "%s: disconnetcted. client fd:%d", __func__, client->base.fd);
				return 1;
			}
			if(nr == -1 && errno == EINTR)
			{
				continue;
			}	
			if(nr == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
			{
				break;
			}
			if( nr == -1 && errno == ECONNRESET )
			{
				LOG(LOG_ERROR, "%s: receive ECONNRESET. disconnected. client fd:%d", __func__, client->base.fd); 
				return 1;
			} 
			perror("read");
			return -1;
		}
	}

	wanted = EPOLLRDHUP;

	int old_paused = client->read_paused;

	if (!client->read_paused && client->queued_bytes >= QUEUE_HIGH_WATER)
	{
		client->read_paused = 1;
	}

	if(client->read_paused && client->queued_bytes <= QUEUE_LOW_WATER)
	{
		client->read_paused = 0;
	}

	if(old_paused != client->read_paused)
	{
		#if LOG_LEVEL >= LOG_DEBUG
			dump_client(client);
		#endif
	}

	if(!client->read_paused)
	{
		wanted |= EPOLLIN;
	}

	if(client->head != NULL)
	{
		wanted |= EPOLLOUT;
	}

	struct epoll_event ev = {
		.events = wanted,
		.data.ptr = client
	};

	if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->base.fd, &ev)==-1) 
	{ 
		perror("epoll_ctl"); 
		return -1; 
	}

	return 0;
}

static int health_check(int tfd)
{
	static uint64_t ticks;
	uint64_t expirations;
	ssize_t nr;

	for(;;)
	{
		nr = read(tfd, &expirations, sizeof(expirations));
		if( nr == sizeof(expirations) )
		{	
			ticks += expirations;
			if(ticks >= 10)
			{
				LOG(LOG_INFO, "%s: active clients: %d", __func__, active_clients);
				LOG(LOG_INFO, "%s: timer fired: %llu time(s)", __func__, (unsigned long long)ticks);
				ticks %= 10;
			}
			break;
		}
		if ( nr < 0 )
		{
			if(errno == EAGAIN || errno == EWOULDBLOCK) { break; }
			if(errno == EINTR) { continue; }
			
			perror("read timerfd");
			return -1;
		}
	}

	return 0;
}

static int handle_signal(int fd)
{
	struct signalfd_siginfo fdsi;
	ssize_t nr;

	for(;;)
	{
		nr = read(fd, &fdsi, sizeof(fdsi));
		if (nr == sizeof(fdsi))
		{
			if(fdsi.ssi_signo == SIGINT)
			{
				printf("Got SIGINT\n");
				return 1;
			}
			if(fdsi.ssi_signo == SIGTERM) 
			{
				LOG(LOG_INFO, "Got SIGTERM");
				return 1;
			}
			if(fdsi.ssi_signo == SIGQUIT) 
			{
				LOG(LOG_INFO, "Got SIGQUIT");
				return 1;
			}
		}	
		if(nr < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				return 0;
			}
			if (errno == EINTR) 
			{
				continue;
			}

			perror("read signal");
			return -1;
		}
	}
}

int main (void)
{
	signal(SIGPIPE, SIG_IGN); // SIGPIPEを無視する

	/*
 	 * tcp variable definition
 	 */
	int sfd, nfds;
	int n;
	int opt = 1;
	struct sockaddr_in my_addr;	

	/*
 	 * signalfd variable definition
 	 */
	int signal_fd;
	sigset_t mask;

	sfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if(sfd == -1)
	{
		perror("socket");
		return -1;
	}
	if(setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
	{
		perror("setsockopt");
		return -1;
	}

	LOG(LOG_INFO, "epoll-server version 1.1");

	memset(&my_addr, 0, sizeof(my_addr));
	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(PORT);
	my_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if(bind(sfd, (struct sockaddr *)&my_addr, sizeof(my_addr) )==-1)
	{
		perror("bind");
		return -1;
	}

	if(listen(sfd, LISTEN_NUM) == -1)
	{
		perror("listen");
		return -1;
	}

	int epoll_fd = epoll_create1(0);
	if(epoll_fd == -1 )
	{
		perror("epoll_create1");
		return -1;

	}

	struct fd_info listener = {
		.type = FD_LISTENER,
		.fd = sfd
	};

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.ptr = &listener;

	if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sfd, &ev) == -1)
	{
		perror("epoll_ctl");
		return -1;
	}

	int metrics_fd = metrics_listener();
	if(metrics_fd == -1) return -1;
	struct fd_info metrics_info = { .type = FD_METRICS_LISTENER, .fd = metrics_fd };
	struct epoll_event metrics_ev = { .events = EPOLLIN, .data.ptr = &metrics_info };
	if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, metrics_fd, &metrics_ev) == -1)
	{ perror("epoll_ctl metrics"); close(metrics_fd); return -1; }

	int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if(timer_fd == -1)
	{
		perror("timerfd_create");
		return -1;
	}

	struct itimerspec timer = {0};
	timer.it_value.tv_sec = 1;
	timer.it_interval.tv_sec = 1;
	if(timerfd_settime(timer_fd, 0, &timer, NULL) == -1)
	{
		perror("timerfd_settime");
		return -1;
	}

	struct fd_info timer_info = {
		.type = FD_TIMER,
		.fd = timer_fd
	};

	struct epoll_event timer_ev = {0};
	timer_ev.events = EPOLLIN;
	timer_ev.data.ptr = &timer_info;

	if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &timer_ev) == -1)
	{
		perror("epoll_ctl");
		return -1;
	}

	/*
 	 * signal setting
 	 */
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGQUIT);	
	
	if(sigprocmask(SIG_BLOCK,&mask, NULL) == -1)
	{
		perror("sigprocmask");
		return -1;
	}

	signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	if(signal_fd == -1)
	{
		perror("signalfd");
		return -1;
	}
	
	struct fd_info signal_info = {
		.type = FD_SIGNAL,
		.fd = signal_fd
	};

	struct epoll_event signal_ev = {0};
	signal_ev.events = EPOLLIN;
	signal_ev.data.ptr = &signal_info;

	if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, signal_fd, &signal_ev) == -1)
	{
		perror("epoll_ctl");
		return -1;
	}

	struct epoll_event events[EVENT_NUM];
	LOG(LOG_INFO, "listening on 8080");

	// epoll event loop
	for(;;)
	{
		nfds = epoll_wait(epoll_fd, events, EVENT_NUM, -1);
		if (nfds == -1)
		{
			if(errno == EINTR) { continue; }

			perror("epoll_wait");
			return -1;
		}

		int timer_fired = 0;
		for (n = 0; n < nfds; n++)
		{
			struct fd_info *info = events[n].data.ptr;

			if (info->type == FD_LISTENER)
			{
				// 新規接続
				if(accept_client(epoll_fd, info->fd) == -1)
				{
					return -1;
				}
			}
			else if(info->type == FD_METRICS_LISTENER)
				http_accept(epoll_fd, info->fd);
			else if(info->type == FD_HTTP_CLIENT)
			{
				struct http_client *client = events[n].data.ptr;
				if(http_handle(epoll_fd, client, events[n].events) == -1) http_close(client);
			}
			else if(info->type == FD_TIMER)
			{
				if(health_check(info->fd) == -1)
				{
					close(info->fd);
					return -1;
				}
				timer_fired = 1;
			}
			else if(info->type == FD_SIGNAL)
			{
				int ret = handle_signal(info->fd);
				
				if(ret == -1)
				{
					close(info->fd);
					return -1;
				}
				else if(ret == 1)
				{
					LOG(LOG_INFO, "Clean up and Exit.");
					while(clients != NULL)
						close_client(clients);
					while(http_clients != NULL) http_close(http_clients);
					close(metrics_fd);
					close(sfd);
					close(timer_fd);
					close(signal_fd);
					close(epoll_fd);
					return 0;
				}
				else if(ret == 0)
				{
					continue;
				}
				else
				{
					;
				}
			}
			else if(info->type == FD_CLIENT)
			{
				struct st_client *client = events[n].data.ptr;
				// clientからのメッセージ受信
				int ret = handle_client(epoll_fd, client, events[n].events);
				if(ret == -1)
				{
					LOG(LOG_ERROR, "%s: get -1 from handle_client,\nsocket close fd=%d", __func__, client->base.fd);
					close_client(client);
					continue;
				}
				else if (ret == 1)
				{
					close_client(client);
					continue;
				}
				else
				{
					;
				}
			}
			else
			{
				;
			}
		}
		/* Expire only after all pointers returned by epoll_wait have been used. */
		if(timer_fired) http_expire();
		if(timer_fired && expire_idle_clients() == -1)
			return -1;
	}

	return 0;
}
