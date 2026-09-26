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

#define PORT 8080
#define EVENT_NUM 1000
#define BUFF_SIZE 4096
#define LISTEN_NUM 128

static int active_clients;

enum fd_type
{
	FD_LISTENER,
	FD_TIMECONTROLLER,
	FD_CLIENT
};

struct fd_info
{
	enum fd_type type;
	int fd;
};

struct st_client {
	struct fd_info base;

	char outbuff[BUFF_SIZE+2];
	size_t out_len; // 全体で何バイト送る予定か
	size_t out_pos; // 何バイト目まで送信済みか
};

struct st_time_controller {
	struct fd_info base;
	// unique member
	
};

static int flush_output(struct st_client *client)
{
	ssize_t nw;

	while(client->out_pos < client->out_len)
	{
		nw = write(client->base.fd, client->outbuff + client->out_pos, client->out_len - client->out_pos);
		if(nw > 0)
		{
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

static int handle_client(int epoll_fd, struct st_client *client, uint32_t events)
{
	char buff[BUFF_SIZE+1];	
	ssize_t nr;
	
	if(events & EPOLLOUT)
	{
		printf("EPOLLOUT fd=%d\n", client->base.fd);
		int ret = flush_output(client);
		if(ret == -1)
		{
			return -1;
		}
		else if(ret == 1)
		{
			return 1; // 閉じているclientに書き込もうとした
		}
		else if(client->out_pos == client->out_len)
		{
			client->out_pos = 0;
			client->out_len = 0;
			struct epoll_event ev = {
				.events = EPOLLIN,
				.data.ptr = client
			};

			if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->base.fd, &ev)==-1) 
			{ 
				perror("epoll_ctl"); 
				return -1; 
			}
		}
		else
		{
			struct epoll_event ev = {
				.events = EPOLLIN | EPOLLOUT,
				.data.ptr = client
			};

			if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->base.fd, &ev)==-1) 
			{ 
				perror("epoll_ctl"); 
				return -1; 
			}
			return 0; // まだ未送信なのでEPOLLINを処理しない
		}
	}

	if(events & EPOLLIN)
	{
		for(;;)
		{
			nr = read(client->base.fd, buff, BUFF_SIZE);

			if(nr > 0)
			{
				client->out_pos = 0;

				//read は'\0'をつけないので自分でつける
				buff[nr] = '\0';
				buff[strcspn(buff, "\r\n")] = '\0';
				printf("receive from client fd%d, message:%s\n", client->base.fd, buff);

				client->out_len = snprintf(client->outbuff, sizeof(client->outbuff), "%s\n", buff);
				int ret = flush_output(client);
				if(ret == -1)
				{
					return -1;
				}
				else if(ret == 1)
				{
					return 1;
				}
				else if(client->out_pos == client->out_len)
				{
					client->out_pos = 0;
					client->out_len = 0;
					struct epoll_event ev = {
						.events = EPOLLIN,
						.data.ptr = client
					};

					if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->base.fd, &ev)==-1) 
					{ 
						perror("epoll_ctl"); 
						return -1; 
					}
					continue;
				}
				else
				{
					struct epoll_event ev = {
						.events = EPOLLIN | EPOLLOUT,
						.data.ptr = client
					};

					if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->base.fd, &ev)==-1) 
					{ 
						perror("epoll_ctl"); 
						return -1; 
					}
					return 0; // まだ未送信なのでEPOLLINを処理しない
				}
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

int main (void)
{
	signal(SIGPIPE, SIG_IGN); // SIGPIPEを無視する

	int sfd, nfds;
	int n;
	int opt;
	struct sockaddr_in my_addr;	
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

	// epollオブジェクト作成
	int epoll_fd = epoll_create1(0);
	if(epoll_fd == -1 )
	{
		perror("epoll_create1");
		return -1;

	}

	// listener用のfd_infoを作成
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

	// setup timerfd
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

	// timer用のfd_infoを作成
	struct fd_info time_controller = {
		.type = FD_TIMECONTROLLER,
		.fd = timer_fd
	};

	struct epoll_event tev = {0};
	tev.events = EPOLLIN;
	tev.data.ptr = &time_controller;

	if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &tev) == -1)
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
			// int fd = events[n].data.fd;
			struct fd_info *info = events[n].data.ptr;

			if (info->type == FD_LISTENER)
			{
				// 新規接続
				if(accept_client(epoll_fd, info->fd) == -1)
				{
					return -1;
				}
			}
			else if(info->type == FD_TIMECONTROLLER)
			{
				if(health_check(info->fd) == -1)
				{
					close(info->fd);
					return -1;
				}
			}
			else if(info->type == FD_CLIENT)
			{
				struct st_client *client = events[n].data.ptr;
				// clientからのメッセージ受信
				int ret = handle_client(epoll_fd, client, events[n].events);
				if(ret == -1)
				{
					close(client->base.fd);
					free(client);
					return -1;
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
	close(sfd);
	close(epoll_fd);

	return 0;
}
