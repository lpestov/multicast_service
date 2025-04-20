FROM gcc:latest

# Устанавливаем утилиты (включая bash, если нужно для отладки)
RUN apt-get update && apt-get install -y \
    net-tools \
    bash

# Создаём рабочую директорию
WORKDIR /app

# Копируем исходник
COPY server/server.cpp .

# Компилируем
RUN g++ -o server server.cpp -lpthread

# Открываем порт (необязательно, но хорошо для документации)
EXPOSE 8080/udp

# Указываем CMD для запуска сервера
CMD ["./server"]
