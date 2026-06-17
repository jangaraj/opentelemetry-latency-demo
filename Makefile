# Build the OpenTelemetry test clients (golang, cpp, rust) as Docker images.
#
# Each client lives in its own subfolder with a Dockerfile. The images are
# named $(IMAGE_PREFIX)-<client>, e.g. otel-http-client-golang.
#
# Usage:
#   make            # build all clients
#   make golang     # build a single client (golang|cpp|rust)
#   make run        # run all built client images in the background
#   make stop       # stop all running client containers
#   make clean      # remove the built images

DOCKER       ?= docker
IMAGE_PREFIX ?= otel-http-client
CLIENTS      := golang cpp rust
RUN_FLAGS    ?= --network host

.PHONY: all $(CLIENTS) run stop clean help

all: $(CLIENTS)

$(CLIENTS):
	$(DOCKER) build -t $(IMAGE_PREFIX)-$@ $@

run:
	@for client in $(CLIENTS); do \
		echo "Starting container $(IMAGE_PREFIX)-$$client in background..."; \
		$(DOCKER) run -d --name $(IMAGE_PREFIX)-$$client --rm $(RUN_FLAGS) $(IMAGE_PREFIX)-$$client; \
	done
	@echo "All agents started. View logs with 'docker logs -f $(IMAGE_PREFIX)-<client>'."

stop:
	@for client in $(CLIENTS); do \
		echo "Stopping container $(IMAGE_PREFIX)-$$client..."; \
		$(DOCKER) rm -f $(IMAGE_PREFIX)-$$client 2>/dev/null || true; \
	done

clean:
	-$(DOCKER) rmi $(addprefix $(IMAGE_PREFIX)-,$(CLIENTS))

help:
	@echo "Targets:"
	@echo "  all (default)  build all clients: $(CLIENTS)"
	@echo "  golang|cpp|rust  build a single client image"
	@echo "  run            run all built client images in the background"
	@echo "  stop           stop all running client containers"
	@echo "  clean          remove the built images"
