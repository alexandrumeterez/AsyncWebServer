CPPFLAGS = -DDEBUG -DLOG_LEVEL=LOG_DEBUG -I. -I./http-parser/
CFLAGS = -Wall -g

.PHONY: all clean

build: all
all: aws
aws: aws.o sock_util.o http_parser.o
	gcc aws.o sock_util.o http_parser.o -o aws -laio	
aws.o: aws.c sock_util.h debug.h util.h
	gcc -c aws.c -laio
sock_util.o: sock_util.c sock_util.h debug.h util.h

http_parser.o: http-parser/http_parser.c http-parser/http_parser.h
	make -C http-parser/ http_parser.o
	cp http-parser/http_parser.o .
clean:
	-rm -f *~
	-rm -f *.o
	-rm -f sock_util.o
	-rm -f http-parser/http_parser.o
	-rm -f epoll_echo_server http_reply_once
