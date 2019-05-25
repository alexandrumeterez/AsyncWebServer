# Asynchronous Web Server

An async web server I implemented in C for my OS class, that I later modified to use Linux's AIO lib.

### Prerequisites

- libaio
- http-parser(included)

## How it works

```
    make
```

It uses epoll() and nonblocking sockets. You add the list of socket file descriptors that belong to client connections to the server to epoll's interest list and then listen for events. Everytime you get an event, you run a handler.

You start the HTTP server by running the ./aws executable.
It now waits for connections on port 8888. To test it, you can use wget from another terminal.
It has 2 modes:
- static files
```
    wget -t 1 "http://localhost:8888/static/file.dat"
```
First it sends the header: 200 OK if it finds the file or 404 NOT FOUND, if it does not. Afterwards, it starts to send fragments of the file using sendfile(), everytime epoll gets and EPOLLIN event on a socket file descriptor. When it finishes, it simply closes the connection. The reason it uses sendfile is because it's faster since it doesn't pass the data to user-space, making the copying in kernel-space.

- dynamic files
```
    wget -t 1 "http://localhost:8888/dynamic/file.dat"
```
First it sends the header: 200 OK if it finds the file or 404 NOT FOUND, if it does not. Afterwards, it creates an io_contex and submits a job to read from the file fd to the buffer declared in the structure. Afterwards, it adds the eventfd descriptor(efd) to epoll's interest list. Now, epoll can notify when an operation is done on efd. Then, in the main function, it detects the number of efd operations done. It uses read instead of io_getevents because read is nonblocking(in this setup). If we have one efd operation done(the one we submited before, to read from the file to the buffer), then a flag gets set notifying the application that it can start to send bytes of data to the socket. When it finishes, it closes the connection.

## Extra
In order to force the program the break the messages for send/recv/sendfile, you can use sockop_preload.so, which has hooks written on top of the functions I mentioned. When run with the command below, it will replace the functionality of send/recv/sendfile by splitting in half the messages.

```
    LD_PRELOAD = ./sockop_preload.so ./aws
```

## Built With

- nonblocking web sockets
- sendfile()
- Linux Async I/O API


## Authors

* **Alexandru Meterez** - the one and only
* the author of the HTTP parser
## License

This project is licensed under the MIT License.