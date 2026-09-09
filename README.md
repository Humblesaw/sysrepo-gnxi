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

* [ ] JSON encoding
* [ ] Bytes encoding
* [ ] Proto encoding
* [ ] ASCII encoding
* [X] JSON IETF encoding

JSON encoding is not supported due to the nature of the data. Libyang and sysrepo libraries expect to deal with structured tree data as specified in RFC 7951.

**Supported encryption & authentication/authorization**

The server supports two connection modes: **insecure connection** and **mTLS connection with username/password authentication/authorization**.
Insecure mode does not provide any encryption of the server/client communication and no authentication/authorization of any sorts, therefore it should only be used in fully secure environments.
Secure mode follows the gNMI specification and requires both the server and client to authenticate via certificate. On top of that the spec requires that the client authenticates using a username and password. The server checks them against the users stored in sysrepo-gnxi-users module data in sysrepo. Furthermore each user can have read-only and/or read-write access to multiple sysrepo modules. The server then checks each request and decides whether the particular user has the right to execute specific RPC. Known limitation: the YANG_RPC `Rpc` RPC is only authenticated, not authorized against the user ACL — the only check is that it does not access the server's private modules (`sysrepo-gnxi-server`, `sysrepo-gnxi-users`). Since an RPC or action can modify arbitrary data, any authenticated user can effectively overwrite data of any module via this RPC and therefore bypass all authorization rules. For this reason the YANG_RPC service is not compiled in by default (CMake option `ENABLE_YANG_RPC`, see below).

| RPC          | Required privilege (per module in RPC)                    |
|--------------|-----------------------------------------------------------|
| Capabilities | -                                                         |
| Get          | read-only or read-write                                   |
| Set          | read-write                                                |
| Subscribe    | read-only or read-write                                   |
| Rpc          | - (authenticated but not authorized, see the note above)  |

## Dependencies

- C++20 compiler
- cmake >= 3.12
- protobuf >= 3.12.4
- grpc (cpp) >= 1.30.2
- OpenSSL >= 3
- crypt(3)
- libyang >= 6.2.8
- libyang-cpp >= 11
- sysrepo >= 5.2.17
- sysrepo-cpp >= 10

Please note that certain dependencies (namely grpc and protobuf) in most package repositories are usually divided into multiple packages (see `docker/` folder for the list of packages needed on specific distributions).

## Build & Install

During install all necessary YANG modules are installed to sysrepo and if sysrepo does not contain any sysrepo-gnxi server configuration, the minimal configuration is stored and used. The minimal configuration can be found in `example_config/minimal-configuration.json` and the setup script executed during install in `scripts/setup.sh`.

```
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
make install
```

The default build type is Debug; use `-DCMAKE_BUILD_TYPE=Release` for production builds. CMake options:

- `ENABLE_TESTS` — build the test-suite (OFF by default)
- `ENABLE_YANG_RPC` — register the YANG_RPC service (OFF by default, it does not support per-module authorization, see the note above)
- `ENABLE_COVERAGE` — build a code coverage report from tests (OFF by default)
- `SYSREPO_SETUP` — install the required YANG modules into sysrepo during `make install` (ON by default)

## Docker build

We currently support Ubuntu 22.04, 24.04 and 26.04, Fedora 43 and 44, OpenSUSE Tumbleweed and Leap 16, ArchLinux. See `docker/` folder.

```
sudo docker build --no-cache -t ubuntu26 -f ./docker/ubuntu26.Dockerfile .
sudo docker run -it ubuntu26
```

## Get started

Client application is not part of the project. You can use OpenConfig's [gnmic](https://gnmic.openconfig.net/install/).

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

To run the server in secure mode, you have to edit the server configuration using e.g. `sysrepocfg --edit=/path/to/configuration.json`. Example working configuration can be found in `example_config/configuration.json`. The configuration contains server key, server certificate, CA certificate (all of which can be found in `example_config/`), user `admin` with no module permissions and password `admin` and listening endpoint on 127.0.0.1:50051.

## Users (ACL) utility

To correctly populate the users which are stored in sysrepo you can use the `sysrepo-gnxi-users` utility.

```
# create a user with an ACL granting read-write access to the interfaces module
sysrepo-gnxi-users add --name alice --password 'secret' --rw interfaces
# grant additional read-only access later
sysrepo-gnxi-users edit --name alice --ro ietf-system
# review stored users and ACLs
sysrepocfg -X -x /sysrepo-gnxi-users:users -f json
# remove the user
sysrepo-gnxi-users remove --name alice
```

Passwords are never stored in plaintext: the utility hashes them into the `iana-crypt-hash` `crypt-hash` format — SHA-512-crypt (`$6$`) by default, with SHA-256-crypt (`$5$`) or MD5-crypt (`$1$`) selectable via `--hash`. Cleartext `$0$` values are rejected both by a YANG constraint of the module and by the authenticator. Stronger KDFs (Argon2id, bcrypt) are a possible future addition.

## Deviations from the gNMI specification

| Area | Spec requirement | Implementation |
|------|------------------|-----------------|
| Path `origin` | identifies the data origin of a path, e.g. `openconfig` | RFC 7951 module names are carried as the first path element instead; the `origin` must be `rfc7951` — on the request prefix, or on each path when the prefix carries no origin - otherwise the request is rejected |
| Encodings | target MUST support JSON | only `JSON_IETF` is supported, all other encodings are rejected with `UNIMPLEMENTED` |
| `SubscriptionList.use_aliases` | alias expansion | aliases are not supported, the flag is ignored |
| `GetRequest.use_models` | restrict the response to the listed models | rejected with `UNIMPLEMENTED` |
| Get data types | CONFIG / STATE / ALL / OPERATIONAL | `CONFIG` reads the running datastore, the other types read the operational datastore (which also includes the configuration) |
| `Set.union_replace` | union replace | rejected with `UNIMPLEMENTED` |
| Set extensions | registered extensions | only the Commit extension is supported, all others are rejected with `UNIMPLEMENTED` |
| `SubscriptionList.updates_only` | send only updates after establishment | rejected with `UNIMPLEMENTED` |
| `TARGET_DEFINED` subscription mode | target chooses ON_CHANGE or SAMPLE | rejected with `UNIMPLEMENTED` |
| `sample_interval` | 0 means the lowest interval possible for the target | 0 is clamped to the server minimum of 200 ms, intervals between 1 ns and 200 ms are rejected with `INVALID_ARGUMENT` |
| `allow_aggregation` | aggregation of subscriptions | ignored, warning logged |
| `qos` | DSCP marking | ignored, warning logged |
| `suppress_redundant` | suppress unchanged values | ignored, warning logged |
| `heartbeat_interval` | synthesize-on-change heartbeats | ignored, warning logged |
| gNMI extensions | Master Arbitration, History, Depth, Config Subscription | not supported, rejected with `UNIMPLEMENTED` |
| gRPC reflection | targets SHOULD register the server reflection service | not provided |

## License

This project is licensed under the Apache License 2.0, see the `./LICENSE` file for the full text. The codebase is derived from third-party code licensed under the Apache License 2.0 (Graphiant Inc., Yohan Pipereau — see the source file headers), which is why the Apache License is kept for the project as a whole.
