# sysrepo-gnxi

A C++ server based on [gNMI specification](https://github.com/openconfig/reference/blob/master/rpc/gnmi/gnmi-specification.md) to communicate with [sysrepo](https://github.com/sysrepo/sysrepo) datastore via gRPC protocol.

**Supported gNMI RPCs:**

* [x] Capabilities
* [X] Set
* [X] Get
* [X] Subscribe

**Supported YANG_RPC RPCs (defined in proto/yang_rpc.proto):**

* [X] Rpc

**Supported gNMI extensions:**

* [ ] Master Arbitration Extension
* [ ] History Extension
* [X] Commit Confirmed Extension
* [ ] Depth Extension
* [ ] Config Subscription Extension

**Supported encoding:**

* [X] JSON encoding (if you ask for `JSON` you will have `JSON_IETF`)
* [ ] Bytes encoding
* [ ] Proto encoding
* [ ] ASCII encoding
* [X] JSON IETF encoding

**Supported encryption & authentication/authorization**

The server supports two connection modes: **insecure connection** and **mTLS connection with username/password authentication/authorization**.
Insecure mode does not provide any encryption of the server/client communication and no authentication/authorization of any sorts, therefore it should only be used in fully secure environments.
Secure mode follows the gNMI specification and requires both the server and client to authenticate via certificate. On top of that the spec requires that the client authenticates using a username and password. The server checks them against the users stored in sysrepo-gnxi-users module data in sysrepo. Furthermore each user can have read-only and/or read-write access to multiple sysrepo modules. The server then checks each request and decides whether the particular user has the right to execute specific RPC. Rpc RPC does not need to be authorized, but since you can run any rpc or action on the server side via this, it can effectively overwrite data of any module and therefore bypass all authorization rules.

| RPC          | Required privilege (per module in RPC)                    |
|--------------|-----------------------------------------------------------|
| Capabilities | -                                                         |
| Get          | read-only or read-write                                   |
| Set          | read-write                                                |
| Subscribe    | read-only or read-write                                   |
| Rpc          | - (if enabled authorization does not work properly)       |

## Dependencies

- C++20 compiler
- cmake >= 3.12
- protobuf >= 3.12.4
- grpc (cpp) >= 1.30.2
- OpenSSL >= 3
- libyang-cpp (master branch)
- sysrepo-cpp (master branch)
  - libyang (devel branch)
  - sysrepo (devel branch)

## Build & Install

During install all necessary YANG modules are installed to sysrepo and if sysrepo does not contain any sysrepo-gnxi server configuration, the minimal configuration is stored and used. The minimal configuration can be found in `example_config/minimal-configuration.json` and the setup script executed during install in `scripts/setup.sh`.

```
mkdir -p build && cd build
cmake ..
make
make install
```

## Docker build

We currently support Ubuntu 22.04, 24.04 and 26.04, Fedora 43 and 44, OpenSUSE Tumbleweed and Leap 16, ArchLinux. See `docker/` folder.

```
sudo docker build --no-cache -t ubuntu26 -f ./docker/ubuntu26.Dockerfile .
sudo docker run -it ubuntu26
```

## Get started

- **INSECURE mode (no TLS connection & no username/password authentication/authorization):**
```
sysrepo-gnxi -f
gnmic capabilities --insecure -a 127.0.0.1 --port 50051
```

- **mTLS connection & username/password authentication/authorization:**
```
sysrepo-gnxi
gnmic capabilities --tls-key example_config/client.key --tls-cert example_config/client.crt --tls-ca example_config/ca.crt -u admin -p admin -a 127.0.0.1 --port 50051
```

To run the server in secure mode, you have to edit the server configuration using e.g. `sysrepocfg`. An example of possible configuration can be found in `example_config/configuration.json`.

## Users (ACL) utility

To correctly populate the users which are stored in sysrepo you can use the `sysrepo-gnxi-users` utility.

## Clients

Client application is not part of the project. You can use OpenConfig's [gnmic](https://gnmic.openconfig.net/).
