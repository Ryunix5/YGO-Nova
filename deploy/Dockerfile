# EdoPro+ online relay server.
#
# Pure-stdlib Python — no pip dependencies. Build from the REPO ROOT:
#   docker build -f deploy/Dockerfile -t edopro-relay .
#   docker run -d --restart unless-stopped -p 7879:7879 --name edopro-relay edopro-relay
#
FROM python:3.12-slim

WORKDIR /app
COPY tools/relay_server.py /app/relay_server.py

ENV EDOPRO_RELAY_HOST=0.0.0.0 \
    EDOPRO_RELAY_PORT=7879

EXPOSE 7879

# -u = unbuffered stdout so `docker logs` is live.
ENTRYPOINT ["python", "-u", "relay_server.py"]
