all: my_daemon

my_daemon: main.c
	$(CC) $^ -o $@ -Wall -Wextra -Wpedantic -std=c11 -lconfuse

clean:
	rm -f my_daemon

.PHONY: all clean