/*
 * Sean Middleditch
 * sean@sourcemud.org
 *
 * The author or authors of this code dedicate any and all copyright interest
 * in this code to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and successors. We
 * intend this dedication to be an overt act of relinquishment in perpetuity of
 * all present and future rights to this code under copyright law. 
 */


#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <getopt.h>

#	define SOCKET int


#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>

#include "libtelnet.h"
#include "serial.h"

#define MAX_USERS 12
#define MAX_SERIAL_NUM 8
#define LINEBUFFER_SIZE 256

#define NEED_FD_MAX MAX_USERS*MAX_SERIAL_NUM + 2*MAX_SERIAL_NUM

#define DEV_NAME_LENGTH 32

static const telnet_telopt_t telopts[] = {
	{ TELNET_TELOPT_COMPRESS2,	TELNET_WILL, TELNET_DONT },
	{ -1, 0, 0 }
};

typedef struct user {
	char *name;
	SOCKET sock;
	telnet_t *telnet;
	char linebuf[256];
	int linepos;
	struct sockaddr_in sock_in;
} user_t;

typedef struct serial {
	int valid;
	int baudrate; /*波特率 */
	char dev_name[DEV_NAME_LENGTH]; /*串口设备名/dev/ttyUSB* */
	int dev_fd; /*打开的设备文件 */
	int listen_fd; /*telnet监听 */
	int listen_port; /*telnet监听端口 */
	int users_num;
	user_t users[MAX_USERS]; /*连接的用户 */
}serial_t;

typedef struct com_telnet {
	serial_t serial[MAX_SERIAL_NUM];	
}com_telnet_t;

static com_telnet_t com_mng;
static com_telnet_t *p_com_mng = &com_mng;

struct option longopts[] = {
	{"daemon", no_argument, NULL, 'd'},
	{"telnet_port", required_argument, NULL, 'p'},
	{"serial_dev", required_argument, NULL, 's'},
	{"baudrate",  required_argument, NULL, 'b'},
	{"motion", no_argument, NULL, 'm'},
	{ 0 }
};

void com_usage(char *program)
{
	printf("Usage :%s [OPTION...]\n", program);
	printf("-d, --daemon    Runs in daemon mode\n");
	printf("-p, --telnet_port set telnet server port\n");
	printf("-s, --serial_dev  set serial dev\n");
	printf("-b, --baudrate    set baud rate\n");
	printf("-m, --motion      self-motion\n");

}

serial_t * find_serial_by_listen_fd(int listen_fd)
{
	int i = 0;
	for (i =0; i<MAX_SERIAL_NUM; i++) {
		if (p_com_mng->serial[i].valid  && (p_com_mng->serial[i].listen_fd == listen_fd)) {
			return &p_com_mng->serial[i];
		}
	}
	return NULL;
}

serial_t * find_serial_by_dev_fd(int dev_fd) 
{
	int i = 0;
	for (i =0; i<MAX_SERIAL_NUM; i++) {
		if (p_com_mng->serial[i].valid  && (p_com_mng->serial[i].dev_fd == dev_fd)) {
			return &p_com_mng->serial[i];
		}
	}
	return NULL;
}

serial_t * find_serial_by_user_fd(int user_fd)
{
	int serial_index, client_index;
	serial_t *p_serial = NULL;
	for (serial_index = 0; serial_index < MAX_SERIAL_NUM; serial_index++) {
		p_serial = &p_com_mng->serial[serial_index];
		if (!p_serial->valid) 
			continue;
		
		for (client_index=0; client_index < MAX_USERS; client_index++) {
			if (p_serial->users[client_index].sock == user_fd)
				return p_serial;
		}
	}
	return NULL;
}

user_t * find_user_by_user_fd(int user_fd)
{
	int serial_index, client_index;
	serial_t *p_serial = NULL;
	for (serial_index = 0; serial_index < MAX_SERIAL_NUM; serial_index++) {
		p_serial = &p_com_mng->serial[serial_index];
		if (!p_serial->valid) 
			continue;
		
		for (client_index=0; client_index < MAX_USERS; client_index++) {
			if (p_serial->users[client_index].sock == user_fd)
				return &p_serial->users[client_index];
		}
	}
	return NULL;
}

#if 0
static void _message(const char *from, const char *msg) {
	int i;
	for (i = 0; i != MAX_USERS; ++i) {
		if (p_com_mng->users[i].sock != -1) {
			telnet_printf(users[i].telnet, "%s: %s\n", from, msg);
		}
	}
}
#endif

static void _send(SOCKET sock, const char *buffer, size_t size) {
	int rs;

	/* ignore on invalid socket */
	if (sock == -1)
		return;

	/* send data */
	while (size > 0) {
		if ((rs = send(sock, buffer, (int)size, 0)) == -1) {
			if (errno != EINTR && errno != ECONNRESET) {
				fprintf(stderr, "send() failed: %s\n", strerror(errno));
				exit(1);
			} else {
				return;
			}
		} else if (rs == 0) {
			fprintf(stderr, "send() unexpectedly returned 0\n");
			exit(1);
		}

		/* update pointer and size to see if we've got more to send */
		buffer += rs;
		size -= rs;
	}
}


static void _input(struct user *user, const char *buffer, size_t size) 
{
	unsigned int i;
	serial_t *p_serial  = NULL;
	p_serial = find_serial_by_user_fd(user->sock);
	if (p_serial ==NULL)
		return;

	for (i = 0; i != size; ++i) {
		serialWriteChar(p_serial->dev_fd, buffer[i]);
	}
}

static void _event_handler(telnet_t *telnet, telnet_event_t *ev,
		void *user_data) {
	struct user *user = (struct user*)user_data;

	switch (ev->type) {
	/* data received */
	case TELNET_EV_DATA:
		_input(user, ev->data.buffer, ev->data.size);
		break;
	/* data must be sent */
	case TELNET_EV_SEND:
		_send(user->sock, ev->data.buffer, ev->data.size);
		break;
	/* enable compress2 if accepted by client */
	case TELNET_EV_DO:
		if (ev->neg.telopt == TELNET_TELOPT_COMPRESS2)
			telnet_begin_compress2(telnet);
		break;
	/* error */
	case TELNET_EV_ERROR:
		close(user->sock);
		user->sock = -1;
	#if 0
		if (user->name != 0) {
			_message(user->name, "** HAS HAD AN ERROR **");
			free(user->name);
			user->name = 0;
		}
	#endif
		telnet_free(user->telnet);
		break;
		
	default:
		printf("ignore ev->type:%d\n", ev->type);
		/* ignore */
		break;
	}
}

void set_fd_noblock(int fd)
{
	int val;
	val = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, val | O_NONBLOCK);
	return;
}

int init_listen_socket(int listen_port)
{
	int listen_sock;
	int rs;
	struct sockaddr_in addr;

	/* create listening socket */
	if ((listen_sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		fprintf(stderr, "socket() failed: %s\n", strerror(errno));
		return -1;
	}

	/* reuse address option */
	rs = 1;
	setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&rs, sizeof(rs));

	/* bind to listening addr/port */
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(listen_port);
	if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		fprintf(stderr, "bind() failed: %s\n", strerror(errno));
		close(listen_sock);
		return -1;
	}
	set_fd_noblock(listen_sock);
	/* listen for clients */
	if (listen(listen_sock, 5) == -1) {
		fprintf(stderr, "listen() failed: %s\n", strerror(errno));
		close(listen_sock);
		return -1;
	}

	return listen_sock;

}

void exithandler() {
   // serialClose(fd);
   return;
}

int main(int argc, char **argv) {
	char buffer[512];
	short listen_port;
	char serial_dev[32];
	SOCKET listen_sock;
	SOCKET client_sock;
	int rs;
	int i;
	struct sockaddr_in addr;
	socklen_t addrlen;
	int fd;
	struct pollfd pfd[NEED_FD_MAX];

	char *p;
	char *progname_l;
	int daemon_mode = 0;
	int telnet_port = 100;
	int baudrate = 115200; /*default 115200*/
	int self_motion = 0;
	char **self_serial = NULL;
	serial_t *p_serial = NULL;
	user_t *p_user = NULL;
	int client_index, serial_index;

	memset(p_com_mng, 0, sizeof(struct com_telnet));

	progname_l = ((p = strrchr (argv[0], '/')) ? ++p : argv[0]);

	if (argc == 1) {
		com_usage(progname_l);
		return 0;
	}

	while (1) {
		int opt = getopt_long(argc, argv, "dp:s:b:m", longopts, 0);
		if (opt == EOF)
			break;
		switch (opt) {
			case 0:
				break;
			case 'd':
				daemon_mode = 1;
				break;
			case 'p':
				telnet_port = atoi(optarg);
				break;
			case 's':
				strncpy(serial_dev, optarg, 32);
				break;
			case 'b':
				baudrate = atoi(optarg);
				break;
			case 'm':
				self_motion = 1;
				break;
			default:
				com_usage(progname_l);
				break;
		}
	}

	/* initialize data structures */
	memset(&pfd, 0, sizeof(pfd));
	for (i = 0; i != NEED_FD_MAX; ++i)
		pfd[i].fd = -1;


	if (self_motion) {
		self_serial = getSerialPorts();
		for (i=0; self_serial[i] != NULL; i++) {
			int serial_port;
			fd = serialOpen(self_serial[i], baudrate);
			if (fd < 0) {
				fprintf(stderr, "serialOpen failed, fd=%d\n", fd);
				return -1;
			}
			p = strrchr (self_serial[i], 'B'); ++p;
			serial_port = (atoi(p) +1) * 100;
			p_com_mng->serial[i].valid = 1;
			p_com_mng->serial[i].baudrate = baudrate;
			p_com_mng->serial[i].listen_port = serial_port;
			p_com_mng->serial[i].dev_fd =fd;
			strncpy(p_com_mng->serial[i].dev_name, self_serial[i], DEV_NAME_LENGTH);

			pfd[MAX_SERIAL_NUM+i].fd = fd;
			pfd[MAX_SERIAL_NUM+i].events = POLLIN;

			
			fd = init_listen_socket(serial_port);
			p_com_mng->serial[i].listen_fd = fd;
			pfd[i].fd = fd;
			pfd[i].events = POLLIN;
			printf("open dev [%s] listen on port [%d]\n", self_serial[i], serial_port);
			
		}
	} else {
		fd = serialOpen(serial_dev, baudrate);
		if (fd < 0) {
			fprintf(stderr, "serialOpen failed, fd=%d\n", fd);
			return -1;
		}
		p_com_mng->serial[0].valid = 1;
		p_com_mng->serial[0].baudrate = baudrate;
		p_com_mng->serial[0].dev_fd =fd;
		strncpy(p_com_mng->serial[0].dev_name, serial_dev, DEV_NAME_LENGTH);
		pfd[MAX_SERIAL_NUM+1].fd = fd;
		pfd[MAX_SERIAL_NUM+1].events = POLLIN;
		fd = init_listen_socket(telnet_port);
		
		p_com_mng->serial[0].listen_fd = fd;
		p_com_mng->serial[0].listen_port = telnet_port;
		pfd[0].fd = fd;
		pfd[0].events = POLLIN;
		printf("open dev [%s] listen on port [%d]\n", serial_dev, telnet_port);
	}
	
	atexit(exithandler);

	for (serial_index = 0; serial_index < MAX_SERIAL_NUM; serial_index++) {
		for (client_index = 0; client_index < MAX_USERS; client_index++) {
			p_com_mng->serial[serial_index].users[client_index].sock = -1;
		}
	}
	
	/* loop for ever */
	for (;;) {
		int listen_index = 0;
		
		for (serial_index = 0; serial_index < MAX_SERIAL_NUM; serial_index++) {

			for (client_index = 0; client_index < MAX_USERS; client_index++) {
				p_user = &p_com_mng->serial[serial_index].users[client_index];
				if (p_user->sock != -1) {
					pfd[2*MAX_SERIAL_NUM+serial_index*MAX_USERS+client_index].fd = p_user->sock;
					pfd[2*MAX_SERIAL_NUM+serial_index*MAX_USERS+client_index].events = POLLIN;
				} else {
					pfd[2*MAX_SERIAL_NUM+serial_index*MAX_USERS+client_index].fd = -1;
					pfd[2*MAX_SERIAL_NUM+serial_index*MAX_USERS+client_index].events = 0;
				}
			}
		}

		/* poll */
		rs = poll(pfd, NEED_FD_MAX, -1);
		if (rs == -1 && errno != EINTR) {
			fprintf(stderr, "poll() failed: %s\n", strerror(errno));
			close(listen_sock);
			return 1;
		}

		for (listen_index = 0; listen_index < MAX_SERIAL_NUM; listen_index++) {
			char ip[32];
			/* new connection */
			if (pfd[listen_index].revents & POLLIN) {
				/* acept the sock */
				addrlen = sizeof(addr);
				if ((client_sock = accept(pfd[listen_index].fd, (struct sockaddr *)&addr,
						&addrlen)) == -1) {
					fprintf(stderr, "accept() failed: %s\n", strerror(errno));
					
					if (errno == EAGAIN)
						continue;
					return 1;
				}

				set_fd_noblock(client_sock);

				
				

				/* find a free user */
				p_serial = find_serial_by_listen_fd(pfd[listen_index].fd); 
				if (p_serial == NULL) {
					printf("ERROR!");
					return -1;
				}

				for (i=0; i < MAX_USERS; i++) {
					if (p_serial->users[i].sock == -1) {
						p_serial->users[i].sock = client_sock;
						p_serial->users[i].telnet = telnet_init(telopts, _event_handler, TELNET_FLAG_NVT_EOL, &p_serial->users[i]);
						telnet_negotiate(p_serial->users[i].telnet, TELNET_WILL, TELNET_TELOPT_COMPRESS2);
						telnet_negotiate(p_serial->users[i].telnet, TELNET_WILL, TELNET_TELOPT_ECHO);
						telnet_negotiate(p_serial->users[i].telnet, TELNET_WONT, TELNET_TELOPT_LINEMODE);
						telnet_negotiate(p_serial->users[i].telnet, TELNET_WILL, TELNET_TELOPT_SGA);
						memcpy(&p_serial->users[i].sock_in, &addr, sizeof(addr));
						p_serial->users_num++;
						break;
					}					
				}

				printf("[%s][%d] Connect user [%s][%u]. current user number [%d]\n", p_serial->dev_name, p_serial->listen_port, inet_ntop(addr.sin_family, &(addr.sin_addr), ip, 16), ntohs(addr.sin_port), p_serial->users_num);
				
				if (i == MAX_USERS) {
					printf("  rejected (too many users)\n");
					_send(client_sock, "Too many users.\r\n", 14);
					close(client_sock);
				}
			}

		}

		for (i=MAX_SERIAL_NUM; i< 2*MAX_SERIAL_NUM; i++) {
			if (pfd[i].revents & POLLIN) {
				char c;
				serialReadChar(pfd[i].fd, &c);
				p_serial = find_serial_by_dev_fd(pfd[i].fd);
				if (p_serial == NULL) {
					printf("by dev_fd find failed!\n");
					return -1;
				}
				for (i = 0; i< MAX_USERS; i++) {
					if (p_serial->users[i].telnet != NULL) {
						telnet_printf(p_serial->users[i].telnet, "%c", c);
					}
				
				}
			}
		}			
		
		/* read from client */
		for (i = 2*MAX_SERIAL_NUM; i < NEED_FD_MAX; ++i) {
			if (pfd[i].revents & POLLIN) {
				p_serial = find_serial_by_user_fd(pfd[i].fd);
				p_user = find_user_by_user_fd(pfd[i].fd);
				if (p_user->sock == -1)
					continue;
				

				if ((rs = recv(pfd[i].fd, buffer, sizeof(buffer), 0)) > 0) {
					telnet_recv(p_user->telnet, buffer, rs);
				} else if (rs == 0) {
					char ip[32];
					close(p_user->sock);
					telnet_free(p_user->telnet);
					p_user->sock = -1;
					p_serial->users_num--;
					printf("[%s][%d] leave user [%s][%u]. current user number [%d]\n", p_serial->dev_name, p_serial->listen_port, inet_ntop(p_user->sock_in.sin_family, &(p_user->sock_in.sin_addr), ip, 16), ntohs(p_user->sock_in.sin_port), p_serial->users_num);
					break;
				} else if (errno != EINTR) {
					fprintf(stderr, "recv(client) failed: %s\n",
							strerror(errno));
					exit(1);
				}
			}
		}
	}

	/* not that we can reach this, but GCC will cry if it's not here */
	return 0;
}
