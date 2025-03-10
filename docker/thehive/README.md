# TheHive and Cortex

> **Warning** The TheHive and Cortex integrations are considered experimental
> and subject to change without notice.

This integration brings together Tenzir and TheHive, supporting the following use
cases:

1. Trigger a historical query to contextualize an observable through a [Cortex
  Analyzer][cortex-analyzers-docs]. This integration spawns an historical query
  directly from the TheHive UI.

2. Forward alerts from Tenzir to TheHive. We provide a small "app" that loads is
  capable of loading both stored and continuously incoming alerts from a running
  Tenzir instance.

### The Tenzir Cortex Analyzer

The analyzer can issue queries for observables with the following types:

- **ip**: match all events that have a corresponding value in any field with the
  `:ip` type.

- **subnet**: match all events that have a value in any field with the `:ip` type
  that belongs to that subnet.

- **hash/domain**: match all events that contain the associated strings.

> **Note** Queries issued to Tenzir have limit of 30 results. They are displayed
> in plain JSON.

### TheHive app

The app performs both a historical and a continuous query on Suricata alerts.

- For the historical query, the number of events is limited to 100 (this can be
  increased with the `BACKFILL_LIMIT` variable).

- Alerts are de-duplicated before being ingested into TheHive. Only alerts with
  a unique start time / flow id pair are considered.

> **Note** The app is currently only capable of processing Suricata alerts. The
> conversion to TheHive entities is hardcoded. Our [vision][vision-page] is to
> decouple the integrations by creating a unified Fabric capable of abstracting
> away concepts such as alerts. This will be enabled in particular by our
> current work on [pipelines][pipeline-page].

[vision-page]: https://docs.tenzir.com/vision
[pipeline-page]: https://github.com/tenzir/tenzir/pull/2577

## Configurations

This directory contains the Docker Compose setup to run a preconfigured instance
of TheHive with a Tenzir [Cortex Analyzer][cortex-analyzers-docs]. This stack can
run with multiple levels of integration with the Tenzir service:

- `thehive.yaml`: no dependency on the Tenzir Compose service.

- `thehive.yaml` + `thehive.tenzir.yaml`: the analyzer can access the Tenzir Compose
  service.

- `thehive.yaml` + `thehive.tenzir.yaml` + `thehive.app.yaml`: the app is relaying
  events between Tenzir and TheHive.

By default, TheHive is exposed on `http://localhost:9000`. We create default
users for both TheHive and Cortex:

- `admin@thehive.local`/`secret`: organization management

- `orgadmin@thehive.local`/`secret`: user and case management within the default
  (Tenzir) organization

These settings can be configured using [environment
variables](../compose/thehive-env.example).

## Standalone (or with Tenzir running locally)

If you have Tenzir instance running locally already or you don't plan on using the
Cortex Tenzir Analyzer, run the default configuration:

```bash
docker compose -f thehive.yaml up --build
```

You can also use the `TENZIR_ENDPOINT` environment variable to target a remote
Tenzir server.

## With Tenzir running as a Compose service

If you want to connect to a Tenzir server running as a Docker Compose service,
some extra networking settings required. Those are specified in
`thehive.tenzir.yaml`. For instance, from the `docker/compose` directory run:

```bash
docker compose \
    -f thehive.yaml \
    -f thehive.tenzir.yaml \
    -f tenzir.yaml \
    up --build
```

## With the Tenzir app

We provide a basic integration script that listens on `suricata.alert` events
and forwards them to TheHive. You can start it along the stack by running:

```bash
# from the docker/compose directory
export COMPOSE_FILE="tenzir.yaml"
COMPOSE_FILE="$COMPOSE_FILE:thehive.yaml"
COMPOSE_FILE="$COMPOSE_FILE:thehive.tenzir.yaml"
COMPOSE_FILE="$COMPOSE_FILE:thehive.app.yaml"

docker compose up --build --detach
```

To test the alert forwarding with some mock data, run:
```bash
# from the docker/compose directory with the COMPOSE_FILE variable above
docker compose run --no-TTY tenzir import --blocking suricata \
    < ../../tenzir/functional-test/data/suricata/eve.json
```

You can also test it out on a real-world dataset:
```bash
wget -O - -o /dev/null https://storage.googleapis.com/tenzir-public-data/malware-traffic-analysis.net/2020-eve.json.gz | \
  gzip -d | \
  head -n 1000 | \
  docker compose run --no-TTY tenzir import --blocking suricata
```

> **Note** To avoid forwarding the same alert multiple times, we use the hash of
> the connection timestamp and the flow id as `sourceRef` of the TheHive alert.
> This field is guarantied to be unique by TheHive.

[cortex-analyzers-docs]: https://docs.thehive-project.org/cortex/
