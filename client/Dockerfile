FROM gcc:latest
WORKDIR /app
COPY client/client.cpp .
RUN g++ -o client client.cpp -lpthread
CMD ["./client", "server:8080"]
