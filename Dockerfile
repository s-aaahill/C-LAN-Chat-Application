FROM gcc:13

WORKDIR /app

COPY . .

RUN g++ server.cpp -o server -pthread

EXPOSE 8080

CMD ["./server"]
