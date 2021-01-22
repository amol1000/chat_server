all: chat_server.c utils.c
	gcc -pthread -o server chat_server.c utils.c

clean:
	$(RM) server