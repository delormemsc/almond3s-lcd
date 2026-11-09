// SPDX-License-Identifier: GPL-2.0-only
/* almond-lcd-at: одна или несколько AT-команд на последовательный порт модема,
 * ответ до OK/ERROR в stdout. Порт переводится в raw без эха (на OpenWrt нет
 * stty), таймаут ответа 3 с на команду. Только чтение статуса: экран модема
 * Almond с встроенным Quectel EC21.
 *   almond-lcd-at /dev/ttyUSB3 'AT+CSQ' 'AT+QTEMP'
 * Между ответами на разные команды печатается строка "--". Код выхода 1, если
 * порт не открылся. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define TIMEOUT_MS 3000

static int ends_with_final(const char *buf, size_t n)
{
	const char *p;
	size_t k;

	for (k = n; k > 0 && (buf[k - 1] == '\r' || buf[k - 1] == '\n'); k--)
		;
	for (p = buf + k; p > buf && p[-1] != '\n' && p[-1] != '\r'; p--)
		;
	return !strncmp(p, "OK", 2) || !strncmp(p, "ERROR", 5) ||
	       !strncmp(p, "+CME ERROR", 10) || !strncmp(p, "+CMS ERROR", 10);
}

int main(int argc, char **argv)
{
	struct termios t;
	int fd, i;

	if (argc < 3) {
		fprintf(stderr, "usage: %s /dev/ttyUSBn 'AT...' [...]\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	if (!tcgetattr(fd, &t)) {
		cfmakeraw(&t);
		t.c_cflag |= CLOCAL | CREAD;
		t.c_cc[VMIN] = 0;
		t.c_cc[VTIME] = 0;
		cfsetispeed(&t, B115200);
		cfsetospeed(&t, B115200);
		tcsetattr(fd, TCSANOW, &t);
	}
	tcflush(fd, TCIOFLUSH);
	for (i = 2; i < argc; i++) {
		char cmd[256], buf[4096];
		size_t n = 0;
		struct pollfd pf = { fd, POLLIN, 0 };
		int left = TIMEOUT_MS;

		snprintf(cmd, sizeof(cmd), "%s\r", argv[i]);
		if (write(fd, cmd, strlen(cmd)) < 0)
			break;
		while (left > 0 && n < sizeof(buf) - 1) {
			int r = poll(&pf, 1, 100);
			ssize_t k;

			if (r < 0 && errno == EINTR)
				continue;
			if (r <= 0) {
				left -= 100;
				continue;
			}
			k = read(fd, buf + n, sizeof(buf) - 1 - n);
			if (k <= 0) {
				left -= 100;
				continue;
			}
			n += (size_t)k;
			buf[n] = 0;
			if (ends_with_final(buf, n))
				break;
		}
		buf[n] = 0;
		/* без эха команды и пустых строк */
		{
			char *s = buf, *e;

			while ((e = strpbrk(s, "\r\n"))) {
				*e = 0;
				if (*s && strcmp(s, argv[i]))
					printf("%s\n", s);
				s = e + 1;
			}
			if (*s && strcmp(s, argv[i]))
				printf("%s\n", s);
		}
		if (!n)
			printf("TIMEOUT\n");
		if (i + 1 < argc)
			printf("--\n");
	}
	close(fd);
	return 0;
}
