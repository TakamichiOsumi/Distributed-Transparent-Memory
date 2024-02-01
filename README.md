# Distributed Transparent Memory
This is the implementation of distributed transparent memory in C, based on socket programming. This builds up one big memory for the user from several connected machines. User can perform operation to save and fetch their data. The locality which node is responsible for processing and storing user data is hidden from users.

## Usage
Execute `make` in the directory and run each node by following command.

```
./DTM <ow node IP> <own node UDP port> <successor node UDP port> <own node TCP port>
```

The DTM expects user to run six machine nodes at the beginning so that it constructs a ring topology by indicating UDP port numbers of each other. Then, user can interact with any node to perform either PUT or GET method, following the instruction of the console.
