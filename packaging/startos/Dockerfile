# SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
# SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
FROM alpine:3.22@sha256:49340b346798875b5592e5d420241f2f9cfba5f47e62ebb1446a1b34734e898f AS build
RUN apk add --no-cache build-base=0.5-r3 gcc=14.2.0-r6 binutils=2.44-r3 \
    cmake=3.31.7-r1 python3=3.12.16-r0 pkgconf=2.4.3-r0 \
    musl-dev=1.2.5-r13 openssl-dev=3.5.9-r1 openssl-libs-static=3.5.9-r1 \
    libevent-dev=2.1.13-r0 libevent-static=2.1.13-r0 zlib-dev=1.4-r0 zlib-static=1.4-r0
WORKDIR /source
COPY apps apps
COPY assets/brand/dispatch-icon.svg assets/brand/dispatch-icon.svg
COPY core core
COPY integrations integrations
COPY .startos-inputs/http-parser /http-parser
ARG DISPATCH_VERSION
RUN test -n "$DISPATCH_VERSION" && \
    cmake -S apps/startos -B /build -DCMAKE_BUILD_TYPE=Release -DDISPATCH_STATIC=ON \
      -DDISPATCH_HTTP_PARSER=/http-parser -DDISPATCH_VERSION="$DISPATCH_VERSION" && \
    cmake --build /build --parallel 4 && strip /build/dispatch-preview
FROM scratch
COPY --from=build /build/dispatch-preview /dispatch-preview
ENTRYPOINT ["/dispatch-preview"]
