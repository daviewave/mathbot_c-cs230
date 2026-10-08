# Container image for the local math-speak server (docs/server.md). Two stages: the first
# compiles server/mathbot_server.c with the same strict flags the Makefile uses, the second
# ships only the resulting binary on a fresh debian:bookworm-slim as a non-root user.
# Build from the repository root so .dockerignore keeps the context down to server/:
#   docker build -f server/Dockerfile -t mathbot-server .
#   docker run --rm -p 27993:27993 mathbot-server
# This file was authored without a reachable Docker daemon; see docs/server.md.

FROM debian:bookworm-slim AS build

# gcc on bookworm-slim does not pull in the C headers without recommends, hence libc6-dev.
# make is not needed: the build is one gcc command, no Makefile is copied in.
RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY server/mathbot_server.c .

# Same warnings as CFLAGS in the Makefile (docs/conventions.md section 2) without -Werror: the
# repository gate enforces zero warnings with the development compiler, and bookworm's gcc 12 must
# not fail the image build over a diagnostic that differs between compiler versions.
RUN gcc -O2 -std=c99 -D_POSIX_C_SOURCE=200809L \
        -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
        -Wconversion -Wvla \
        mathbot_server.c -o mathbot_server

FROM debian:bookworm-slim

# A system account with no home and no shell: the server needs nothing but a socket.
RUN useradd --system --no-create-home --shell /usr/sbin/nologin mathbot

COPY --from=build /build/mathbot_server /usr/local/bin/mathbot_server

USER mathbot
# The server reads MATHBOT_PORT when no argument is given; MATHBOT_SECRET and MATHBOT_PROBLEMS
# are optional and passed with `docker run -e` or compose's environment block.
ENV MATHBOT_PORT=27993
EXPOSE 27993
ENTRYPOINT ["/usr/local/bin/mathbot_server"]
