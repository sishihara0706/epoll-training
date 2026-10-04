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

#define PORT 8080
#define EVENT_NUM 1000
#define BUFF_SIZE 4096
#define LISTEN_NUM 128

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

struct st_client {
	struct fd_info base;

	char in_buf[BUFF_SIZE+1];
	size_t in_len;

	char out_buf[BUFF_SIZE+2];
	size_t out_len; // 全体で何バイト送る予定か
	size_t out_pos; // 何バイト目まで送信済みか
};

struct st_timer_info {
	struct fd_info base;
	// unique member
	
};

static int flush_output(struct st_client *client)
{
	ssize_t nw;

	while(client->out_pos < client->out_len)
	{
		nw = write(client->base.fd, client->out_buf + client->out_pos, client->out_len - client->out_pos);
		if(nw > 0)
		{
			printf(
					"write fd=%d nw=%zd pos=%zu/%zu func=%s\n",
					client->base.fd,
					nw,
					client->out_pos + nw,
					client->out_len,
					__func__
				  );

			client->out_pos += nw;
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
					"write EAGAIN fd=%d pos=%zu len=%zu\n",
					client->base.fd,
					client->out_pos,
					client->out_len
				  );
			break;
		}
		perror("write");
		return -1;
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
	if(client->out_len != 0)
	{
		return 0;
	}

	char *p = memchr(client->in_buf, '\n', client->in_len);
	if( p == NULL )
	{
		return 0;
	}

	size_t line_len = (size_t)(p - client->in_buf) + 1;
	memcpy(client->out_buf, client->in_buf, line_len);

	client->out_len = line_len;
	client->out_pos = 0;

	size_t remain = client->in_len - line_len;
	memmove(client->in_buf, p + 1, remain);
	client->in_len = remain;
	
	return 1;	
}

static int process_and_flush(struct st_client *client)
{
	for(;;)
	{
		if(client->out_len == 0)
		{
			if(!process_input(client))
			{
				break;
			}
		}

		int ret = flush_output(client);
		if(ret != 0)
		{
			return ret;
		}

		if(client->out_pos < client->out_len)
		{
			// まだout_bufの中をすべて送りきれていない
			break;
		}

		client->out_pos = 0;
		client->out_len = 0;
	}
	return 0;	
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
		if(client->out_pos < client->out_len)
		{
			goto update_events;
		}
	}

	if(events & EPOLLIN)
	{
		printf("EPOLLIN fd=%d\n", client->base.fd);
		for(;;)
		{
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
					break;
				}

				if(client->out_pos < client->out_len)
				{
					break;
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

update_events:

	if(client->out_pos < client->out_len)
	{
		wanted = EPOLLOUT;
	}
	else
	{
		wanted = EPOLLIN;
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
					free(client);
					continue;
				}
				else if (ret == 1)
				{
					close(client->base.fd);
					active_clients--;
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
