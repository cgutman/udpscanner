#include "udpscanner.h"

#define DEFAULT_KNOWN_CLOSED_PORT 1
#define DEFAULT_RECV_DELAY 500
#define DEFAULT_SEND_LENGTH 1
#define DEFAULT_DELAY_PER_PROBE 200

static int verbose_enabled = 0;
static int output_closed_ports = 0;

static
void wait_ms(int ms) {
#ifdef WIN32
	Sleep(ms);
#else
	{
		useconds_t sleep_time = ms;
		sleep_time *= 1000;
		usleep(sleep_time);
	}
#endif
}

static
int send_probe(struct addrinfo *addrinfo, int port, int delay_ms, const char *send_data, int send_len) {
	SOCKET s;
	int err;
	char recv_buf[1];

	s = socket(addrinfo->ai_family,
		addrinfo->ai_socktype, addrinfo->ai_protocol);
	if (s == INVALID_SOCKET) {
		fprintf(stderr, "socket() failed: %d\n", LastSocketError());
		return SCAN_RESULT_ERROR;
	}

#ifdef WIN32
		err = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&delay_ms, sizeof(delay_ms));
#else
	{
		struct timeval tv;
		tv.tv_sec = delay_ms / 1000;
		tv.tv_usec = (delay_ms % 1000) * 1000;
		err = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
	}
#endif

	if (err == SOCKET_ERROR) {
		fprintf(stderr, "setsockopt() failed: %d\n", LastSocketError());
		closesocket(s);
		return SCAN_RESULT_ERROR;
	}

	((struct sockaddr_in *)addrinfo->ai_addr)->sin_port = htons(port);

	err = connect(s, addrinfo->ai_addr, addrinfo->ai_addrlen);
	if (err == SOCKET_ERROR) {
		fprintf(stderr, "connect() failed: %d\n", LastSocketError());
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
int scan_host(struct addrinfo *addrinfo, int known_closed_port,
	int start_port, int end_port, int resp_delay_ms, int probe_delay_ms, int send_len) {

	void *send_buf;
	int res;
	int tries;

	send_buf = malloc(send_len);
	if (send_buf == NULL) {
		return -1;
	}

	// Probe the known closed port first to make sure we're getting
	// ICMP port unreachable messages on it
	res = send_probe(addrinfo, known_closed_port, resp_delay_ms, (const char *) send_buf, send_len);
	if (res != SCAN_RESULT_PORT_CLOSED) {
		fprintf(stderr, "No ICMP port unreachable message received for the known closed port. The scan cannot proceed.\n");
		return -1;
	}

	tries = 0;
	while (start_port <= end_port) {
		wait_ms(tries * probe_delay_ms);
		res = send_probe(addrinfo, start_port, resp_delay_ms, (const char *) send_buf, send_len);
		if (res == SCAN_RESULT_PORT_INCONCLUSIVE) {
			// We need to probe the known closed port
			wait_ms(tries * probe_delay_ms);
			res = send_probe(addrinfo, known_closed_port, resp_delay_ms, (const char *) send_buf, send_len);
			if (res == SCAN_RESULT_PORT_OPEN) {
				fprintf(stderr, "The known closed port is now open. The scan is now aborting.\n");
				return -1;
			}
			else if (res == SCAN_RESULT_PORT_CLOSED) {
				// The known closed port got a port unreachable message so we are getting
				// ICMP messages. Let's try again and make sure it's closed
				wait_ms(tries * probe_delay_ms);
				res = send_probe(addrinfo, start_port, resp_delay_ms, (const char *) send_buf, send_len);

				// If it's still inconclusive, that means we got no port unreachable message
				// for this port after just receiving one for the known closed port
				if (res == SCAN_RESULT_PORT_INCONCLUSIVE) {
					// Probe the known closed port one last time before concluding the port was open
					wait_ms(tries * probe_delay_ms);
					res = send_probe(addrinfo, known_closed_port, resp_delay_ms, (const char *) send_buf, send_len);
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
				// ICMP rate limiting. We'll wait a bit more next time.
				tries++;

				if (verbose_enabled) {
					fprintf(stderr, "Retried %d time(s) scanning port %d. Waiting %d milliseconds between probes...\n",
						tries, start_port, tries * probe_delay_ms);
				}

				goto try_again;
			}
		}

		if (res == SCAN_RESULT_PORT_CLOSED) {
			if (output_closed_ports || verbose_enabled) {
				printf("Port %d - Closed\n", start_port);
			}
		}
		else if (res == SCAN_RESULT_PORT_OPEN) {
			printf("Port %d - Open\n", start_port);
		}

		if (res == SCAN_RESULT_ERROR) {
			return -1;
		}

		start_port++;
		tries = 0;

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
	printf("\t-l <send length>\n");
	printf("\t\tThe length of random data that will be sent in each packet.\n");
	printf("\t\tThe default value is %d byte(s).\n", DEFAULT_SEND_LENGTH);
	printf("\t-k <known closed port>\n");
	printf("\t\tA port on the host that is known to be closed.\n");
	printf("\t\tThe default known closed port is %d.\n", DEFAULT_KNOWN_CLOSED_PORT);
	printf("\t-p <probe delay (ms)>\n");
	printf("\t\tThe amount of time added between probes to avoid triggering\n");
	printf("\t\tICMP rate limiting on consecutive probes.\n");
	printf("\t\tThe default retry delay is %d milliseconds.\n", DEFAULT_DELAY_PER_PROBE);
	printf("\t-c\n");
	printf("\t\tOutput closed ports in addition to open ones.\n");
	printf("\t-v\n");
	printf("\t\tEnable verbose output (implies -c).\n");
}

int main(int argc, char* argv[]) {
	int start_port;
	int end_port;
	int resp_delay_ms;
	int send_len;
	int res;
	int known_closed_port;
	int probe_delay_ms;
	struct addrinfo *addrinfo;
	struct addrinfo hint;
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

	memset(&hint, 0, sizeof(hint));
	hint.ai_family = AF_UNSPEC;
	hint.ai_socktype = SOCK_DGRAM;
	hint.ai_protocol = IPPROTO_UDP;
	hint.ai_addrlen = 0;
	hint.ai_canonname = NULL;
	hint.ai_addr = NULL;
	hint.ai_next = NULL;

	res = getaddrinfo(argv[1], NULL, &hint, &addrinfo);
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
	send_len = DEFAULT_SEND_LENGTH;
	known_closed_port = DEFAULT_KNOWN_CLOSED_PORT;
	probe_delay_ms = DEFAULT_DELAY_PER_PROBE;

	i = 4;
	while (i < argc) {
		switch (argv[i][1]) {
		case 'v':
			verbose_enabled = 1;
			i++;
			break;

		case 'c':
			output_closed_ports = 1;
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

			case 'l':
				send_len = atoi(argv[i + 1]);
				i += 2;
				break;

			case 'k':
				known_closed_port = atoi(argv[i + 1]);
				i += 2;
				break;

			case 'p':
				probe_delay_ms = atoi(argv[i + 1]);
				i += 2;
				break;

			default:
				fprintf(stderr, "Invalid option: %s\n", argv[i]);
				usage();
				return -1;
			}
		}
	}

	return scan_host(addrinfo, known_closed_port,
		start_port, end_port, resp_delay_ms, probe_delay_ms, send_len);
}

