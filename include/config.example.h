#pragma once

// Copy this file to include/config.h and point both URLs at your LAN-only,
// read-only dashboard snapshot service.
#define HOMELAB_DOCKER_SNAPSHOT_URL \
  "http://dashboard.local:8080/data/docker.json"
#define HOMELAB_BESZEL_METRICS_URL \
  "http://dashboard.local:8080/data/beszel.json"
