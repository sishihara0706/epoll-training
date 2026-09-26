# epoll-training

Linux の `epoll`、ノンブロッキングI/O、TCPソケットの挙動、イベント駆動型サーバー設計を学ぶためのハンズオンプロジェクトです。

C言語でTCP Echo Serverを実装し、単一の `epoll` イベントループで複数クライアントを同時に処理します。

また、ShellとPythonによる負荷試験も用意し、接続処理、TCPバッファ、バックプレッシャー、`EAGAIN`、`EPOLLOUT` などを実際に観察できるようにしています。

[English version](README.md)

## 目的

このプロジェクトの主な目的は、Linux上のネットワークサーバーがフレームワークの下でどのように動作しているかを理解することです。

主に以下のテーマを扱っています。

* `select` / `poll` / `epoll`
* ノンブロッキングソケット
* `EPOLLIN` / `EPOLLOUT`
* partial read / partial write
* `EAGAIN` / `EWOULDBLOCK`
* `EINTR`
* TCP送受信バッファ
* `Recv-Q` / `Send-Q`
* TCPバックプレッシャー
* クライアントごとの状態管理
* `timerfd` を利用した定期処理
* file descriptorの再利用
* `SIGPIPE` / `EPIPE`
* `strace` によるsystem call観察
* `ss` によるTCP状態観察

## 主な機能

* C言語によるTCP Echo Server
* `epoll` を使ったI/O多重化
* ノンブロッキングソケット
* 複数クライアントの同時接続
* `timerfd` による接続中クライアント数の定期報告
* `accept4()` による

  * `SOCK_NONBLOCK`
  * `SOCK_CLOEXEC`
    の設定
* `epoll_event.data.ptr` を利用したクライアント状態管理
* partial writeへの対応
* 書き込み完了できない場合のみ `EPOLLOUT` を登録
* `EINTR` のリトライ処理
* `EAGAIN` / `EWOULDBLOCK` の処理
* クライアント切断処理
* `SIGPIPE` / `EPIPE` の考慮
* Shellによる同時接続テスト
* Python `asyncio` による負荷試験
* slow readerによるTCPバックプレッシャー試験

## アーキテクチャ

サーバーは典型的なイベント駆動型の構成になっています。

```text
socket()
   |
bind()
   |
listen()
   |
epoll_create1()
   |
listener socket を epoll に登録
   |
epoll_wait()
   |
   +-- listener に EPOLLIN
   |       |
   |     accept4()
   |       |
   |     client状態を確保
   |       |
   |     client socketをepollへ登録
   |
   +-- client に EPOLLIN
   |       |
   |     read()
   |       |
   |     echo用データを作成
   |       |
   |     writeできるだけ送信
   |       |
   |       +-- 全部送信できた
   |       |       |
   |       |     EPOLLINのみ監視
   |       |
   |       +-- EAGAIN
   |               |
   |             未送信データを保持
   |               |
   |             EPOLLOUTを追加
   |
   +-- client に EPOLLOUT
   |       |
   |     未送信データの続きからwrite
   |       |
   |       +-- 全部送信
   |               |
   |             EPOLLOUTを解除
   |
   +-- timerfd に EPOLLIN（10秒ごと）
           |
         timerの満了回数をread
           |
         接続中のクライアント数を報告
```

## クライアントごとの状態管理

各クライアントごとに送信状態を保持します。

```c
struct st_client {
    struct fd_info base;

    char outbuff[BUFF_SIZE + 2];
    size_t out_len;
    size_t out_pos;
};
```

`out_len` は送信予定の総バイト数です。

`out_pos` はすでに送信済みのバイト数です。

例えば、

```text
out_len = 1000
out_pos = 600
```

であれば、残り400 byteを送信する必要があります。

この状態を `epoll_wait()` をまたいで保持します。

## なぜEPOLLOUTが必要なのか

ノンブロッキングソケットでは、`write()` が要求された全データを一度に送信できる保証はありません。

例えば、

```text
1000 bytes送信したい
        |
write() -> 600 bytes
        |
write() -> -1 / EAGAIN
        |
残り400 bytes
        |
EPOLLOUTを有効化
        |
epoll_wait()
        |
socketが再びwrite可能になる
        |
600 byte目から送信を再開
```

という動きになります。

通常は、

```text
EPOLLIN
```

のみを監視します。

`write()` が完了できず、

```text
-1 / EAGAIN
```

になった場合だけ、

```text
EPOLLIN | EPOLLOUT
```

へ変更します。

未送信データをすべて送信したら、再び `EPOLLOUT` を解除します。

これにより、送信データがないのに `EPOLLOUT` が大量に通知され続けることを防ぎます。

## ビルド

```bash
gcc -Wall -Wextra -Wpedantic -Og -g server.c -o server
```

## 実行

```bash
./server
```

TCP port `8080` で待ち受けます。

```text
listening on 8080
```

## 接続数の定期モニタリング

サーバーはノンブロッキングな `timerfd` を作成し、listener socketやclient
socketと同じ `epoll` インスタンスへ登録します。timerは10秒ごとに満了するため、
別スレッドやsignal handlerを使わず、イベントループ内で定期処理を実行できます。

timerイベントを受け取るたびに、現在接続中のクライアント数と、`read()` で取得した
timerの満了回数を表示します。

```text
active clients: 3
timer fired: 1 time(s)
```

満了回数は通常 `1` です。イベントループがtimerをすぐに処理できなかった場合は、
`timerfd` が未処理の満了回数を蓄積するため、`2` 以上になることがあります。

## 手動テスト

別ターミナルから `nc` で接続します。

```bash
nc 127.0.0.1 8080
```

例:

```text
hello
hello
```

複数のターミナルから同時に接続することで、複数クライアントの処理を確認できます。

## 同時接続テスト

Shellスクリプトを使って、多数のクライアントを並列に接続できます。

例:

```bash
for i in $(seq 1 100); do
    {
        printf "client-%d\n" "$i"
        sleep 5
    } | nc 127.0.0.1 8080 &
done

wait
```

このテストでは、およそ100クライアントを同時接続し、それぞれの接続を処理できることを確認しました。

## Python負荷試験

このリポジトリには、Pythonの `asyncio` を利用したテストも含まれています。

典型的な負荷試験では、多数のクライアントが並行して接続し、繰り返しEchoリクエストを送信します。

例えば、

```text
100 clients
x
1000 messages
=
100,000 echo requests
```

のような負荷を与えることができます。

これにより以下を確認できます。

* 複数クライアント同時接続
* 連続したread / write
* イベントループの挙動
* 切断処理
* 負荷時のsocket挙動

## Slow Reader Test

`EPOLLOUT` やTCPバックプレッシャーを確認するため、slow readerテストも用意しています。

クライアント側は大量のデータをサーバーへ送信しますが、サーバーから返ってきたEchoレスポンスをすぐには読みません。

概念的には以下の状態を作ります。

```text
client application
      |
      | 大量にsend
      v
server
      |
      | echo response
      v
client kernel receive buffer
      |
      | applicationがreadしない
      v
Recv-Qが増加
      |
      v
TCP flow control
      |
      v
server側のwriteが詰まる
      |
      v
write() -> EAGAIN
      |
      v
EPOLLOUT待ち
```

通常の小さなEcho通信では `write()` がすぐ成功してしまうため、このテストによって `EPOLLOUT` 処理を発生させやすくしています。

## ssによるTCP状態観察

TCP接続状態は `ss` で確認できます。

```bash
ss -tan
```

主な列は以下です。

```text
State  Recv-Q  Send-Q  Local Address:Port  Peer Address:Port
```

### Recv-Q

`Recv-Q` は、

> カーネルがすでに受信しているが、アプリケーションがまだ `read()` していないデータ量

です。

slow readerテストでは、クライアントアプリケーションがEchoレスポンスを読まないため、クライアント側の `Recv-Q` が大きくなる様子を確認できます。

### Send-Q

`Send-Q` は、

> アプリケーションが `write()` したが、TCPスタック上でまだ完全に送信されていないデータ量

です。

さらに詳しいTCP情報を見る場合は、

```bash
ss -tin
```

を利用できます。

## straceによるsystem call観察

`strace` を使うと、イベントループ内部でどのsystem callが呼ばれているか確認できます。

```bash
strace -p $(pidof server) \
  -e trace=epoll_wait,accept4,read,write
```

例えば、1つのクライアントメッセージを処理すると、

```text
epoll_wait(...)
read(...)
write(...)
read(...) = -1 EAGAIN
epoll_wait(...)
```

のような流れを観察できます。

これはサーバー内部の、

```text
イベント待ち
   |
データをread
   |
Echoをwrite
   |
もう一度read
   |
EAGAIN
   |
epoll_waitへ戻る
```

という処理そのものです。

新規接続では、

```text
epoll_wait(...)
accept4(...)
accept4(...) = -1 EAGAIN
epoll_wait(...)
```

のようになります。

2回目の `accept4()` は意図した動作です。

ノンブロッキングlistenerに対して `EAGAIN` が返るまで `accept4()` を繰り返し、accept待ちキューを処理します。

## EINTR

system callはsignalによって中断される場合があります。

例えば、

```text
epoll_wait(...) = -1 EINTR
```

となることがあります。

この場合、サーバーを終了するのではなく、処理をリトライします。

## File Descriptorの再利用

file descriptor番号は永続的なクライアントIDではありません。

例えば、

```text
client A -> fd 5

close(fd 5)

client B -> fd 5
```

のように、closeされたfd番号は再利用されることがあります。

そのため、このサーバーではfd番号だけをクライアント識別子として使わ
