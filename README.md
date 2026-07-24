# sysrepo-gnxi

A C++ server based on [gNMI specification](https://github.com/openconfig/reference/blob/master/rpc/gnmi/gnmi-specification.md) to communicate with [sysrepo](https://github.com/sysrepo/sysrepo) datastore.

**Supported gNMI RPCs:**

* [x] Capabilities
* [X] Set
* [X] Get
* [X] Subscribe

**Supported gNXI RPCs (defined in proto/gnxi.proto):**

* [X] Rpc

**Supported gNMI extensions:**

* [ ] Master Arbitration Extension
* [ ] History Extension
* [X] Commit Confirmed Extension
* [ ] Depth Extension
* [ ] Config Subscription Extension

**Supported encoding:**

* [ ] No encoding / gNMI native encoding (use `PROTO`)
* [X] JSON IETF encoding  (use `JSON_IETF`)
* [X] JSON encoding (if you ask for `JSON` you will have `JSON_IETF`)
* [ ] ~~Protobuf encoding~~
* [ ] ~~Binary encoding~~
* [ ] ~~ASCII encoding~~

**Supported encryption & authentication methods:**

* [x] no encryption, no username/password (DEBUGGING ONLY)
* [ ] ~~no encryption, username/password~~
* [ ] ~~TLS/SSL encryption only~~
* [x] TLS/SSL encryption + username/password authentication (server key pair + CA certificate)
* [x] TLS/SSL encryption & authentication (server/client key pairs + CA certificate) (RECOMMENDED)

## Dependencies

- C++20 compiler
- cmake >= 3.18.1
- protobuf >= 3.12.4
- grpc (cpp) >= 1.30.2
- libyang-cpp (master branch)
- sysrepo-cpp (master branch)
  - libyang (devel branch)
  - sysrepo (devel branch)

## Build & Install

```
mkdir -p build && cd build
cmake ..
make
make install
```

## Docker build

We currently support Ubuntu Jammy, Noble and Resolute, Fedora 43 and 44, OpenSUSE Tumbleweed and Leap 16, ArchLinux. See `docker/` folder.

```
sudo docker build --no-cache -t ubuntu26 -f ./docker/ubuntu26.Dockerfile .
sudo docker run -it ubuntu26
```

## Get started

- **INSECURE mode with no TLS connection (no username/password authentication):**
```
sysrepo-gnxi -f
gnmic capabilities --insecure -a localhost --port 50051
```

- **TLS connection (username/password authentication):**
```
sysrepo-gnxi -k example_config/server.key -c example_config/server.crt -u cesnet -p cesnet -r example_config/ca.crt
gnmic capabilities -a localhost --port 50051 -u cesnet -p cesnet --tls-ca example_config/ca.crt
```

- **TLS connection (client certificate authentication):**
```
sysrepo-gnxi -k example_config/server.key -c example_config/server.crt -r example_config/ca.crt
gnmic capabilities -a localhost --port 50051 --tls-key example_config/client.key --tls-cert example_config/client.crt --tls-ca example_config/ca.crt
```

## Clients

Client application is not part of the project. You can use OpenConfig's [gnmic](https://gnmic.openconfig.net/).
