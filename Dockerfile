FROM gcc:14

WORKDIR /app

COPY Makefile .
COPY src/ ./src
COPY include/ ./include/

RUN make

EXPOSE 8080

CMD ["./build/server"]
