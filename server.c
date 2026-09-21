#define _GNU_SOURCE

#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/socket.h>
#include<unistd.h>
#include<netinet/in.h>
#include<sys/epoll.h>
#include<errno.h>

#define PORT 8080
#define EVENT_NUM 1000
#define BUFF_SIZE 4096

static int accept_client(int epoll_fd, int sfd)
{
	struct sockaddr_in peer_addr;
	struct epoll_event ev;
	int cfd;

	for(;;)
	{
		socklen_t peer_len = sizeof(peer_addr); 
		cfd = accept4(sfd, (struct sockaddr *)&peer_addr, &peer_len, SOCK_NONBLOCK | SOCK_CLOEXEC); 

		if(cfd >= 0)
		{
			ev.events = EPOLLIN; 
			ev.data.fd = cfd; 
			if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cfd, &ev)==-1) 
			{ 
				perror("epoll_ctl"); 
				close(cfd); 
				return -1; 
			}
			printf("connected. client fd:%d\n", cfd);
			
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

static int handle_client(int fd)
{
	char buff[BUFF_SIZE+1];	
	ssize_t total = 0;
	ssize_t nr,nw;
	int write_len; //書き込みたい長さ
	char send_buff[BUFF_SIZE+2];

	for(;;)
	{
		nr = read(fd, buff, BUFF_SIZE);

		if(nr > 0)
		{
			total = 0;

			//read は'\0'をつけないので自分でつける
			buff[nr] = '\0';
			buff[strcspn(buff, "\r\n")] = '\0';
			printf("receive from client fd%d, message:%s\n", fd, buff);

			write_len = snprintf(send_buff, sizeof(send_buff), "%s\n", buff);

			while(total < write_len)
			{
				nw = write(fd, send_buff + total, write_len - total);
				if(nw > 0)
				{
					total += nw;
					continue;
				}
				if (nw == -1 && errno == EINTR)
				{
					continue;
				}

				if (nw == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
				{ 
					break;
				}
				perror("write");
				break;
			}
			continue;
		}
		if(nr == 0)
		{
			printf("disconnetcted. client fd:%d\n", fd);
			close(fd);
			break;
		}
		if(errno == EINTR)
		{
			continue;
		}	
		if(errno == EAGAIN || errno == EWOULDBLOCK)
		{
			break;
		}

		perror("read");
		return -1;
	}
	return 0;
}

int main (void)
{
	int sfd, nfds;
	int n;
	struct sockaddr_in my_addr;	
	sfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if(sfd == -1)
	{
		perror("socket");
		return -1;
	}

	memset(&my_addr, 0, sizeof(my_addr));
	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(PORT);
	my_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // ローカルホストを指定

	if(bind(sfd, (struct sockaddr *)&my_addr, sizeof(my_addr) )==-1)
	{
		perror("bind");
		return -1;
	}

	if(listen(sfd, 5) == -1)
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

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = sfd;

	if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sfd, &ev) == -1)
	{
		perror("epoll_ctl");
		return -1;
	}

	struct epoll_event events[EVENT_NUM];
	printf("listening on 8080\n");

	for(;;)
	{
		nfds = epoll_wait(epoll_fd, events, EVENT_NUM, -1);
		if (nfds == -1)
		{
			perror("epoll_wait");
			return -1;
		}

		for (n = 0; n < nfds; n++)
		{
			int fd = events[n].data.fd;
			if (fd == sfd)
			{
				// 新規接続
				if(accept_client(epoll_fd, sfd) == -1)
				{
					return -1;
				}
			}
			else
			{
				// clientからのメッセージ受信
				if(handle_client(fd) == -1)
				{
					close(fd);
					return -1;
				}
			}
		}
	}
	close(sfd);
	close(epoll_fd);

	return 0;
}
