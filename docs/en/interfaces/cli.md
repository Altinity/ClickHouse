---
description: 'Documentation for clickhousectl, the CLI for ClickHouse: local and cloud'
sidebar_label: 'clickhousectl'
sidebar_position: 17
slug: /interfaces/cli
title: 'clickhousectl'
doc_type: 'reference'
---

import BetaBadge from '@theme/badges/BetaBadge';

<BetaBadge/>

`clickhousectl` is the CLI for ClickHouse: local and cloud.

With `clickhousectl` you can:
- Install and manage local ClickHouse versions
- Launch and manage local ClickHouse servers
- Execute queries against ClickHouse servers
- Set up ClickHouse Cloud and create cloud-managed ClickHouse clusters
- Manage ClickHouse Cloud resources
- Install the official ClickHouse agent skills into supported coding agents
- Push your local ClickHouse development to cloud

`clickhousectl` helps humans and AI-agents to develop with ClickHouse.

## Installation {#installation}

### Quick install {#quick-install}

```bash
curl https://clickhouse.com/cli | sh
```

The install script downloads the correct version for your OS and installs to `~/.local/bin/clickhousectl`. A `chctl` alias is also created automatically for convenience.

## Requirements {#requirements}

- macOS (aarch64, x86_64) or Linux (aarch64, x86_64)
- Cloud commands require a [ClickHouse Cloud API key](/cloud/manage/api/api-overview)

## Local {#local}

### Installing and managing ClickHouse versions {#installing-versions}

`clickhousectl` downloads ClickHouse binaries from [GitHub releases](https://github.com/ClickHouse/ClickHouse/releases).

```bash
# Install a version
clickhousectl local install stable          # Latest stable release
clickhousectl local install lts             # Latest LTS release
clickhousectl local install 26.3            # Latest 26.3.x.x
clickhousectl local install 26.3.4.3        # Exact version

# List versions
clickhousectl local list                    # Installed versions
clickhousectl local list --remote           # Available for download

# Manage default version
clickhousectl local use stable              # Latest stable (installs if needed)
clickhousectl local use lts                 # Latest LTS (installs if needed)
clickhousectl local use 26.3                # Latest 26.3.x.x (installs if needed)
clickhousectl local use 26.3.4.3            # Exact version
clickhousectl local which                   # Show current default

# Remove a version
clickhousectl local remove 26.3.4.3
```

#### ClickHouse binary storage {#binary-storage}

ClickHouse binaries are stored in a global repository, so they can be used by multiple projects without duplicating storage. Binaries are stored in `~/.clickhousectl/`:

```bash
~/.clickhousectl/
├── versions/
│   └── 26.3.4.3/
│       └── clickhouse
└── default              # tracks the active version
```

### Initializing a project {#initializing-project}

```bash
clickhousectl local init
```

`init` bootstraps your current working directory with a standard folder structure for your ClickHouse project files. It is optional; you are welcome to use your own folder structure if preferred.

It creates the following structure:

```bash
clickhouse/
├── tables/                 # Table definitions (CREATE TABLE ...)
├── materialized_views/     # Materialized view definitions
├── queries/                # Saved queries
└── seed/                   # Seed data / INSERT statements
```

### Running queries {#running-queries}

```bash
# Connect to a running server with clickhouse-client
clickhousectl local client                           # Connects to "default" server
clickhousectl local client --name dev                # Connects to "dev" server
clickhousectl local client --query "SHOW DATABASES"  # Run a query
clickhousectl local client --queries-file schema.sql # Run queries from a file
clickhousectl local client --host remote-host --port 9000  # Connect to a specific host/port
```

### Creating and managing ClickHouse servers {#managing-servers}

Start and manage ClickHouse server instances. Each server gets its own isolated data directory at `.clickhousectl/servers/<name>/data/`.

```bash
# Start a server (runs in background by default)
clickhousectl local server start                          # Named "default"
clickhousectl local server start --name dev               # Named "dev"
clickhousectl local server start --foreground             # Run in foreground (-F / --fg)
clickhousectl local server start --http-port 8124 --tcp-port 9001  # Explicit ports
clickhousectl local server start -- --config-file=/path/to/config.xml

# List all servers (running and stopped)
clickhousectl local server list

# Stop servers
clickhousectl local server stop default                   # Stop by name
clickhousectl local server stop-all                       # Stop all running servers

# Remove a stopped server and its data
clickhousectl local server remove test
```

**Server naming:** Without `--name`, the first server is called "default". If "default" is already running, a random name is generated (e.g. "bold-crane"). Use `--name` for stable identities you can start/stop repeatedly.

**Ports:** Defaults are HTTP 8123 and TCP 9000. If these are already in use, free ports are automatically assigned and shown in the output. Use `--http-port` and `--tcp-port` to set explicit ports.

#### Project-local data directory {#project-local-data}

All server data lives inside `.clickhousectl/` in your project directory:

```bash
.clickhousectl/
├── .gitignore              # auto-created, ignores everything
├── credentials.json        # cloud API credentials (if configured)
└── servers/
    ├── default/
    │   └── data/           # ClickHouse data files for "default" server
    └── dev/
        └── data/           # ClickHouse data files for "dev" server
```

Each named server has its own data directory, so servers are fully isolated from each other. Data persists between restarts — stop and start a server by name to pick up where you left off. Use `clickhousectl local server remove <name>` to permanently delete a server's data.

## Authentication {#authentication}

Authenticate to ClickHouse Cloud using OAuth (browser-based) or API keys.

### OAuth login (recommended) {#oauth-login}

```bash
clickhousectl cloud auth login
```

This opens your browser for authentication via the OAuth device flow. Tokens are saved to `.clickhousectl/tokens.json` (project-local).

### API key/secret {#api-key}

```bash
# Non-interactive (CI-friendly)
clickhousectl cloud auth login --api-key YOUR_KEY --api-secret YOUR_SECRET

# Interactive prompt
clickhousectl cloud auth login --interactive
```

Credentials are saved to `.clickhousectl/credentials.json` (project-local).

You can also use environment variables:
```bash
export CLICKHOUSE_CLOUD_API_KEY=your-key
export CLICKHOUSE_CLOUD_API_SECRET=your-secret
```

Or pass credentials directly via flags on any command:
```bash
clickhousectl cloud --api-key KEY --api-secret SECRET ...
```

### Auth status and logout {#auth-status}

```bash
clickhousectl cloud auth status    # Show current auth state
clickhousectl cloud auth logout    # Clear all saved credentials (credentials.json & tokens.json)
```

Credential resolution order: CLI flags > OAuth tokens > `.clickhousectl/credentials.json` > environment variables.

## Cloud {#cloud}

Manage ClickHouse Cloud services via the API.

### Organizations {#organizations}

```bash
clickhousectl cloud org list              # List organizations
clickhousectl cloud org get <org-id>      # Get organization details
clickhousectl cloud org update <org-id> --name "Renamed Org"
clickhousectl cloud org update <org-id> \
  --remove-private-endpoint pe-1,cloud-provider=aws,region=us-east-1 \
  --enable-core-dumps false
clickhousectl cloud org prometheus <org-id> --filtered-metrics true
clickhousectl cloud org usage <org-id> \
  --from-date 2024-01-01 \
  --to-date 2024-01-31
```

### Services {#services}

```bash
# List services
clickhousectl cloud service list

# Get service details
clickhousectl cloud service get <service-id>

# Create a service (minimal)
clickhousectl cloud service create --name my-service

# Create with scaling options
clickhousectl cloud service create --name my-service \
  --provider aws \
  --region us-east-1 \
  --min-replica-memory-gb 8 \
  --max-replica-memory-gb 32 \
  --num-replicas 2

# Create with specific IP allowlist
clickhousectl cloud service create --name my-service \
  --ip-allow 10.0.0.0/8 \
  --ip-allow 192.168.1.0/24

# Create from backup
clickhousectl cloud service create --name restored-service --backup-id <backup-uuid>

# Create with release channel
clickhousectl cloud service create --name my-service --release-channel fast

# Start/stop a service
clickhousectl cloud service start <service-id>
clickhousectl cloud service stop <service-id>

# Connect to a cloud service with clickhouse-client
clickhousectl cloud service client --name my-service --password secret
clickhousectl cloud service client --id <service-id> -q "SELECT 1" --password secret

# Use CLICKHOUSE_PASSWORD env var (recommended for scripts/agents)
CLICKHOUSE_PASSWORD=secret clickhousectl cloud service client \
  --name my-service -q "SELECT count() FROM system.tables"

# Update service metadata and patches
clickhousectl cloud service update <service-id> \
  --name my-renamed-service \
  --add-ip-allow 10.0.0.0/8 \
  --remove-ip-allow 0.0.0.0/0 \
  --release-channel fast

# Update replica scaling
clickhousectl cloud service scale <service-id> \
  --min-replica-memory-gb 24 \
  --max-replica-memory-gb 48 \
  --num-replicas 3 \
  --idle-scaling true \
  --idle-timeout-minutes 10

# Reset password with generated credentials
clickhousectl cloud service reset-password <service-id>

# Delete a service (must be stopped first)
clickhousectl cloud service delete <service-id>

# Force delete: stops a running service then deletes
clickhousectl cloud service delete <service-id> --force
```

#### Service create options {#service-create-options}

| Option | Description |
|--------|-------------|
| `--name` | Service name (required) |
| `--provider` | Cloud provider: `aws`, `gcp`, `azure` (default: `aws`) |
| `--region` | Region (default: `us-east-1`) |
| `--min-replica-memory-gb` | Min memory per replica in GB (8-356, multiple of 4) |
| `--max-replica-memory-gb` | Max memory per replica in GB (8-356, multiple of 4) |
| `--num-replicas` | Number of replicas (1-20) |
| `--idle-scaling` | Allow scale to zero (default: `true`) |
| `--idle-timeout-minutes` | Min idle timeout in minutes (>= 5) |
| `--ip-allow` | IP CIDR to allow (repeatable, default: `0.0.0.0/0`) |
| `--backup-id` | Backup ID to restore from |
| `--release-channel` | Release channel: `slow`, `default`, `fast` |

#### Query endpoint management {#query-endpoints}

```bash
clickhousectl cloud service query-endpoint get <service-id>
clickhousectl cloud service query-endpoint create <service-id> \
  --role admin \
  --open-api-key key-1 \
  --allowed-origins https://app.example.com
clickhousectl cloud service query-endpoint delete <service-id>
```

#### Private endpoint management {#private-endpoints}

```bash
clickhousectl cloud service private-endpoint create <service-id> --endpoint-id vpce-123
clickhousectl cloud service private-endpoint get-config <service-id>
```

#### Backup configuration {#backup-config}

```bash
clickhousectl cloud service backup-config get <service-id>
clickhousectl cloud service backup-config update <service-id> \
  --backup-period-hours 24 \
  --backup-retention-period-hours 720 \
  --backup-start-time 02:00
```

### Backups {#backups}

```bash
clickhousectl cloud backup list <service-id>
clickhousectl cloud backup get <service-id> <backup-id>
```

### Members {#members}

```bash
clickhousectl cloud member list
clickhousectl cloud member get <user-id>
clickhousectl cloud member update <user-id> --role-id <role-id>
clickhousectl cloud member remove <user-id>
```

### Invitations {#invitations}

```bash
clickhousectl cloud invitation list
clickhousectl cloud invitation create --email dev@example.com --role-id <role-id>
clickhousectl cloud invitation get <invitation-id>
clickhousectl cloud invitation delete <invitation-id>
```

### Keys {#keys}

```bash
clickhousectl cloud key list
clickhousectl cloud key get <key-id>
clickhousectl cloud key create --name ci-key --role-id <role-id> --ip-allow 10.0.0.0/8
clickhousectl cloud key update <key-id> \
  --name renamed-key \
  --expires-at 2025-12-31T00:00:00Z \
  --state disabled \
  --ip-allow 0.0.0.0/0
clickhousectl cloud key delete <key-id>
```

### Activity {#activity}

```bash
clickhousectl cloud activity list --from-date 2024-01-01 --to-date 2024-12-31
clickhousectl cloud activity get <activity-id>
```

### JSON output {#json-output}

Use the `--json` flag to print JSON-formatted responses.

```bash
clickhousectl cloud --json service list
clickhousectl cloud --json service get <service-id>
```

## Skills {#skills}

Install the official ClickHouse Agent Skills from [ClickHouse/agent-skills](https://github.com/ClickHouse/agent-skills).

```bash
# Default: interactive mode for humans, choose scope, then choose agents
clickhousectl skills

# Non-interactive: install into every supported project-local agent folder
clickhousectl skills --all

# Non-interactive: install only into detected agents
clickhousectl skills --detected-only

# Non-interactive: install into every supported global agent folder
clickhousectl skills --global --all

# Non-interactive: install into specific project-local agents
clickhousectl skills --agent claude --agent codex
```

### Non-interactive flags {#non-interactive-flags}

<<<<<<< HEAD
| Flag | Description |
|------|-------------|
| `--agent <name>` | Install Skills for a specific agent (can be repeated) |
| `--global` | Use global scope; if omitted, project scope is used |
| `--all` | Install Skills for all supported agents |
| `--detected-only` | Install Skills for supported agents that were detected on the system |
=======
```bash
clickhouse-client clickhouse://localhost/my_database?s

# equivalent to:
clickhouse-client clickhouse://localhost/my_database -s
```

Connect to the default host using the default port, the default user, and the default database.

```bash
clickhouse-client clickhouse:
```

Connect to the default host using the default port, as the user `my_user` and no password.

```bash
clickhouse-client clickhouse://my_user@

# Using a blank password between : and @ means to asking the user to enter the password before starting the connection.
clickhouse-client clickhouse://my_user:@
```

Connect to `localhost` using the email as the user name. `@` symbol is percent encoded to `%40`.

```bash
clickhouse-client clickhouse://some_user%40some_mail.com@localhost:9000
```

Connect to one of two hosts: `192.168.1.15`, `192.168.1.25`.

```bash
clickhouse-client clickhouse://192.168.1.15,192.168.1.25
```

## Query ID format {#query-id-format}

In interactive mode ClickHouse Client shows the query ID for every query. By default, the ID is formatted like this:

```sql
Query id: 927f137d-00f1-4175-8914-0dd066365e96
```

A custom format may be specified in a configuration file inside a `query_id_formats` tag. The `{query_id}` placeholder in the format string is replaced with the query ID. Several format strings are allowed inside the tag.
This feature can be used to generate URLs to facilitate profiling of queries.

**Example**

```xml
<config>
  <query_id_formats>
    <speedscope>http://speedscope-host/#profileURL=qp%3Fid%3D{query_id}</speedscope>
  </query_id_formats>
</config>
```

With the configuration above, the ID of a query is shown in the following format:

```response
speedscope:http://speedscope-host/#profileURL=qp%3Fid%3Dc8ecc783-e753-4b38-97f1-42cddfb98b7d
```

## Configuration files {#configuration_files}

ClickHouse Client uses the first existing file of the following:

- A file that is defined with the `-c [ -C, --config, --config-file ]` parameter.
- `./clickhouse-client.[xml|yaml|yml]`
- `$XDG_CONFIG_HOME/clickhouse/config.[xml|yaml|yml]` (or `~/.config/clickhouse/config.[xml|yaml|yml]` if `XDG_CONFIG_HOME` is not set)
- `~/.clickhouse-client/config.[xml|yaml|yml]`
- `/etc/clickhouse-client/config.[xml|yaml|yml]`

See the sample configuration file in the ClickHouse repository: [`clickhouse-client.xml`](https://github.com/ClickHouse/ClickHouse/blob/master/programs/client/clickhouse-client.xml)

<Tabs>
  <TabItem value="xml" label="XML" default>
    ```xml
    <config>
        <user>username</user>
        <password>password</password>
        <secure>true</secure>
        <openSSL>
          <client>
            <caConfig>/etc/ssl/cert.pem</caConfig>
          </client>
        </openSSL>
    </config>
    ```
  </TabItem>
  <TabItem value="yaml" label="YAML">
    ```yaml
    user: username
    password: 'password'
    secure: true
    openSSL:
      client:
        caConfig: '/etc/ssl/cert.pem'
    ```
  </TabItem>
</Tabs>

## Environment variable options {#environment-variable-options}

The user name, password and host can be set via environment variables `CLICKHOUSE_USER`, `CLICKHOUSE_PASSWORD` and `CLICKHOUSE_HOST`.
Command line arguments `--user`, `--password` or `--host`, or a [connection string](#connection_string) (if specified) take precedence over environment variables.

## Command-line options {#command-line-options}

All command-line options can be specified directly on the command line or as defaults in the [configuration file](#configuration_files).

### General options {#command-line-options-general}

| Option                                              | Description                                                                                                                        | Default                      |
|-----------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------|------------------------------|
| `-c [ -C, --config, --config-file ] <path-to-file>` | The location of the configuration file for the client, if it is not at one of the default locations. See [Configuration Files](#configuration_files). | -                            |
| `--help`                                            | Print usage summary and exit. Combine with `--verbose` to display all possible options including query settings.                  | -                            |
| `--history_file <path-to-file>`                     | Path to a file containing the command history.                                                                                     | -                            |
| `--history_max_entries`                             | Maximum number of entries in the history file.                                                                                     | `1000000` (1 million)        |
| `--prompt <prompt>`                                 | Specify a custom prompt.                                                                                                           | The `display_name` of the server |
| `--verbose`                                         | Increase output verbosity.                                                                                                         | -                            |
| `-V [ --version ]`                                  | Print version and exit.                                                                                                            | -                            |

### Connection options {#command-line-options-connection}

| Option                           | Description                                                                                                                                                                                                                                                                                                                        | Default                                                                                                          |
|----------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------|
| `--connection <name>`            | The name of preconfigured connection details from the configuration file. See [Connection credentials](#connection-credentials).                                                                                                                                                                                                   | -                                                                                                                |
| `-d [ --database ] <database>`   | Select the database to default to for this connection.                                                                                                                                                                                                                                                                             | The current database from the server settings (`default` by default)                                             |
| `-h [ --host ] <host>`           | The hostname of the ClickHouse server to connect to. Can either be a hostname or an IPv4 or IPv6 address. Multiple hosts can be passed via multiple arguments.                                                                                                                                                                    | `localhost`                                                                                                      |
| `--jwt <value>`                  | Use JSON Web Token (JWT) for authentication. <br/><br/>Server JWT authorization is only available in ClickHouse Cloud.                                                                                                                                                                                                            | -                                                                                                                |
| `--login[=<mode>]`       | Authenticate via OAuth2. Bare `--login` (no `=<mode>`) triggers ClickHouse Cloud automatic login — the provider is inferred from the server. To authenticate against a custom OpenID Connect provider, supply a `mode` and `--oauth-credentials`: `--login=browser` runs the Authorization Code + PKCE flow (opens a browser), `--login=device` runs the Device Authorization flow (prints a URL and short code — no browser needed). | -                                                                                                                |
| `--oauth-credentials <path>` | Path to an OAuth2 credentials JSON file (Google Cloud Console format). Required when using `--login=browser` or `--login=device` with a custom OpenID Connect provider. See [OAuth credentials file format](#oauth-credentials-file) below. Refresh tokens are cached in `~/.clickhouse-client/oauth_cache.json` (mode `0600`). | `~/.clickhouse-client/oauth_client.json`                                                                        |
| `--no-warnings`                  | Disable showing warnings from `system.warnings` when the client connects to the server.                                                                                                                                                                                                                                            | -                                                                                                                |
| `--no-server-client-version-message`                  | Suppress server-client version mismatch message when the client connects to the server.                                                                                                                                                                                                                                            | -                                                                                                                |
| `--password <password>`          | The password of the database user. You can also specify the password for a connection in the configuration file. If you do not specify the password, the client will ask for it.                                                                                                                                                   | -                                                                                                                |
| `--port <port>`                  | The port the server is accepting connections on. The default ports are 9440 (TLS) and 9000 (no TLS). <br/><br/>Note: The client uses the native protocol and not HTTP(S).                                                                                                                                                         | `9440` if `--secure` is specified, `9000` otherwise. Always defaults to `9440` if the hostname ends in `.clickhouse.cloud`. |
| `-s [ --secure ]`                | Whether to use TLS. <br/><br/>Enabled automatically when connecting to port 9440 (the default secure port) or ClickHouse Cloud. <br/><br/>You might need to configure your CA certificates in the [configuration file](#configuration_files). The available configuration settings are the same as for [server-side TLS configuration](../operations/server-configuration-parameters/settings.md#openssl). | Auto-enabled when connecting to port 9440 or ClickHouse Cloud                                                   |
| `--ssh-key-file <path-to-file>`  | File containing the SSH private key for authenticate with the server.                                                                                                                                                                                                                                                              | -                                                                                                                |
| `--ssh-key-passphrase <value>`   | Passphrase for the SSH private key specified in `--ssh-key-file`.                                                                                                                                                                                                                                                                 | -                                                                                                                |
| `--tls-sni-override <server name>`       | If using TLS, the server name (SNI) to pass in the handshake.                                                                                                                                                                                                                                                                                                    | The host provided via `-h` or `--host`.                                                                                                        |
| `-u [ --user ] <username>`       | The database user to connect as.                                                                                                                                                                                                                                                                                                   | `default`                                                                                                        |

:::note
Instead of the `--host`, `--port`, `--user` and `--password` options, the client also supports [connection strings](#connection_string).
:::

### OAuth credentials file {#oauth-credentials-file}

When using `--login=browser` or `--login=device` with a custom OpenID Connect provider, the client reads a credentials JSON file. The file uses the same format produced by the Google Cloud Console ("OAuth 2.0 Client IDs" → "Download JSON"):

```json
{
  "installed": {
    "client_id": "YOUR_CLIENT_ID",
    "client_secret": "YOUR_CLIENT_SECRET",
    "auth_uri": "https://accounts.google.com/o/oauth2/auth",
    "token_uri": "https://oauth2.googleapis.com/token",
    "redirect_uris": ["http://127.0.0.1"]
  }
}
```

The top-level key can be `installed` (desktop/CLI apps) or `web`. Required fields: `client_id`, `auth_uri`, `token_uri`. Optional fields:

| Field | Description |
|---|---|
| `client_secret` | Confidential-client secret. Omit (or leave empty) for OIDC public clients — the auth-code flow is always protected by PKCE and the device flow by the device code, so a secret is not required by the protocol. When the field is absent the client never sends a `client_secret` form parameter, which is the form public-client registrations require (Auth0, Microsoft Entra ID, Keycloak, Okta and others reject empty secrets with `invalid_client`). |
| `device_authorization_uri` | Device authorization endpoint. Discovered automatically via OIDC Discovery if absent. |
| `issuer` | OIDC issuer URL (e.g. `https://accounts.google.com`). Used to locate the discovery document when `device_authorization_uri` is not set. |

The default path is `~/.clickhouse-client/oauth_client.json`. Override it with `--oauth-credentials <path>`.

After a successful login the obtained refresh token is cached in `~/.clickhouse-client/oauth_cache.json` (file mode `0600`). Subsequent runs reuse the cached token silently and only open the browser or print a device code when the refresh token has expired.

### Query options {#command-line-options-query}

| Option                          | Description                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
|---------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `--param_<name>=<value>`        | Substitution value for a parameter of a [query with parameters](#cli-queries-with-parameters).                                                                                                                                                                                                                                                                                                                                                                                                    |
| `-q [ --query ] <query>`        | The query to run in batch mode. Can be specified multiple times (`--query "SELECT 1" --query "SELECT 2"`) or once with multiple semicolon-separated queries (`--query "SELECT 1; SELECT 2;"`). In the latter case, `INSERT` queries with formats other than `VALUES` must be separated by empty lines. <br/><br/>A single query can also be specified without a parameter: `clickhouse-client "SELECT 1"` <br/><br/>Cannot be used together with `--queries-file`.                               |
| `--queries-file <path-to-file>` | Path to a file containing queries. `--queries-file` can be specified multiple times, e.g. `--queries-file queries1.sql --queries-file queries2.sql`. <br/><br/>Cannot be used together with `--query`.                                                                                                                                                                                                                                                                                            |
| `-m [ --multiline ]`            | If specified, allow multiline queries (do not send the query on Enter). Queries will be sent only when they are ended with a semicolon.                                                                                                                                                                                                                                                                                                                                                           |

### Query settings {#command-line-options-query-settings}

Query settings can be specified as command-line options in the client, for example:
```bash
$ clickhouse-client --max_threads 1
```

See [Settings](../operations/settings/settings.md) for a list of settings.

### Formatting options {#command-line-options-formatting}

| Option                    | Description                                                                                                                                                                                                                   | Default        |
|---------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|----------------|
| `-f [ --format ] <format>` | Use the specified format to output the result. <br/><br/>See [Formats for Input and Output Data](formats.md) for a list of supported formats.                                                                                | `TabSeparated` |
| `--pager <command>`       | Pipe all output into this command. Typically `less` (e.g., `less -S` to display wide result sets) or similar.                                                                                                                | -              |
| `-E [ --vertical ]`       | Use the [Vertical format](/interfaces/formats/Vertical) to output the result. This is the same as `–-format Vertical`. In this format, each value is printed on a separate line, which is helpful when displaying wide tables. | -              |

### Execution details {#command-line-options-execution-details}

| Option                            | Description                                                                                                                                                                                                                                                                                                         | Default                                                             |
|-----------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------|
| `--enable-progress-table-toggle`  | Enable toggling of the progress table by pressing the control key (Space). Only applicable in interactive mode with progress table printing enabled.                                                                                                                                                                | `enabled`                                                           |
| `--hardware-utilization`          | Print hardware utilization information in progress bar.                                                                                                                                                                                                                                                             | -                                                                   |
| `--memory-usage`                  | If specified, print memory usage to `stderr` in non-interactive mode. <br/><br/>Possible values: <br/>• `none` - do not print memory usage <br/>• `default` - print number of bytes <br/>• `readable` - print memory usage in human-readable format                                                                | -                                                                   |
| `--print-profile-events`          | Print `ProfileEvents` packets.                                                                                                                                                                                                                                                                                      | -                                                                   |
| `--progress`                      | Print progress of query execution. <br/><br/>Possible values: <br/>• `tty\|on\|1\|true\|yes` - outputs to the terminal in interactive mode <br/>• `err` - outputs to `stderr` in non-interactive mode <br/>• `off\|0\|false\|no` - disables progress printing                                                       | `tty` in interactive mode, `off` in non-interactive (batch) mode    |
| `--progress-table`                | Print a progress table with changing metrics during query execution. <br/><br/>Possible values: <br/>• `tty\|on\|1\|true\|yes` - outputs to the terminal in interactive mode <br/>• `err` - outputs to `stderr` non-interactive mode <br/>• `off\|0\|false\|no` - disables the progress table                      | `tty` in interactive mode, `off` in non-interactive (batch) mode    |
| `--stacktrace`                    | Print stack traces of exceptions.                                                                                                                                                                                                                                                                                   | -                                                                   |
| `-t [ --time ]`                   | Print query execution time to `stderr` in non-interactive mode (for benchmarks).                                                                                                                                                                                                                                    | -                                                                   |
>>>>>>> 67683cd1b46 (Merge pull request #1606 from Altinity/feature/client-IdP)
