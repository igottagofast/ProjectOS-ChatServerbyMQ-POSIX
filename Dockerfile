FROM ubuntu:22.04
FROM gcc:latest
WORKDIR /app
COPY . .
CMD ["/bin/bash"]
