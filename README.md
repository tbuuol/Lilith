# Lilith

Lilith is a local JSON-RPC proxy for multiple blockchain nodes.

## Configuration

```sh
cp Lilith.conf.example Lilith.conf
chmod 600 Lilith.conf
```

Edit `Lilith.conf` and add a `[node:NAME]` section for each node with `host`, `port`, `user`, and `password`. Empty passwords are allowed when a node does not require RPC authentication. Configure the API port, UI port, and allowed browser origins in the `[server]` section. By default, the API listens on port `9999` and the UI on port `10000`.

The real configuration file is not tracked by version control. Lilith refuses to start if the file is missing or accessible to group or other users. Store credentials only in this file.

## Run

```sh
make
./Lilith                 # reads ./Lilith.conf
./Lilith /path/to.conf   # optional configuration path
```

At startup, Lilith probes every node with `getblockcount`. After a successful probe, it calls `help` and extracts the available RPC commands. The web console provides a command dropdown and fetches each command's help text when selected, including its usage and required or optional parameters. `GET /nodes` returns each node's `id`, `available`, and `commands`. Node status and command lists are startup snapshots; unavailable nodes remain configured and can still be called after connectivity is restored.

Both servers bind exclusively to `127.0.0.1`. Lilith serves the UI on port `10000`; its browser origin must be listed exactly in `allowed_origins`. With the example configuration, open:

```text
http://127.0.0.1:10000/
```

## Requests

Lilith accepts a POST containing a JSON object whose string field `id` matches a configured node name. It forwards the JSON-RPC request to that node.

The HTTP server limits headers to 16 KiB and request bodies to 1 MiB, applies read and write timeouts, and limits concurrent client connections. HTTP errors are returned with appropriate status codes.

At startup and for each request, Lilith logs diagnostic information about CORS, the RPC method, target node, upstream HTTP status, CURL errors, peer IP, duration, and response size. Passwords and RPC payloads are never logged.

Lilith is intended for trusted local use. For network access, put authentication and TLS in front of it using a dedicated reverse proxy.
