#include "udpscanner.h"

#define DEFAULT_KNOWN_CLOSED_PORT 1
#define DEFAULT_RECV_DELAY 500
#define DEFAULT_RATE_LIMIT_DELAY 1000
#define DEFAULT_SEND_LENGTH 1

static int verbose_enabled = 0;

static
int send_probe(struct sockaddr_in *addr, int delay_ms, const char *send_data, int send_len) {
	SOCKET s;
	int err;
	char recv_buf[1];

	s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) {
		fprintf(stderr, "socket() failed: %d\n", LastSocketError());
		return SCAN_RESULT_ERROR;
	}

#ifdef WIN32
		err = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&delay_ms, sizeof(delay_ms));
#else
	{
		struct timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = delay_ms * 1000;
		err = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
	}
#endif
	if (err == SOCKET_ERROR) {
		fprintf(stderr, "setsockopt() failed: %d\n", LastSocketError());
		closesocket(s);
		return SCAN_RESULT_ERROR;
	}

	err = connect(s, (const struct sockaddr*)addr, sizeof(*addr));
	if (err == SOCKET_ERROR) {
		fprintf(stderr, "bind() failed: %d\n", LastSocketError());
		closesocket(s);
		return SCAN_RESULT_ERROR;
	}

	err = send(s, send_data, send_len, 0);
	if (err == SOCKET_ERROR) {
		if (ERR_IS_REJECTION(LastSocketError())) {
			// ICMP port unreachable received so this is a confirmed no-go
			closesocket(s);
			return SCAN_RESULT_PORT_CLOSED;
		}
		else {
			fprintf(stderr, "send() failed: %d\n", LastSocketError());
			closesocket(s);
			return SCAN_RESULT_ERROR;
		}
	}

	err = recv(s, recv_buf, sizeof(recv_buf), 0);
	if (err == SOCKET_ERROR) {
		if (ERR_IS_TIMEOUT(LastSocketError())) {
			// Expected if no data was received in the time period
			closesocket(s);
			return SCAN_RESULT_PORT_INCONCLUSIVE;
		}
		else if (ERR_IS_TRUNCATION(LastSocketError())) {
			// Data was received so something is definitely there
			closesocket(s);
			return SCAN_RESULT_PORT_OPEN;
		}
		else if (ERR_IS_REJECTION(LastSocketError())) {
			// ICMP port unreachable received
			closesocket(s);
			return SCAN_RESULT_PORT_CLOSED;
		}
		else {
			fprintf(stderr, "recv() failed: %d\n", LastSocketError());
			closesocket(s);
			return SCAN_RESULT_ERROR;
		}
	}
	else {
		// Data was received so the port is open
		closesocket(s);
		return SCAN_RESULT_PORT_OPEN;
	}
}

static
int scan_host(struct sockaddr_in *addr, int known_closed_port, int start_port, int end_port, int resp_delay_ms, int rate_limiting_delay_ms, int send_len) {
	void *send_buf;
	int res;

	send_buf = malloc(send_len);
	if (send_buf == NULL) {
		return -1;
	}

	// Probe the known closed port first to make sure we're getting
	// ICMP port unreachable messages on it
	addr->sin_port = htons(known_closed_port);
	res = send_probe(addr, resp_delay_ms, (const char *) send_buf, send_len);
	if (res != SCAN_RESULT_PORT_CLOSED) {
		fprintf(stderr, "No ICMP port unreachable message received for the known closed port. The scan cannot proceed.\n");
		return -1;
	}

	while (start_port <= end_port) {
		addr->sin_port = htons(start_port);
		res = send_probe(addr, resp_delay_ms, (const char *) send_buf, send_len);
		if (res == SCAN_RESULT_PORT_INCONCLUSIVE) {
			// We need to probe the known closed port
			addr->sin_port = htons(known_closed_port);
			res = send_probe(addr, resp_delay_ms, (const char *) send_buf, send_len);
			if (res == SCAN_RESULT_PORT_OPEN) {
				fprintf(stderr, "The known closed port is now open. The scan is now aborting.\n");
				return -1;
			}
			else if (res == SCAN_RESULT_PORT_CLOSED) {
				// The known closed port got a port unreachable message so we are getting
				// ICMP messages. Let's try again and make sure it's closed
				addr->sin_port = htons(start_port);
				res = send_probe(addr, resp_delay_ms, (const char *) send_buf, send_len);

				// If it's still inconclusive, that means we got no port unreachable message
				// for this port after just receiving one for the known closed port
				if (res == SCAN_RESULT_PORT_INCONCLUSIVE) {
					// Probe the known closed port one last time before concluding the port was open
					addr->sin_port = htons(known_closed_port);
					res = send_probe(addr, resp_delay_ms, (const char *) send_buf, send_len);
					if (res == SCAN_RESULT_PORT_OPEN) {
						fprintf(stderr, "The known closed port is now open. The scan is now aborting.\n");
						return -1;
					}
					else if (res == SCAN_RESULT_PORT_CLOSED) {
						// We can now conclude the tested port is open
						res = SCAN_RESULT_PORT_OPEN;
					}
				}
			}
			
			if (res == SCAN_RESULT_PORT_INCONCLUSIVE) {
				// The known closed port is now inconclusive so we're probably hitting
				// ICMP rate limiting. We'll wait a bit and try again.
				if (verbose_enabled) {
					fprintf(stderr, "ICMP rate limiting is in effect. Waiting %d milliseconds...\n",
						rate_limiting_delay_ms);
				}
#ifdef WIN32
				Sleep(rate_limiting_delay_ms);
#else
				{
					useconds_t sleep_time = rate_limiting_delay_ms;
					sleep_time *= 1000;
					usleep(sleep_time);
				}
#endif
				goto try_again;
			}
		}

		if (res == SCAN_RESULT_PORT_CLOSED) {
			printf("Port %d - Closed\n", start_port);
		}
		else if (res == SCAN_RESULT_PORT_OPEN) {
			printf("Port %d - Open\n", start_port);
		}

		if (res == SCAN_RESULT_ERROR) {
			return -1;
		}

		start_port++;

	try_again:
		;
	}

	return 0;
}

void usage(void) {
	printf("udpscanner <host> <start port> <end port>\n");
	printf("\t-r <Response delay (ms)>\n");
	printf("\t\tShould be set to roughly the RTT to the host.\n");
	printf("\t\tIf set too high, the FP rate will increase.\n");
	printf("\t\tThe default value is %d milliseconds.\n", DEFAULT_RECV_DELAY);
	printf("\t-b <ICMP rate limiting delay (ms)>\n");
	printf("\t\tThe amount of time that the scanner will wait\n");
	printf("\t\tfor the host to start sending ICMP packets again.\n");
	printf("\t\tThe default value is %d milliseconds.\n", DEFAULT_RATE_LIMIT_DELAY);
	printf("\t-l <send length>\n");
	printf("\t\tThe length of random data that will be sent in each packet.\n");
	printf("\t\tThe default value is %d byte(s).\n", DEFAULT_SEND_LENGTH);
	printf("\t-k <known closed port>\n");
	printf("\t\tA port on the host that is known to be closed.\n");
	printf("\t\tThe default known closed port is %d.\n", DEFAULT_KNOWN_CLOSED_PORT);
	printf("\t-v\n");
	printf("\t\tEnable verbose output\n");
}

int main(int argc, char* argv[]) {
	int start_port;
	int end_port;
	int resp_delay_ms;
	int rate_limit_delay_ms;
	int send_len;
	int res;
	int known_closed_port;
	struct addrinfo *addrinfo;
	int i;

	if (argc < 4) {
		usage();
		return -1;
	}

#ifdef WIN32
	{
		WSADATA Data;

		res = WSAStartup(MAKEWORD(2, 0), &Data);
		if (res == SOCKET_ERROR) {
			fprintf(stderr, "WSAStartup() failed: %d\n", LastSocketError());
			return -1;
		}
	}
#endif

	res = getaddrinfo(argv[1], NULL, NULL, &addrinfo);
	if (res != 0) {
		fprintf(stderr, "getaddrinfo() failed: %d\n", res);
		return -1;
	}

	start_port = atoi(argv[2]);
	end_port = atoi(argv[3]);
	if (!start_port || !end_port) {
		fprintf(stderr, "Invalid port range\n");
		usage();
		return -1;
	}

	resp_delay_ms = DEFAULT_RECV_DELAY;
	rate_limit_delay_ms = DEFAULT_RATE_LIMIT_DELAY;
	send_len = DEFAULT_SEND_LENGTH;
	known_closed_port = DEFAULT_KNOWN_CLOSED_PORT;

	i = 4;
	while (i < argc) {
		switch (argv[i][1]) {
		case 'v':
			verbose_enabled = 1;
			i++;
			break;

		default:
			if (i == argc - 1) {
				fprintf(stderr, "Missing parameter to option: %s\n", argv[i]);
				usage();
				return -1;
			}

			switch (argv[i][1]) {
			case 'r':
				resp_delay_ms = atoi(argv[i + 1]);
				i += 2;
				break;

			case 'b':
				rate_limit_delay_ms = atoi(argv[i + 1]);
				i += 2;
				break;

			case 'l':
				send_len = atoi(argv[i + 1]);
				i += 2;
				break;

			case 'k':
				known_closed_port = atoi(argv[i + 1]);
				i += 2;
				break;

			default:
				fprintf(stderr, "Invalid option: %s\n", argv[i]);
				usage();
				return -1;
			}
		}
	}

	return scan_host((struct sockaddr_in*)addrinfo->ai_addr, known_closed_port,
		start_port, end_port, resp_delay_ms, rate_limit_delay_ms, send_len);
}

