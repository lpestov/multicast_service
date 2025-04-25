FROM gcc:latest
RUN apt-get update && apt-get install -y \
    net-tools \
    bash

WORKDIR /app
COPY server/server.cpp .
RUN g++ -o server server.cpp -lpthread
EXPOSE 8080/udp
CMD ["./server"]