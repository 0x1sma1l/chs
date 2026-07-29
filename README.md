# chs

A small HTTP file server written as a systems-programming exercise in C.

The project intentionally works directly with POSIX APIs to explore:

- sockets and file descriptors;
- manual resource and buffer management;
- I/O multiplexing with `select`;
- parsing basic HTTP requests;
- streaming files and preventing path traversal.

Files are served from `./public` on port `4000`.

```sh
make server
./server
```

This is a learning project, not a production-ready HTTP server. The low-level
API usage and explicit resource management are intentional design choices.
