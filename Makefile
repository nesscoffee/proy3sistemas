CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c99 -g -Iservidor/include -Wno-format-truncation

CLIENT = cliente/aws-s3
SERVER = servidor/aws-s3_server

.PHONY: all clean

all: $(CLIENT) $(SERVER)

$(CLIENT): cliente/aws-s3.c
	@mkdir -p cliente
	$(CC) $(CFLAGS) -o $@ $<

$(SERVER): servidor/src/aws-s3_server.c servidor/src/bucket_handler.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(CLIENT) $(SERVER)
	rm -rf buckets/
