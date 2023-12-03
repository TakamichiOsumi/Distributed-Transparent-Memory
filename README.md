# Distributed Transparent Memory
This is the implementation of distributed transparent memory in C, based on socket programming. This builds up one big memory for the user from several connected machines. User can perform operation to save and fetch their data. The locality which node is responsible for processing and storing user data is hiddenfrom users.

## Usage
Execute `make` in the directory and run each node by following command.

```
./DTM <ow node IP> <own node UDP port> <successor node UDP port> <own node TCP port>
```

After setting up all six nodes that construct a ring topology by indicating UDP port number of each other, intractive with any node to perform either PUT or GET method.
