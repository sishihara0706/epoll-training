# epoll-training

Linux の `epoll`、ノンブロッキング I/O、TCP ストリーム処理、バックプレッシャーを C 言語で学ぶためのハンズオンプロジェクトです。

実装は改行区切りの TCP Echo Server です。単一の `epoll` イベントループで、リスナー、クライアントソケット、定期処理用の `timerfd`、終了処理用の `signalfd` を扱います。

[English version](README.md)

## 主な機能

- level-triggered な単一の `epoll` ループによる複数クライアント処理
- `socket()` と `accept4()` で作成する non-blocking／close-on-exec ソケット
- `epoll_event.data.ptr` を使ったクライアントごとの入力バッファと FIFO 出力キューの管理
- TCP の分割・結合された read に対応する改行ベースのメッセージ分割
- 順序を維持した partial-write-safe な `EPOLLOUT` 処理
- 出力キューの byte 数に基づく high/low-water read throttling
- `timerfd` による接続中クライアント数の定期表示
- `signalfd` によるイベントループ内でのシグナル処理
- `EPIPE` や connection reset を含むクライアント単位のエラー処理
- 通常負荷、slow reader、no reader のテストプログラム

## 最近の更新

元の README に反映されていなかった最近の変更は次のとおりです。

- 完全な入力行ごとにクライアント固有の linked-list 出力キューへコピーします。遅いクライアントに複数のレスポンスが滞留しても、メッセージの順序を維持できます。
- キュー内の byte 数とメッセージ数を追跡します。partial write が成功するたびに byte 数を減らし、送信を完了したキューノードはすぐに解放します。
- read-side throttling に hysteresis を導入しました。キュー内の未送信データが 64 KiB に達すると `EPOLLIN` を停止し、32 KiB 以下になると再開します。high-water mark 未満では、`EPOLLIN` と `EPOLLOUT` を同時に監視する場合があります。
- クライアントの切断またはエラー時に、残っている出力キューを解放します。
- 入力をクライアントごとに蓄積し、TCP の read 境界をメッセージ境界とみなさず、改行で区切られた完全なメッセージへ分割します。
- `SIGINT`、`SIGTERM`、`SIGQUIT` を block して `signalfd` から読み取り、終了処理も `epoll` イベントループ内で行うようになりました。
- build、run、clean の標準ターゲットを持つ `Makefile` を追加しました。
- 継続的なバックプレッシャーを意図的に起こす `no_reader_test.py` を追加しました。

## イベントループの構成

登録する各 descriptor は、先頭に共通のヘッダーを持ちます。

```c
struct fd_info {
    enum fd_type type;
    int fd;
};
```

`epoll_event.data.ptr` はこのヘッダー、または同じヘッダーを先頭メンバーに持つクライアント構造体を指します。イベントループは descriptor の種類によって処理を振り分けます。

```text
epoll_wait()
   |
   +-- listener / EPOLLIN
   |     EAGAIN まで accept4()
   |
   +-- client / EPOLLIN
   |     client の入力バッファへ read
   |     改行で区切られた完全なメッセージをすべて enqueue
   |     socket が許す範囲で出力キューを flush
   |
   +-- client / EPOLLOUT
   |     出力キューの flush を再開
   |
   +-- timerfd / EPOLLIN
   |     10秒ごとに接続中の client 数を表示
   |
   +-- signalfd / EPOLLIN
         終了 signal を読み取って shutdown
```

listener は `accept4()` が `EAGAIN` を返すまで処理します。クライアントの read／write も同様に、処理できなくなるまで繰り返してからイベントループへ戻ります。

## TCP のメッセージ分割

TCP は byte stream です。送信側の1回の `write()` と受信側の1回の `read()` が対応する保証はありません。例えば、

```text
alpha\n
beta\n
```

という2回の書き込みが1回の read にまとまることも、1行が複数の read に分割されることもあります。

そのため、各クライアントは入力バッファを持ちます。

```c
char in_buf[BUFF_SIZE];
size_t in_len;
```

サーバーは不完全な入力を保持して `\n` を探します。完全な各行を新しい FIFO 出力キューのノードへコピーし、残った不完全なデータは次の read のために入力バッファへ保持します。改行がないまま 4096 byte の入力バッファを使い切った場合はエラーとし、そのクライアントを切断します。

## Partial write とバックプレッシャー

non-blocking な `write()` は、レスポンスの一部だけを送信したり、`EAGAIN` を返したりします。キュー内の各メッセージがそれぞれの送信位置を保持し、クライアントはキュー全体とその合計量を管理します。

```c
struct st_outmsg {
    char buf[BUFF_SIZE];
    size_t len;
    size_t pos;
    struct st_outmsg *next;
};

struct st_outmsg *head;
struct st_outmsg *tail;
size_t queued_bytes;
size_t queued_messages;
int read_paused;
```

例えば、キュー先頭のメッセージが `len = 1000`、`pos = 600` なら、そのメッセージの残りは 400 byte です。write が成功するたびに `queued_bytes` を減らし、送信完了したノードをキュー先頭から取り除いて解放します。完全な行が複数待機していても、レスポンスの FIFO 順序を維持できます。

```text
完全な行を取得
        |
        v
メッセージを FIFO キューへ追加
        |
        v
キュー先頭から可能な範囲まで flush
        |
        +-- キューが空 ----------> EPOLLIN
        |
        +-- read が有効かつ
        |   64 KiB 未満 ----------> EPOLLIN | EPOLLOUT
        |
        +-- 64 KiB に到達 --------> EPOLLIN を停止
                                         |
                                         v
                                  EPOLLOUT で flush
                                         |
                              未送信データが 32 KiB 以下
                                         |
                                         v
                                   EPOLLIN を再開
```

high watermark と low watermark を分ける hysteresis によって、単一の閾値付近で `EPOLLIN` が頻繁に切り替わることを防ぎます。read の停止中は kernel の受信バッファと TCP flow control を通して送信元へ圧力を伝えます。キューが空になれば `EPOLLOUT` もすぐ解除するため、不要な writable 通知は発生し続けません。

## ビルドと実行

Linux、GCC または互換 C compiler、GNU Make が必要です。テストプログラムには Python 3 も使用します。

```bash
make
make run
```

サーバーは全 interface の TCP port `8080` で待ち受けます。

```text
listening on 8080
```

生成した binary を削除する場合は次を実行します。

```bash
make clean
```

直接コンパイルする場合の同等のコマンドは次のとおりです。

```bash
gcc -Wall -Wextra -Wpedantic -Og -g server.c -o server
```

`SO_REUSEADDR` を有効にしているため、以前の TCP 状態が残っていても通常はすぐにサーバーを再起動できます。

## 終了処理

`Ctrl-C`、`SIGTERM`、または `SIGQUIT` でサーバーを停止できます。

```bash
kill -TERM "$(pidof server)"
```

これらの signal は通常の配送経路では block され、non-blocking な `signalfd` から読み取られます。この descriptor も `epoll` に登録されているため、signal 処理は他のイベントと同じ流れで同期的に行われます。signal を受け取ると、listener、timer、signal、epoll の各 descriptor を閉じて終了します。

これはプロセスの制御された終了ですが、未送信のクライアントレスポンスを drain してから終了する実装ではありません。

## テスト

一方の terminal でサーバーを起動してから、別の terminal で以下を実行します。

### 手動 Echo テスト

```bash
nc 127.0.0.1 8080
```

改行で終わる各メッセージが1回ずつそのまま返ります。

### 同時接続テスト

```bash
./clients-connect.sh
```

100個の `nc` クライアントを同時に開き、それぞれ異なる時間だけ接続を維持します。

### Request／response 負荷試験

```bash
python3 load_test.py
```

デフォルトでは100クライアントを作り、クライアントごとに1000個の行単位リクエストを送信します。すべての Echo を待った後、経過時間と1秒あたりのメッセージ数を表示します。

### Slow reader テスト

```bash
python3 slow_reader_test.py
```

100クライアントが多数のメッセージを送信し、レスポンスを読まないまま5秒待ってから切断します。これによりサーバー側の `write()` が `EAGAIN` になりやすい状態を作ります。

### No reader テスト

```bash
python3 no_reader_test.py
```

1クライアントが100,000メッセージの送信を試み、Echo を一切読みません。リポジトリ内で最も強いバックプレッシャーテストで、TCP flow control がクライアントまで到達すると `sendall()` で block する場合があります。最後の sleep は観察のために接続を維持します。

各 Python ファイル内の workload 値を小さくすれば、短時間で試せます。

## サーバーの観察

TCP queue は次のコマンドで確認できます。

```bash
ss -tan
ss -tin
```

- `Recv-Q`: kernel が受信済みで、application がまだ読んでいないデータ
- `Send-Q`: application が書き込み済みで、TCP stack 上ではまだ完全に配送されていないデータ

主要な system call は次のように追跡できます。

```bash
strace -p "$(pidof server)" \
  -e trace=epoll_wait,epoll_ctl,accept4,read,write
```

バックプレッシャーテスト中は、次の server log が手掛かりになります。

```text
write EAGAIN fd=7 pos=600 len=1000
EPOLLOUT fd=7
```

最初の行は partial write の位置が保存されたことを示します。その後の `EPOLLOUT` event で、その位置から送信を再開します。

## エラー処理

```text
EINTR
    中断された処理を retry

EAGAIN / EWOULDBLOCK
    状態を保持して epoll_wait() へ戻る

EPIPE または ECONNRESET
    影響を受けた client だけを close

read() == 0
    peer が正常に shutdown したため、その client を close
```

`SIGPIPE` は無視し、socket write の失敗でプロセス全体が終了する代わりに `EPIPE` として扱います。クライアント固有のエラーでは接続数を減らし、サーバー自体は動作を続けます。

## 学習テーマ

- level-triggered `epoll`
- non-blocking socket と readiness-driven I/O
- `accept4()`、`timerfd`、`signalfd`
- TCP byte stream のメッセージ分割
- partial read／partial write
- `EAGAIN`、`EINTR`、`EPIPE`、connection reset の処理
- TCP の送受信 queue と flow control
- 接続ごとの状態管理と file descriptor の再利用
- `ss` と `strace` による実行時の観察

## 開発について

このサーバーは Linux systems programming の学習として段階的に実装し、負荷試験、`ss`、`strace` を使って挙動を確認してきました。Codex や ChatGPT などの AI coding assistant は、code review、debug の相談、edge case の検討、test 設計、document 作成の支援に使用しています。一度の生成で完成させたプロジェクトではありません。

## 現在の制約と今後の候補

- `EPOLLERR`、`EPOLLHUP`、`EPOLLRDHUP` の明示的な処理
- `QUEUE_MAX_BYTES` と `QUEUE_MAX_MESSAGES` による hard limit の適用。定数は定義済みですが、現在のキュー処理が行うのは high/low-water read throttling だけです
- graceful shutdown 時に接続中クライアントを drain
- address、port、buffer size、logging の設定可能化
- 自動 integration test
- 接続数や throughput の統計
- `select`、`poll`、edge-triggered `EPOLLET` との比較
- より大規模な latency／throughput 計測

このリポジトリは意図的に小さく直接的な構成にしています。Linux のイベント駆動 networking を、コード・実行・観察を通して理解することが目的です。
