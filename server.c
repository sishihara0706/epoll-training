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

#define PORT				8080
#define EVENT_NUM			1000
#define BUFF_SIZE			4096
#define LISTEN_NUM			128

#define QUEUE_HIGH_WATER	(64 * 1024)
#define QUEUE_LOW_WATER 	(32 * 1024)
#define QUEUE_MAX_BYTES		(256 * 1024)
#define QUEUE_MAX_MESSAGES	1024

static int active_clients;

enum fd_type
{
	FD_LISTENER,
	FD_TIMER,
	FD_SIGNAL,
	FD_CLIENT
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
};

struct st_timer_info {
	struct fd_info base;
	// unique member
	
};

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
	client->queued_bytes = 0;
	client->queued_messages = 0;
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
				printf(
						"write fd=%d nw=%zd pos=%zu/%zu queued_bytes=%zu func=%s\n",
						client->base.fd,
						nw,
						msg->pos + nw,
						msg->len,
						client->queued_bytes,
						__func__
					  );

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
				printf(
						"write EAGAIN fd=%d pos=%zu len=%zu queued_bytes=%zu func=%s\n",
						client->base.fd,
						msg->pos,
						msg->len,
						client->queued_bytes,
						__func__
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

			ev.events = EPOLLIN; 
			ev.data.ptr = client;
 
			if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cfd, &ev)==-1) 
			{ 
				perror("epoll_ctl"); 
				close(client->base.fd); 
				free(client);
				return -1; 
			}
			printf("connected. client fd:%d\n", cfd);

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
			fprintf(stderr,
				"output queue limit exceeded fd=%d\n",
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
		printf("EPOLLOUT fd=%d\n", client->base.fd);
		int ret = process_and_flush(client);
		if(ret != 0)
		{
			return ret;
		}
	}

	if((events & EPOLLIN) && !client->read_paused)
	{
		printf("EPOLLIN fd=%d\n", client->base.fd);
		for(;;)
		{
			if(client->queued_bytes >= QUEUE_HIGH_WATER) 
			{
				break;
			}

			if(client->in_len == BUFF_SIZE)
			{
				fprintf(stderr, "input buffer full fd=%d\n", client->base.fd);
				return -1;
			}

			nr = read(
				client->base.fd, 
				client->in_buf + client->in_len,
				BUFF_SIZE - client->in_len
			);

			if(nr > 0)
			{
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
				printf("disconnetcted. client fd:%d\n", client->base.fd);
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
				printf("receive ECONNRESET. disconnected. client fd:%d\n", client->base.fd); 
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
		dump_client(client);
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
	uint64_t expirations;
	ssize_t nr;

	for(;;)
	{
		nr = read(tfd, &expirations, sizeof(expirations));
		if( nr == sizeof(expirations) )
		{	
			printf("active clients: %d\n", active_clients);
			printf("timer fired: %llu time(s)\n", (unsigned long long)expirations);
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
				printf("Got SIGTERM\n");
				return 1;
			}
			if(fdsi.ssi_signo == SIGQUIT) 
			{
				printf("Got SIGQUIT\n");
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

	int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if(timer_fd == -1)
	{
		perror("timerfd_create");
		return -1;
	}

	struct itimerspec timer = {0};
	timer.it_value.tv_sec = 10;
	timer.it_interval.tv_sec = 10;
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
	printf("listening on 8080\n");

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
			else if(info->type == FD_TIMER)
			{
				if(health_check(info->fd) == -1)
				{
					close(info->fd);
					return -1;
				}
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
					printf("Clean up and Exit.\n");
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
					fprintf(stderr, "get -1 from handle_client,\nsocket close fd=%d\n", client->base.fd);
					close(client->base.fd);
					active_clients--;
					free_output_queue(client);
					free(client);
					continue;
				}
				else if (ret == 1)
				{
					close(client->base.fd);
					active_clients--;
					free_output_queue(client);
					free(client);
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
	}

	return 0;
}
