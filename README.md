# Distributed Transparent Memory
This is the implementation of distributed transparent memory in C, based on socket programming. This builds up one big memory for the user from several connected machines. User can perform operation to save and fetch their data. The locality which node is responsible for processing and storing user data is hidden from users.

## Usage
Execute `make` in the directory and run each node by following command.

```
./DTM <ow node IP> <own node UDP port> <successor node UDP port> <own node TCP port>
```

The DTM expects user to run six machine nodes at the beginning so that it constructs a ring topology by indicating UDP port numbers of each other. Then, user can interact with any node to perform either PUT or GET method.

Both commands have inputs described below:

1. PUT requires Key K and Value X, where K and X are two integers and make the pair in the distributed memory.
2. GET requires Key K, where K is an integer, to get registered X from K in the same.

Below is an example of the configuration.

| own node IP | own node UDP port number | successor node UDP port number | own node TCP port number | Notes |
| ---- | ---- | ---- | ---- | ---- |
| 127.0.0.1 | 60000 | 60001 | 50000 |  |
| 127.0.0.1 | 60001 | 60002 | 50001 |   |
| 127.0.0.1 | 60002 | 60003 | 50002 |  |
| 127.0.0.1 | 60003 | 60004 | 50003 |  |
| 127.0.0.1 | 60005 | 60006 | 50004 |  |
| 127.0.0.1 | 60006 | 60000 | 50005 | Points to the first node UDP port |
