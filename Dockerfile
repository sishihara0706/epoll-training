FROM gcc:14

WORKDIR /app

COPY Makefile .
COPY src/ ./src

RUN make

EXPOSE 8080

CMD ["./build/server"]
