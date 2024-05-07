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
