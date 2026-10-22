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

The server supports two connection modes: **insecure connection** and **mTLS connection with username/password authentication**.
Insecure mode does not provide any encryption of the server/client communication and no authentication/authorization of any sorts, therefore it should only be used in fully secure environments.
Secure mode follows the gNMI specification and requires both the server and client to authenticate via certificate. On top of that the spec requires that the client authenticates using a username and password. The server checks them against the users stored in `sysrepo-gnxi-users` module data in sysrepo. Authorization is then enforced by sysrepo itself using standard NACM (`ietf-netconf-acm`, RFC 8341), the authenticated username is attached to every RPC's sysrepo session and sysrepo checks the configured NACM rule-lists on each operation.

| RPC          | NACM access needed (per affected module)                                    |
|--------------|-----------------------------------------------------------------------------|
| Capabilities | -                                                                           |
| Get          | read (unreadable data is silently filtered out of the response)             |
| Set          | create/update/delete per edit (denied edits fail with `PERMISSION_DENIED`)  |
| Subscribe    | read (initial/polled data and streamed changes are filtered per recipient)  |
| Rpc          | exec on the invoked RPC/action (denied exec fails with `PERMISSION_DENIED`) |

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

To run the server in secure mode, you have to edit the server configuration using e.g. `sysrepocfg --edit=/path/to/configuration.json`. Example working configuration can be found in `example_config/configuration.json`. The configuration contains server key, server certificate, CA certificate (all of which can be found in `example_config/`), user `admin` with password `admin`, a NACM rule-list granting full access and listening endpoint on 127.0.0.1:50051.

## Users utility

To populate the user database stored in sysrepo you can use the `sysrepo-gnxi-users` utility. It manages accounts only, authorization is configured through NACM rule-lists in `ietf-netconf-acm`.

```
# create a user
sysrepo-gnxi-users add --name alice --password secret

# change the password later
sysrepo-gnxi-users edit --name alice --password newsecret

# review stored users (also shows the NACM groups each user belongs to)
sysrepo-gnxi-users show
sysrepo-gnxi-users show --name alice

# remove the user (optionally also stripping its name from NACM groups)
sysrepo-gnxi-users remove --name alice
sysrepo-gnxi-users remove --name alice --purge-nacm
```

Passwords are never stored in plaintext: the utility hashes them into the `iana-crypt-hash` `crypt-hash` format — SHA-512-crypt (`$6$`) by default, with SHA-256-crypt (`$5$`) or MD5-crypt (`$1$`) selectable via `--hash`. Cleartext `$0$` values are rejected both by a YANG constraint of the module and by the authenticator. Stronger KDFs (Argon2id, bcrypt) are a possible future addition.

## Deviations from the gNMI specification

| Area | Spec requirement | Implementation |
|------|------------------|-----------------|
| Path `origin` | identifies the data origin of a path, e.g. `openconfig` | the origins `openconfig` (also the default when unspecified) and `rfc7951` are supported and address the same sysrepo datastore, differing only in path naming (`rfc7951` emits module-qualified element names, `openconfig` bare local names, both forms are accepted on input), a YANG module annotated with `oc-ext:origin` is only addressable under the annotated origin, unannotated modules under all origins, any other origin is rejected with `UNIMPLEMENTED` |
| Path element key values | arbitrary string key values | a key value containing both single and double quotes cannot be expressed (XPath 1.0 predicates have no escape mechanism) and is rejected with `INVALID_ARGUMENT` |
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

## Contributing

In case you want to contribute to the project, please follow the latest commit and coding style. To format the source code use `make format` target. Single and multi-line comments are both written in `//` style. For annotating files, classes, functions etc. use doxygen-style comments. Naming styles are combined, for classes and structures we use `PascalCase`, for macros and constants `UPPER_SNAKE_CASE`, for functions and variables `lower_snake_case` and for private members and private methods `lower_snake_case_` (ending with _).

## License

This project is licensed under the Apache License 2.0, see the `./LICENSE` file for the full text. The codebase is derived from third-party code licensed under the Apache License 2.0 (Graphiant Inc., Yohan Pipereau — see the source file headers), which is why the Apache License is kept for the project as a whole.
